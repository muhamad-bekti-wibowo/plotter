#include "TextRender.h"
#include <QFile>
#include <QFont>
#include <QFontMetricsF>
#include <QHash>
#include <QPainterPath>
#include <QPolygonF>
#include <QRegularExpression>
#include <QTransform>
#include <algorithm>
#include <cmath>

namespace {
struct Glyph { double left = 0, right = 0; Paths strokes; };
struct StrokeFont { QHash<int, Glyph> g; double cap = -12, base = 9; bool ok = false; };

StrokeFont& loadFont(const QString& id) {
    static QHash<QString, StrokeFont> cache;
    auto it = cache.find(id);
    if (it != cache.end()) return it.value();
    StrokeFont& f = cache[id];
    QFile file(":/fonts/" + id + ".jhf");
    if (!file.open(QIODevice::ReadOnly)) return f;
    const QList<QByteArray> lines = file.readAll().split('\n');
    int code = 32;
    for (QByteArray ln : lines) {
        while (ln.endsWith('\r')) ln.chop(1);
        if (ln.startsWith('#')) {
            static const QRegularExpression capRe("define_cap_line\"\\s*:\\s*(-?[\\d.]+)"), baseRe("define_base_line\"\\s*:\\s*(-?[\\d.]+)");
            const QString s = QString::fromLatin1(ln);
            if (auto m = capRe.match(s); m.hasMatch()) f.cap = m.captured(1).toDouble();
            if (auto m = baseRe.match(s); m.hasMatch()) f.base = m.captured(1).toDouble();
            continue;
        }
        if (ln.size() < 10) continue;
        const QByteArray d = ln.mid(8);
        Glyph g;
        g.left = d[0] - 'R';
        g.right = d[1] - 'R';
        Polyline cur;
        auto endStroke = [&] {
            if (cur.size() >= 2) g.strokes.push_back(cur);
            else if (cur.size() == 1) g.strokes.push_back(Polyline{cur[0], cur[0] + QPointF(0.01, 0)});   // titik
            cur.clear();
        };
        for (int i = 2; i + 1 < d.size(); i += 2) {
            if (d[i] == ' ' && d[i + 1] == 'R') { endStroke(); continue; }
            cur.push_back(QPointF(d[i] - 'R', d[i + 1] - 'R'));
        }
        endStroke();
        f.g.insert(code++, g);
    }
    f.ok = !f.g.isEmpty();
    return f;
}

QTransform rot(double deg) { QTransform t; t.rotate(-deg); return t; }   // y ke bawah: negatif = berlawanan arah jarum jam

Paths applyRotation(const Paths& p, double deg) {
    if (std::fabs(deg) < 1e-9) return p;
    const QTransform t = rot(deg);
    Paths out;
    for (const Polyline& l : p) {
        Polyline q;
        for (const QPointF& pt : l) q << t.map(pt);
        out << q;
    }
    return out;
}

// arsiran even-odd: poligon tertutup, garis sejajar sudut 'angle' berjarak 'sp'
Paths hatchFill(const QList<QPolygonF>& polys, double sp, double angle) {
    Paths out;
    if (sp <= 0.01) return out;
    const QTransform to = rot(-angle), back = rot(angle);   // putar sehingga garis arsir mendatar
    QList<QPolygonF> r;
    double minY = 1e18, maxY = -1e18;
    for (const QPolygonF& p : polys) {
        QPolygonF q = to.map(p);
        for (const QPointF& pt : q) { minY = std::min(minY, pt.y()); maxY = std::max(maxY, pt.y()); }
        r << q;
    }
    for (double y = minY + sp / 2; y < maxY; y += sp) {
        QVector<double> xs;
        for (const QPolygonF& p : r) {
            const int n = p.size();
            for (int i = 0; i < n; i++) {
                const QPointF a = p[i], b = p[(i + 1) % n];
                if ((a.y() <= y) == (b.y() <= y)) continue;
                xs << a.x() + (y - a.y()) * (b.x() - a.x()) / (b.y() - a.y());
            }
        }
        std::sort(xs.begin(), xs.end());
        for (int i = 0; i + 1 < xs.size(); i += 2)
            if (xs[i + 1] - xs[i] > 0.05) out.push_back(Polyline{back.map(QPointF(xs[i], y)), back.map(QPointF(xs[i + 1], y))});
    }
    return out;
}
}

