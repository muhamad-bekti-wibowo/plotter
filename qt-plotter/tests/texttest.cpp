// Uji font sistem (butuh QGuiApplication): QT_QPA_PLATFORM=offscreen
#include <QGuiApplication>
#include <QFontDatabase>
#include <QImage>
#include <QPainter>
#include <cstdio>
#include "../src/PathPlanner.h"
#include "../src/TextRender.h"
int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QFontDatabase::addApplicationFont("C:/Windows/Fonts/arial.ttf");   // platform offscreen tidak punya font sistem
    TextOptions o;
    o.singleStroke = false;
    o.text = "HO";
    o.height = 20;
    Paths p = renderText(o);
    QRectF b = boundsOf(p);
    std::printf("kontur: %d jalur, %.1f x %.1f mm\n", int(p.size()), b.width(), b.height());
    bool ok = !p.isEmpty() && b.height() > 17 && b.height() < 23;
    o.hatch = true;
    Paths q = renderText(o);
    std::printf("arsir: %d jalur (kontur %d)\n", int(q.size()), int(p.size()));
    ok = ok && q.size() > p.size() + 10;
    o.rotation = 90;
    QRectF r = boundsOf(renderText(o));
    std::printf("putar 90: %.1f x %.1f\n", r.width(), r.height());
    ok = ok && r.height() > r.width() * 0.4 && r.height() > 30;
    if (argc > 1) {                      // gambar contoh untuk dilihat: kontur, arsir, goresan
        QImage img(900, 520, QImage::Format_RGB32);
        img.fill(Qt::white);
        QPainter pt(&img);
        pt.setPen(QPen(Qt::black, 1.2));
        auto draw = [&](const Paths& ps, double ox, double oy) {
            for (const Polyline& l : ps) for (int i = 1; i < l.size(); i++) pt.drawLine(QPointF(ox + l[i - 1].x() * 4, oy + l[i - 1].y() * 4), QPointF(ox + l[i].x() * 4, oy + l[i].y() * 4));
        };
        TextOptions a;
        a.singleStroke = false; a.text = "Plotter 123"; a.height = 20; a.family = "Arial";
        draw(renderText(a), 10, 10);
        a.hatch = true; a.hatchSpacing = 0.8;
        draw(renderText(a), 10, 120);
        TextOptions b;
        b.strokeFont = "scripts"; b.height = 16; b.align = 1;
        b.text = "Plotter 123\nHalo dunia";
        draw(renderText(b), 10, 230);
        b.strokeFont = "futural"; b.align = 0; b.strokePasses = 2;
        draw(renderText(b), 450, 230);
        pt.end();
        img.save(argv[1]);
    }
    std::puts(ok ? "TEKS OK" : "TEKS GAGAL");
    return ok ? 0 : 1;
}
