#include "ImageTracer.h"
#include <QHash>
#include <QSet>
#include <QSizeF>
#include <algorithm>
#include <cmath>

static void dp(const Polyline& p, int a, int b, double eps, QVector<bool>& keep) {
    if (b <= a + 1) return;
    double maxd = 0;
    int idx = -1;
    QPointF A = p[a], B = p[b];
    double dx = B.x() - A.x(), dy = B.y() - A.y(), len = std::hypot(dx, dy);
    for (int i = a + 1; i < b; i++) {
        double d = len < 1e-12 ? std::hypot(p[i].x() - A.x(), p[i].y() - A.y())
                               : std::abs(dy * p[i].x() - dx * p[i].y() + B.x() * A.y() - B.y() * A.x()) / len;
        if (d > maxd) { maxd = d; idx = i; }
    }
    if (idx >= 0 && maxd > eps) {
        keep[idx] = true;
        dp(p, a, idx, eps, keep);
        dp(p, idx, b, eps, keep);
    }
}

Polyline simplifyPolyline(const Polyline& p, double eps) {
    if (p.size() < 3) return p;
    QVector<bool> keep(p.size(), false);
    keep.first() = keep.last() = true;
    dp(p, 0, p.size() - 1, eps, keep);
    Polyline r;
    for (int i = 0; i < p.size(); i++) if (keep[i]) r << p[i];
    return r;
}

static double plen(const Polyline& p) {
    double l = 0;
    for (int i = 1; i < p.size(); i++) l += std::hypot(p[i].x() - p[i - 1].x(), p[i].y() - p[i - 1].y());
    return l;
}

Paths traceImage(const QImage& src, const TraceOptions& opt, QSizeF* size) {
    QImage img = src;
    if (std::max(img.width(), img.height()) > opt.maxDim)
        img = img.scaled(opt.maxDim, opt.maxDim, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    img = img.convertToFormat(QImage::Format_ARGB32);          // latar transparan dianggap putih (lihat alpha di bawah)
    const int W = img.width(), H = img.height();
    if (size) *size = QSizeF(W, H);
    QVector<uint8_t> dark(size_t(W) * H, 0);
    for (int y = 0; y < H; y++) {
        const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < W; x++) {
            QRgb c = row[x];
            double a = qAlpha(c) / 255.0;
            double g = (0.299 * qRed(c) + 0.587 * qGreen(c) + 0.114 * qBlue(c)) * a + 255.0 * (1 - a);
            bool d = g < opt.threshold;
            dark[size_t(y) * W + x] = (d != opt.invert) ? 1 : 0;
        }
    }
    auto at = [&](int x, int y) -> int { return (x < 0 || y < 0 || x >= W || y >= H) ? 0 : dark[size_t(y) * W + x]; };

    // Kunci titik tengah sisi sel dalam koordinat x2 (dua kali lipat) agar bilangan bulat.
    auto key = [&](int x2, int y2) -> qint64 { return qint64(x2 + 4) * (2 * H + 16) + (y2 + 4); };
    struct Adj { qint64 n[2]; int c = 0; };
    QHash<qint64, Adj> adj;
    auto link = [&](int ax, int ay, int bx, int by) {
        qint64 ka = key(ax, ay), kb = key(bx, by);
        Adj& A = adj[ka];
        if (A.c < 2) A.n[A.c++] = kb;
        Adj& B = adj[kb];
        if (B.c < 2) B.n[B.c++] = ka;
    };

    for (int j = -1; j < H; j++) {
        for (int i = -1; i < W; i++) {
            int tl = at(i, j), tr = at(i + 1, j), br = at(i + 1, j + 1), bl = at(i, j + 1);
            int c = tl * 8 + tr * 4 + br * 2 + bl;
            if (c == 0 || c == 15) continue;
            int Tx = 2 * i + 1, Ty = 2 * j, Rx = 2 * i + 2, Ry = 2 * j + 1, Bx = 2 * i + 1, By = 2 * j + 2, Lx = 2 * i, Ly = 2 * j + 1;
            switch (c) {
            case 1: case 14: link(Lx, Ly, Bx, By); break;
            case 2: case 13: link(Bx, By, Rx, Ry); break;
            case 3: case 12: link(Lx, Ly, Rx, Ry); break;
            case 4: case 11: link(Tx, Ty, Rx, Ry); break;
            case 5: link(Lx, Ly, Tx, Ty); link(Bx, By, Rx, Ry); break;
            case 6: case 9: link(Tx, Ty, Bx, By); break;
            case 7: case 8: link(Lx, Ly, Tx, Ty); break;
            case 10: link(Tx, Ty, Rx, Ry); link(Lx, Ly, Bx, By); break;
            }
        }
    }

    auto decode = [&](qint64 k) {
        qint64 x2 = k / (2 * H + 16) - 4, y2 = k % (2 * H + 16) - 4;
        return QPointF(x2 / 2.0, y2 / 2.0);
    };

    Paths out;
    QList<qint64> keys = adj.keys();
    for (qint64 k0 : keys) {
        auto it0 = adj.find(k0);
        if (it0 == adj.end() || it0->c == 0) continue;
        Polyline p;
        QVector<qint64> visited;
        QSet<qint64> seen;
        qint64 prev = -1, cur = k0;
        p << decode(cur);
        while (true) {
            auto it = adj.find(cur);
            if (it == adj.end()) break;
            visited << cur;
            seen.insert(cur);
            qint64 nxt = -1;
            for (int t = 0; t < it->c; t++) {
                qint64 n = it->n[t];
                if (n == prev) continue;
                if (n == k0 && visited.size() > 2) { nxt = n; break; }       // kontur kembali ke awal: tertutup
                if (adj.contains(n) && !seen.contains(n)) { nxt = n; break; }
            }
            if (nxt < 0) break;
            prev = cur;
            cur = nxt;
            p << decode(cur);
            if (cur == k0) break;
        }
        for (qint64 v : visited) adj.remove(v);
        if (plen(p) < opt.minLength) continue;
        Polyline s = simplifyPolyline(p, opt.simplify);
        if (s.size() >= 2) out << s;
    }
    return out;
}
