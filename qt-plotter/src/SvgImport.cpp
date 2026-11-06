#include "SvgImport.h"
#include <QRectF>
#include <algorithm>
#include <QRegularExpression>
#include <QStack>
#include <QTransform>
#include <QXmlStreamReader>
#include <cmath>

namespace {

struct Parser {
    Paths out;
    double step = 1.0;       // panjang segmen perataan kurva (satuan SVG)
    bool unsupportedText = false;

    // ---- angka
    static bool isNumStart(QChar c) { return c.isDigit() || c == '-' || c == '+' || c == '.'; }

    // ---- transform
    static QTransform parseTransform(const QString& s) {
        static const QRegularExpression re(R"((matrix|translate|scale|rotate|skewX|skewY)\s*\(([^)]*)\))");
        QTransform total;
        auto it = re.globalMatch(s);
        while (it.hasNext()) {
            auto m = it.next();
            QString name = m.captured(1);
            QVector<double> a;
            for (const QString& p : m.captured(2).split(QRegularExpression("[\\s,]+"), Qt::SkipEmptyParts)) a << p.toDouble();
            QTransform t;
            if (name == "matrix" && a.size() == 6) t = QTransform(a[0], a[1], a[2], a[3], a[4], a[5]);
            else if (name == "translate" && a.size() >= 1) t = QTransform::fromTranslate(a[0], a.size() > 1 ? a[1] : 0);
            else if (name == "scale" && a.size() >= 1) t = QTransform::fromScale(a[0], a.size() > 1 ? a[1] : a[0]);
            else if (name == "rotate" && a.size() >= 1) {
                t = QTransform();
                if (a.size() == 3) t.translate(a[1], a[2]);
                t.rotate(a[0]);
                if (a.size() == 3) t.translate(-a[1], -a[2]);
            } else if (name == "skewX" && a.size() == 1) t = QTransform(1, 0, std::tan(a[0] * M_PI / 180), 1, 0, 0);
            else if (name == "skewY" && a.size() == 1) t = QTransform(1, std::tan(a[0] * M_PI / 180), 0, 1, 0, 0);
            total = t * total;    // SVG: urutan kiri->kanan = terluar->terdalam
        }
        return total;
    }

    void emitPoly(Polyline p, const QTransform& tf, bool close) {
        if (p.size() < 2) return;
        if (close && p.first() != p.last()) p << p.first();
        for (QPointF& q : p) q = tf.map(q);
        out << p;
    }

    // ---- kurva
    static QPointF cubic(const QPointF& a, const QPointF& b, const QPointF& c, const QPointF& d, double t) {
        double u = 1 - t;
        return a * (u * u * u) + b * (3 * u * u * t) + c * (3 * u * t * t) + d * (t * t * t);
    }
    int segs(double len) const { return std::clamp(int(std::ceil(len / step)), 4, 200); }

    void addCubic(Polyline& pl, QPointF a, QPointF b, QPointF c, QPointF d) {
        auto L = [](QPointF p, QPointF q) { return std::hypot(p.x() - q.x(), p.y() - q.y()); };
        int n = segs(L(a, b) + L(b, c) + L(c, d));
        for (int i = 1; i <= n; i++) pl << cubic(a, b, c, d, double(i) / n);
    }

    void addArc(Polyline& pl, QPointF p0, double rx, double ry, double phiDeg, bool large, bool sweep, QPointF p1) {
        if (p0 == p1) return;
        rx = std::abs(rx);
        ry = std::abs(ry);
        if (rx < 1e-9 || ry < 1e-9) { pl << p1; return; }
        double phi = phiDeg * M_PI / 180, cp = std::cos(phi), sp = std::sin(phi);
        double dx = (p0.x() - p1.x()) / 2, dy = (p0.y() - p1.y()) / 2;
        double x1 = cp * dx + sp * dy, y1 = -sp * dx + cp * dy;
        double lam = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
        if (lam > 1) { rx *= std::sqrt(lam); ry *= std::sqrt(lam); }
        double num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
        double den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
        double co = (den == 0) ? 0 : std::sqrt(std::max(0.0, num / den));
        if (large == sweep) co = -co;
        double cxp = co * rx * y1 / ry, cyp = -co * ry * x1 / rx;
        double cx = cp * cxp - sp * cyp + (p0.x() + p1.x()) / 2, cy = sp * cxp + cp * cyp + (p0.y() + p1.y()) / 2;
        auto ang = [](double ux, double uy, double vx, double vy) {
            double a = std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);
            return a;
        };
        double th1 = ang(1, 0, (x1 - cxp) / rx, (y1 - cyp) / ry);
        double dth = ang((x1 - cxp) / rx, (y1 - cyp) / ry, (-x1 - cxp) / rx, (-y1 - cyp) / ry);
        if (!sweep && dth > 0) dth -= 2 * M_PI;
        else if (sweep && dth < 0) dth += 2 * M_PI;
        int n = segs(std::abs(dth) * std::max(rx, ry));
        for (int i = 1; i <= n; i++) {
            double t = th1 + dth * i / n;
            double ex = rx * std::cos(t), ey = ry * std::sin(t);
            pl << QPointF(cp * ex - sp * ey + cx, sp * ex + cp * ey + cy);
        }
        pl.last() = p1;
    }

