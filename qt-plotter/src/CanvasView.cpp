#include "CanvasView.h"
#include <QPainter>
#include <QPaintEvent>
#include <cmath>

CanvasView::CanvasView(QWidget* parent) : QWidget(parent) { setMinimumSize(300, 300); }

void CanvasView::setArea(double w, double h) { w_ = std::max(1.0, w); h_ = std::max(1.0, h); update(); }

void CanvasView::setPaths(const Paths& p) {
    paths_ = p;
    totalPts_ = 0;
    for (const auto& l : p) totalPts_ += l.size();
    progress_ = 0;
    update();
}

void CanvasView::setGantry(double x, double y, bool pen, bool valid) {
    gx_ = x; gy_ = y; pen_ = pen; gvalid_ = valid;
    update();
}

void CanvasView::setProgress(double f) { progress_ = std::clamp(f, 0.0, 1.0); update(); }

void CanvasView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), palette().window());
    const double m = 20;
    double s = std::min((width() - 2 * m) / w_, (height() - 2 * m) / h_);
    if (s <= 0) return;
    double ox = (width() - w_ * s) / 2, oy = (height() + h_ * s) / 2;   // y mesin ke atas
    auto map = [&](double x, double y) { return QPointF(ox + x * s, oy - y * s); };

    p.setPen(QPen(QColor(120, 120, 120), 1));
    p.setBrush(Qt::white);
    p.drawRect(QRectF(map(0, h_), map(w_, 0)));
    if (grid_ && gridCm_ > 0) {
        const double g = gridCm_ * 10;                                  // cm -> mm
        if (g * s >= 4) {                                               // jangan gambar kalau terlalu rapat di layar
            int i = 1;
            for (double x = g; x < w_ - 1e-6; x += g, i++) { p.setPen(i % 5 ? QColor(228, 228, 228) : QColor(195, 195, 195)); p.drawLine(map(x, 0), map(x, h_)); }
            i = 1;
            for (double y = g; y < h_ - 1e-6; y += g, i++) { p.setPen(i % 5 ? QColor(228, 228, 228) : QColor(195, 195, 195)); p.drawLine(map(0, y), map(w_, y)); }
            // label skala tiap garis tebal (5 kotak) bila cukup ruang
            if (g * s * 5 >= 40) {
                p.setPen(QColor(150, 150, 150));
                QFont f = p.font(); f.setPointSizeF(7.5); p.setFont(f);
                for (double x = g * 5; x < w_ - 1e-6; x += g * 5) p.drawText(map(x, 0) + QPointF(2, -3), QString::number(x / 10) + " cm");
                for (double y = g * 5; y < h_ - 1e-6; y += g * 5) p.drawText(map(0, y) + QPointF(2, -2), QString::number(y / 10) + " cm");
                f.setPointSizeF(9); p.setFont(f);
            }
        }
    }

    int done = int(progress_ * totalPts_), seen = 0;
    for (const Polyline& l : paths_) {
        QPolygonF poly;
        for (const QPointF& q : l) poly << map(q.x(), q.y());
        bool finished = (seen + l.size()) <= done;
        p.setPen(QPen(finished ? QColor(30, 140, 60) : QColor(20, 20, 20), 1.2));
        p.setBrush(Qt::NoBrush);
        p.drawPolyline(poly);
        seen += l.size();
    }
    p.setPen(QColor(90, 90, 90));
    p.drawText(QPointF(ox, oy + 14), QString("0,0"));
    p.drawText(QPointF(ox + w_ * s - 60, oy + 14), QString("%1 x %2 mm").arg(w_).arg(h_));

    if (gvalid_) {
        QPointF g = map(gx_, gy_);
        p.setPen(QPen(pen_ ? QColor(220, 30, 30) : QColor(30, 90, 220), 2));
        p.drawLine(g + QPointF(-8, 0), g + QPointF(8, 0));
        p.drawLine(g + QPointF(0, -8), g + QPointF(0, 8));
        p.drawEllipse(g, 5, 5);
    }
}

void CanvasView::setGrid(bool on, double stepCm) {
    grid_ = on;
    gridCm_ = stepCm;
    update();
}