QVector<StrokeFontInfo> strokeFonts() {
    return {{"futural", "Sans tipis (satu goresan)"},      {"futuram", "Sans sedang (satu goresan)"},
            {"rowmans", "Roman (satu goresan)"},           {"timesr", "Serif (satu goresan)"},
            {"timesrb", "Serif tebal (satu goresan)"},     {"scripts", "Script (satu goresan)"},
            {"cursive", "Tulisan sambung (satu goresan)"}, {"gothiceng", "Gothic (satu goresan)"}};
}

QString expandTextVars(const QString& text, int counter, const QDateTime& now) {
    static const QRegularExpression nRe("\\{n(?::(\\d+))?\\}", QRegularExpression::CaseInsensitiveOption);
    QString res;
    int last = 0;
    auto it = nRe.globalMatch(text);
    while (it.hasNext()) {
        auto m = it.next();
        res += text.mid(last, m.capturedStart() - last);
        const int w = m.captured(1).isEmpty() ? 0 : m.captured(1).toInt();
        res += QString::number(counter).rightJustified(w, '0');
        last = m.capturedEnd();
    }
    res += text.mid(last);
    res.replace("{tanggal}", now.toString("yyyy-MM-dd"), Qt::CaseInsensitive);
    res.replace("{date}", now.toString("yyyy-MM-dd"), Qt::CaseInsensitive);
    res.replace("{jam}", now.toString("HH:mm"), Qt::CaseInsensitive);
    res.replace("{time}", now.toString("HH:mm"), Qt::CaseInsensitive);
    return res;
}

Paths renderText(const TextOptions& o, QString* error) {
    Paths out;
    const QStringList lines = o.text.split('\n');
    const double h = std::max(0.5, o.height);

    if (o.singleStroke) {
        StrokeFont& f = loadFont(o.strokeFont);
        if (!f.ok) { if (error) *error = "Font goresan tidak ditemukan: " + o.strokeFont; return out; }
        const double s = h / (f.base - f.cap);
        const double advanceSpace = f.g.contains(32) ? f.g[32].right - f.g[32].left : 8;
        const int passes = std::clamp(o.strokePasses, 1, 3);
        Paths all;
        for (int li = 0; li < lines.size(); li++) {
            double x = 0;
            const double by = h + li * h * o.lineSpacing;
            Paths lp;
            for (const QChar ch : lines[li]) {
                auto git = f.g.constFind(int(ch.unicode()));
                if (git == f.g.constEnd()) { x += advanceSpace * s + o.letterSpacing; continue; }
                const Glyph& g = *git;
                for (const Polyline& st : g.strokes) {
                    Polyline p;
                    for (const QPointF& pt : st) p << QPointF(x + (pt.x() - g.left) * s, by + (pt.y() - f.base) * s);
                    lp << p;
                }
                x += (g.right - g.left) * s + o.letterSpacing;
            }
            if (!lines[li].isEmpty()) x -= o.letterSpacing;
            const double shift = o.align == 1 ? -x / 2 : o.align == 2 ? -x : 0;
            for (Polyline& p : lp) for (QPointF& pt : p) pt.rx() += shift;
            all += lp;
        }
        for (int k = 0; k < passes; k++) {
            const QPointF off = k == 0 ? QPointF(0, 0) : k == 1 ? QPointF(o.penWidth, 0) : QPointF(0, o.penWidth);
            for (const Polyline& p : all) {
                Polyline q;
                for (const QPointF& pt : p) q << pt + off;
                out << q;
            }
        }
        return applyRotation(out, o.rotation);
    }

    // font sistem (kontur)
    QFont font(o.family);
    font.setPixelSize(1000);
    font.setBold(o.bold);
    font.setItalic(o.italic);
    font.setStyleStrategy(QFont::PreferOutline);
    const QFontMetricsF fm(font);
    const double cap = fm.capHeight() > 1 ? fm.capHeight() : fm.ascent() * 0.7;
    const double s = h / cap;
    font.setLetterSpacing(QFont::AbsoluteSpacing, o.letterSpacing / s);
    const QFontMetricsF fm2(font);
    QPainterPath path;
    for (int li = 0; li < lines.size(); li++) {
        const double w = fm2.horizontalAdvance(lines[li]);
        const double x0 = o.align == 1 ? -w / 2 : o.align == 2 ? -w : 0;
        path.addText(x0, cap + li * cap * o.lineSpacing, font, lines[li]);
    }
    QTransform t;
    t.scale(s, s);
    const QList<QPolygonF> polys = path.toSubpathPolygons(t);
    for (const QPolygonF& p : polys) {
        Polyline l;
        for (const QPointF& pt : p) l << pt;
        if (l.size() >= 2) out << l;
    }
    if (o.hatch) out += hatchFill(polys, o.hatchSpacing, o.hatchAngle);
    return applyRotation(out, o.rotation);
}