    // ---- data path
    void parsePath(const QString& d, const QTransform& tf) {
        int i = 0, n = d.size();
        auto skip = [&] { while (i < n && (d[i].isSpace() || d[i] == ',')) i++; };
        auto num = [&](double* v) {
            skip();
            if (i >= n || !isNumStart(d[i])) return false;
            int s = i;
            if (d[i] == '-' || d[i] == '+') i++;
            while (i < n && d[i].isDigit()) i++;
            if (i < n && d[i] == '.') { i++; while (i < n && d[i].isDigit()) i++; }
            if (i < n && (d[i] == 'e' || d[i] == 'E') && i + 1 < n && (d[i + 1].isDigit() || d[i + 1] == '-' || d[i + 1] == '+')) {
                i += 2;
                while (i < n && d[i].isDigit()) i++;
            }
            bool ok;
            *v = d.mid(s, i - s).toDouble(&ok);
            return ok;
        };
        auto flag = [&](bool* f) {
            skip();
            if (i < n && (d[i] == '0' || d[i] == '1')) { *f = d[i] == '1'; i++; return true; }
            return false;
        };
        Polyline pl;
        QPointF cur, start, lastCtl;
        QChar cmd, prev;
        auto flush = [&] { if (pl.size() >= 2) emitPoly(pl, tf, false); pl.clear(); };
        while (true) {
            skip();
            if (i >= n) break;
            if (d[i].isLetter()) { cmd = d[i++]; }
            else if (cmd.isNull()) break;
            else if (cmd == 'M') cmd = 'L';
            else if (cmd == 'm') cmd = 'l';
            bool rel = cmd.isLower();
            QChar C = cmd.toUpper();
            double a[7];
            auto rd = [&](int k) { for (int j = 0; j < k; j++) if (!num(&a[j])) return false; return true; };
            if (C == 'Z') {
                if (!pl.isEmpty()) { pl << start; flush(); }
                cur = start;
                pl << cur;
                prev = 'Z';
                continue;
            }
            if (C == 'M') {
                if (!rd(2)) break;
                flush();
                cur = rel ? cur + QPointF(a[0], a[1]) : QPointF(a[0], a[1]);
                start = cur;
                pl << cur;
            } else if (C == 'L') {
                if (!rd(2)) break;
                cur = rel ? cur + QPointF(a[0], a[1]) : QPointF(a[0], a[1]);
                if (pl.isEmpty()) pl << start;
                pl << cur;
            } else if (C == 'H') {
                if (!rd(1)) break;
                cur.setX(rel ? cur.x() + a[0] : a[0]);
                if (pl.isEmpty()) pl << start;
                pl << cur;
            } else if (C == 'V') {
                if (!rd(1)) break;
                cur.setY(rel ? cur.y() + a[0] : a[0]);
                if (pl.isEmpty()) pl << start;
                pl << cur;
            } else if (C == 'C' || C == 'S') {
                QPointF c1, c2, e;
                if (C == 'C') {
                    if (!rd(6)) break;
                    QPointF o = rel ? cur : QPointF();
                    c1 = o + QPointF(a[0], a[1]); c2 = o + QPointF(a[2], a[3]); e = o + QPointF(a[4], a[5]);
                } else {
                    if (!rd(4)) break;
                    QPointF o = rel ? cur : QPointF();
                    c1 = (prev == 'C' || prev == 'S') ? 2 * cur - lastCtl : cur;
                    c2 = o + QPointF(a[0], a[1]); e = o + QPointF(a[2], a[3]);
                }
                if (pl.isEmpty()) pl << start;
                addCubic(pl, cur, c1, c2, e);
                lastCtl = c2; cur = e;
            } else if (C == 'Q' || C == 'T') {
                QPointF c, e;
                if (C == 'Q') {
                    if (!rd(4)) break;
                    QPointF o = rel ? cur : QPointF();
                    c = o + QPointF(a[0], a[1]); e = o + QPointF(a[2], a[3]);
                } else {
                    if (!rd(2)) break;
                    QPointF o = rel ? cur : QPointF();
                    c = (prev == 'Q' || prev == 'T') ? 2 * cur - lastCtl : cur;
                    e = o + QPointF(a[0], a[1]);
                }
                if (pl.isEmpty()) pl << start;
                addCubic(pl, cur, cur + (c - cur) * (2.0 / 3), e + (c - e) * (2.0 / 3), e);
                lastCtl = c; cur = e;
            } else if (C == 'A') {
                bool fl, fs;
                if (!num(&a[0]) || !num(&a[1]) || !num(&a[2]) || !flag(&fl) || !flag(&fs) || !num(&a[5]) || !num(&a[6])) break;
                QPointF e = rel ? cur + QPointF(a[5], a[6]) : QPointF(a[5], a[6]);
                if (pl.isEmpty()) pl << start;
                addArc(pl, cur, a[0], a[1], a[2], fl, fs, e);
                cur = e;
            } else break;
            prev = C;
        }
        flush();
    }

