#pragma once
#include <QWidget>
#include "Types.h"

class CanvasView : public QWidget {
    Q_OBJECT
public:
    explicit CanvasView(QWidget* parent = nullptr);
    void setArea(double w, double h);
    void setPaths(const Paths& p);
    void setGantry(double x, double y, bool pen, bool valid);
    void setProgress(double fraction);      // 0..1 bagian jalur yang sudah diplot (perkiraan per titik)

protected:
    void paintEvent(QPaintEvent*) override;
    QSize sizeHint() const override { return {520, 520}; }

private:
    double w_ = 100, h_ = 100, gx_ = 0, gy_ = 0, progress_ = 0;
    bool pen_ = false, gvalid_ = false;
    Paths paths_;
    int totalPts_ = 0;
};
