#include "PathPlanner.h"
#include <algorithm>
#include <cmath>

QRectF boundsOf(const Paths& p) {
    bool first = true;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    for (const Polyline& l : p)
        for (const QPointF& q : l) {
            if (first) { x0 = x1 = q.x(); y0 = y1 = q.y(); first = false; }
            x0 = std::min(x0, q.x()); x1 = std::max(x1, q.x());
            y0 = std::min(y0, q.y()); y1 = std::max(y1, q.y());
        }
    return QRectF(QPointF(x0, y0), QPointF(x1, y1));
}

static double dist(const QPointF& a, const QPointF& b) { return std::hypot(a.x() - b.x(), a.y() - b.y()); }

PlanResult planPaths(const Paths& src, const PlanOptions& o) {
    PlanResult r;
    QRectF b = boundsOf(src);
    if (src.isEmpty() || (b.width() <= 0 && b.height() <= 0)) return r;
    double availW = std::max(1.0, o.areaW - 2 * o.margin), availH = std::max(1.0, o.areaH - 2 * o.margin);
    double sx = b.width() > 0 ? availW / b.width() : 1e9, sy = b.height() > 0 ? availH / b.height() : 1e9;
    double s = o.keepSize ? o.scalePct / 100.0 : std::min(sx, sy) * o.scalePct / 100.0;
    double cx = b.center().x(), cy = b.center().y();
    double mx = o.areaW / 2 + o.offsetX, my = o.areaH / 2 + o.offsetY;

    Paths out;
    for (const Polyline& l : src) {
        Polyline p;
        for (const QPointF& q : l) {
            bool fy = o.flipY != o.mirrorY;                  // dua pembalikan saling meniadakan
            double x = mx + (o.mirrorX ? -(q.x() - cx) : (q.x() - cx)) * s;
            double y = my + (fy ? -(q.y() - cy) : (q.y() - cy)) * s;
            x = std::clamp(x, 0.0, o.areaW);
            y = std::clamp(y, 0.0, o.areaH);
            QPointF pt(x, y);
            if (p.isEmpty() || dist(p.last(), pt) >= o.minSegment) p << pt;
        }
        if (l.size() >= 2 && l.first() == l.last() && p.size() >= 2 && p.first() != p.last()) p << p.first();
        if (p.size() >= 2) out << p;
    }

    if (o.optimize && out.size() > 1) {
        Paths ordered;
        QVector<bool> used(out.size(), false);
        QPointF cur(0, 0);
        for (int n = 0; n < out.size(); n++) {
            int best = -1;
            bool rev = false;
            double bd = 1e300;
            for (int i = 0; i < out.size(); i++) {
                if (used[i]) continue;
                double d1 = dist(cur, out[i].first()), d2 = dist(cur, out[i].last());
                if (d1 < bd) { bd = d1; best = i; rev = false; }
                if (d2 < bd) { bd = d2; best = i; rev = true; }
            }
            used[best] = true;
            Polyline p = out[best];
            if (rev) std::reverse(p.begin(), p.end());
            cur = p.last();
            ordered << p;
        }
        out = ordered;
    }

    QPointF cur(0, 0);
    for (const Polyline& p : out) {
        r.travelLength += dist(cur, p.first());
        for (int i = 1; i < p.size(); i++) r.drawLength += dist(p[i - 1], p[i]);
        r.points += p.size();
        cur = p.last();
    }
    r.paths = out;
    return r;
}

QStringList toCommands(const PlanResult& plan) {
    QStringList c;
    auto mv = [](const QPointF& p) { return QString("MV %1 %2").arg(p.x(), 0, 'f', 3).arg(p.y(), 0, 'f', 3); };
    c << "PU";
    QPointF cur(-1e9, -1e9);
    bool down = false;
    for (const Polyline& p : plan.paths) {
        if (dist(cur, p.first()) > 0.05) {
            if (down) { c << "PU"; down = false; }
            c << mv(p.first());
        }
        if (!down) { c << "PD"; down = true; }
        for (int i = 1; i < p.size(); i++) c << mv(p[i]);
        cur = p.last();
    }
    c << "PU" << "MV 0 0";
    return c;
}