    static double attrNum(const QXmlStreamAttributes& at, const char* n, double def = 0) {
        QString v = at.value(n).toString();
        if (v.isEmpty()) return def;
        return v.remove(QRegularExpression("[a-zA-Z%]+$")).toDouble();
    }

    void shape(const QString& name, const QXmlStreamAttributes& at, const QTransform& tf) {
        if (name == "path") parsePath(at.value("d").toString(), tf);
        else if (name == "line") emitPoly({QPointF(attrNum(at, "x1"), attrNum(at, "y1")), QPointF(attrNum(at, "x2"), attrNum(at, "y2"))}, tf, false);
        else if (name == "polyline" || name == "polygon") {
            Polyline p;
            QStringList v = at.value("points").toString().split(QRegularExpression("[\\s,]+"), Qt::SkipEmptyParts);
            for (int i = 0; i + 1 < v.size(); i += 2) p << QPointF(v[i].toDouble(), v[i + 1].toDouble());
            emitPoly(p, tf, name == "polygon");
        } else if (name == "rect") {
            double x = attrNum(at, "x"), y = attrNum(at, "y"), w = attrNum(at, "width"), h = attrNum(at, "height");
            if (w > 0 && h > 0) emitPoly({{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}}, tf, true);
        } else if (name == "circle" || name == "ellipse") {
            double cx = attrNum(at, "cx"), cy = attrNum(at, "cy");
            double rx = name == "circle" ? attrNum(at, "r") : attrNum(at, "rx");
            double ry = name == "circle" ? rx : attrNum(at, "ry");
            if (rx <= 0 || ry <= 0) return;
            int n = segs(2 * M_PI * std::max(rx, ry));
            Polyline p;
            for (int i = 0; i <= n; i++) p << QPointF(cx + rx * std::cos(2 * M_PI * i / n), cy + ry * std::sin(2 * M_PI * i / n));
            emitPoly(p, tf, false);
        } else if (name == "text") unsupportedText = true;
    }
};

}  // namespace

SvgResult parseSvg(const QByteArray& data) {
    SvgResult res;
    // Pass 1: ukuran dokumen untuk menentukan ketelitian perataan kurva
    double diag = 1000;
    {
        QXmlStreamReader x(data);
        while (!x.atEnd()) {
            x.readNext();
            if (x.isStartElement() && x.name() == QLatin1String("svg")) {
                QStringList vb = x.attributes().value("viewBox").toString().split(QRegularExpression("[\\s,]+"), Qt::SkipEmptyParts);
                if (vb.size() == 4) diag = std::hypot(vb[2].toDouble(), vb[3].toDouble());
                else {
                    double w = Parser::attrNum(x.attributes(), "width"), h = Parser::attrNum(x.attributes(), "height");
                    if (w > 0 && h > 0) diag = std::hypot(w, h);
                }
                break;
            }
        }
    }
    Parser P;
    P.step = std::max(diag / 800.0, 1e-6);

    QXmlStreamReader x(data);
    QStack<QTransform> st;
    QStack<bool> skipStack;
    int skipDepth = 0;
    st.push(QTransform());
    while (!x.atEnd()) {
        x.readNext();
        if (x.isStartElement()) {
            QString nm = x.name().toString();
            QXmlStreamAttributes at = x.attributes();
            QTransform tf = Parser::parseTransform(at.value("transform").toString()) * st.top();
            bool hidden = nm == "defs" || nm == "clipPath" || nm == "mask" || nm == "symbol" || nm == "marker" || nm == "pattern" || nm == "metadata" ||
                          at.value("display").toString() == "none";
            if (skipDepth > 0 || hidden) skipDepth++;
            else P.shape(nm, at, tf);
            st.push(tf);
        } else if (x.isEndElement()) {
            if (st.size() > 1) st.pop();
            if (skipDepth > 0) skipDepth--;
        }
    }
    if (x.hasError()) res.error = "XML tidak valid: " + x.errorString();
    res.paths = P.out;
    if (P.unsupportedText) res.warning = "Elemen <text> diabaikan (ubah ke path/outline dulu).";
    if (res.paths.isEmpty() && res.error.isEmpty()) res.error = "Tidak ada bentuk yang bisa digambar di SVG ini.";
    return res;
}

QString pathsToSvg(const Paths& paths, const QRectF& b) {
    QString s;
    s += QString("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"%1 %2 %3 %4\" width=\"%3\" height=\"%4\">\n")
             .arg(b.x(), 0, 'f', 2).arg(b.y(), 0, 'f', 2).arg(b.width(), 0, 'f', 2).arg(b.height(), 0, 'f', 2);
    s += "<path fill=\"none\" stroke=\"#000\" stroke-width=\"1\" stroke-linejoin=\"round\" d=\"";
    for (const Polyline& p : paths) {
        if (p.size() < 2) continue;
        for (int i = 0; i < p.size(); i++) s += QString("%1%2 %3").arg(i ? "L" : "M").arg(p[i].x(), 0, 'f', 2).arg(p[i].y(), 0, 'f', 2);
        s += ' ';
    }
    s += "\"/>\n</svg>\n";
    return s;
}
