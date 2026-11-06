#include "GcodeImport.h"
#include <QRegularExpression>
#include <QSet>
#include <cmath>

namespace {
const double kPi = 3.14159265358979323846;

struct Word { QChar letter; double value; };

QVector<Word> words(QString line) {
    // buang komentar (...) dan ;...
    line.remove(QRegularExpression(R"(\([^)]*\))"));
    const int sc = line.indexOf(';');
    if (sc >= 0) line.truncate(sc);
    QVector<Word> w;
    static const QRegularExpression re(R"(([A-Za-z])\s*([-+]?(?:\d+\.?\d*|\.\d+)))");
    auto it = re.globalMatch(line);
    while (it.hasNext()) {
        auto m = it.next();
        w.push_back({m.captured(1).at(0).toUpper(), m.captured(2).toDouble()});
    }
    return w;
}
}

GcodeResult parseGcode(const QByteArray& data) {
    GcodeResult r;
    const QStringList lines = QString::fromUtf8(data).split('\n');

    // lihat dulu apakah pena dikendalikan oleh Z atau M3/M5
    bool usesZ = false, usesM = false;
    for (const QString& ln : lines)
        for (const Word& w : words(ln)) {
            if (w.letter == 'Z') usesZ = true;
            if (w.letter == 'M' && (int(w.value) == 3 || int(w.value) == 4 || int(w.value) == 5)) usesM = true;
        }

    double x = 0, y = 0, z = 0, scale = 1;
    bool absolute = true;
    int mode = 0;                 // gerak modal terakhir: 0,1,2,3
    bool penDown = !usesZ && !usesM;   // tanpa kendali pena: G1 = gambar
    bool unknown = false;
    Polyline cur;
    auto flush = [&] {
        if (cur.size() >= 2) r.paths.push_back(cur);
        cur.clear();
    };
    auto line = [&](double nx, double ny, bool draw) {
        const double d = std::hypot(nx - x, ny - y);
        if (draw) {
            if (cur.isEmpty()) cur.push_back(QPointF(x, -y));
            cur.push_back(QPointF(nx, -ny));
            r.draw += d;
        } else {
            flush();
            r.travel += d;
        }
        x = nx; y = ny;
    };

    for (const QString& ln : lines) {
        const QVector<Word> w = words(ln);
        if (w.isEmpty()) continue;
        r.lines++;
        double nx = x, ny = y, nz = z, I = 0, J = 0, R = 0;
        bool hasX = false, hasY = false, hasZ = false, hasIJ = false, hasR = false, moveWord = false;
        for (const Word& t : w) {
            const int n = int(std::lround(t.value));
            if (t.letter == 'G') {
                if (n == 0 || n == 1 || n == 2 || n == 3) { mode = n; moveWord = true; }
                else if (n == 20) scale = 25.4;
                else if (n == 21) scale = 1;
                else if (n == 90) absolute = true;
                else if (n == 91) absolute = false;
                else if (n == 28 || n == 92 || n == 17 || n == 54 || n == 94 || n == 4 || n == 21) {}
                else unknown = true;
            } else if (t.letter == 'M') {
                if (n == 3 || n == 4) { flush(); penDown = true; }
                else if (n == 5) { flush(); penDown = false; }
            } else if (t.letter == 'X') { nx = absolute ? t.value * scale : x + t.value * scale; hasX = true; }
            else if (t.letter == 'Y') { ny = absolute ? t.value * scale : y + t.value * scale; hasY = true; }
            else if (t.letter == 'Z') { nz = absolute ? t.value * scale : z + t.value * scale; hasZ = true; }
            else if (t.letter == 'I') { I = t.value * scale; hasIJ = true; }
            else if (t.letter == 'J') { J = t.value * scale; hasIJ = true; }
            else if (t.letter == 'R') { R = t.value * scale; hasR = true; }
        }
        (void)moveWord;
        if (hasZ) {
            z = nz;
            if (usesZ) {
                const bool down = z <= 0.0;
                if (down != penDown) { flush(); penDown = down; }
            }
        }
        if (!hasX && !hasY) continue;
        if (mode == 0 || mode == 1) {
            // tanpa kendali pena: G0 = perjalanan, G1 = gambar. Dengan kendali pena: ikut keadaan pena (G0 saat turun tetap menggambar)
            const bool draw = (usesZ || usesM) ? penDown : (mode == 1);
            line(nx, ny, draw && (mode == 1 || usesZ || usesM));
        } else {
            // busur: G2 searah jarum jam, G3 berlawanan
            double cx, cy;
            if (hasIJ) { cx = x + I; cy = y + J; }
            else if (hasR) {
                const double dx = nx - x, dy = ny - y, d = std::hypot(dx, dy);
                if (d < 1e-9) continue;
                double h2 = R * R - d * d / 4;
                if (h2 < 0) h2 = 0;
                const double h = std::sqrt(h2) * ((mode == 3) == (R > 0) ? 1 : -1);
                cx = x + dx / 2 - dy / d * h;
                cy = y + dy / 2 + dx / d * h;
            } else { line(nx, ny, penDown); continue; }
            const double rad = std::hypot(x - cx, y - cy);
            double a0 = std::atan2(y - cy, x - cx), a1 = std::atan2(ny - cy, nx - cx);
            if (mode == 3) { while (a1 <= a0) a1 += 2 * kPi; }
            else { while (a1 >= a0) a1 -= 2 * kPi; }
            if (std::fabs(nx - x) < 1e-9 && std::fabs(ny - y) < 1e-9) a1 = a0 + (mode == 3 ? 2 * kPi : -2 * kPi);
            const int segs = std::max(4, int(std::fabs(a1 - a0) * rad / 0.5));   // titik tiap ~0,5 mm
            const bool draw = (usesZ || usesM) ? penDown : true;
            const double sx = x, sy = y;
            (void)sx; (void)sy;
            for (int i = 1; i <= segs; i++) {
                const double a = a0 + (a1 - a0) * i / segs;
                line(i == segs ? nx : cx + rad * std::cos(a), i == segs ? ny : cy + rad * std::sin(a), draw);
            }
        }
    }
    flush();
    if (unknown) r.warning = "Ada perintah G yang tidak didukung (diabaikan).";
    if (r.paths.isEmpty()) r.error = "Tidak ada gerakan menggambar di file G-code ini.";
    return r;
}
