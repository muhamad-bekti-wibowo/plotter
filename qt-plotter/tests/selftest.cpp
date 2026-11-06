// Uji logika tanpa hardware: parser SVG, tracer raster, planner jalur, SVG writer.
#include <QCoreApplication>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <cstdio>
#include "../src/GcodeImport.h"
#include "../src/ImageTracer.h"
#include "../src/PathPlanner.h"
#include "../src/SvgImport.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("GAGAL: %s (baris %d)\n", #c, __LINE__); fails++; } else std::printf("ok: %s\n", #c); } while (0)

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::puts("main");
    QCoreApplication app(argc, argv);
    std::puts("app ok");

    const char* svg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect x="10" y="10" width="30" height="20"/>
      <g transform="translate(50,50) scale(2)"><circle cx="0" cy="0" r="10"/></g>
      <path d="M 10 80 L 30 80 C 30 60 50 60 50 80 A 10 10 0 0 1 70 80 Z"/>
      <path d="m70,10 h10 v10 h-10 z"/>
      <line x1="0" y1="0" x2="100" y2="100"/>
      <polygon points="80,80 90,80 85,90"/>
    </svg>)SVG";
    std::puts("start parse");
    SvgResult r = parseSvg(svg);
    std::puts("parsed");
    CHECK(r.error.isEmpty());
    CHECK(r.paths.size() == 6);
    QRectF b = boundsOf(r.paths);
    CHECK(b.left() >= -0.01 && b.right() <= 100.01);
    // lingkaran r=10 di-scale 2 => jari-jari 20 di (50,50): bounds 30..70
    bool foundCircle = false;
    for (const Polyline& p : r.paths) {
        QRectF pb = boundsOf({p});
        if (std::abs(pb.width() - 40) < 0.5 && std::abs(pb.height() - 40) < 0.5 && std::abs(pb.center().x() - 50) < 0.5) foundCircle = true;
    }
    CHECK(foundCircle);

    // raster: lingkaran hitam terisi -> satu kontur tertutup
    QImage img(200, 200, QImage::Format_RGB32);
    img.fill(Qt::white);
    {
        QPainter p(&img);
        p.setBrush(Qt::black);
        p.setPen(Qt::NoPen);
        p.drawEllipse(QPointF(100, 100), 60, 60);
    }
    QSizeF sz;
    Paths t = traceImage(img, TraceOptions{}, &sz);
    CHECK(t.size() == 1);
    CHECK(t.size() == 1 && t[0].first() == t[0].last());
    QRectF tb = boundsOf(t);
    CHECK(std::abs(tb.width() - 120) < 3);
    std::printf("kontur titik: %d\n", int(t.isEmpty() ? 0 : t[0].size()));

    // planner
    PlanOptions po;
    po.areaW = 200; po.areaH = 100; po.margin = 10;
    PlanResult pr = planPaths(r.paths, po);
    QRectF pb = boundsOf(pr.paths);
    CHECK(pb.left() >= 9.99 && pb.right() <= 190.01 && pb.top() >= 9.99 && pb.bottom() <= 90.01);
    QStringList cmds = toCommands(pr);
    CHECK(cmds.first() == "PU" && cmds.last() == "MV 0 0");
    int pd = cmds.count("PD"), pu = cmds.count("PU");
    CHECK(pd >= 1 && pu >= pd);
    std::printf("%d perintah, PD=%d PU=%d, garis=%.1f mm\n", int(cmds.size()), pd, pu, pr.drawLength);

    // SVG writer round trip
    QString out = pathsToSvg(t, QRectF(0, 0, sz.width(), sz.height()));
    SvgResult rr = parseSvg(out.toUtf8());
    CHECK(rr.error.isEmpty() && !rr.paths.isEmpty());

    // G-code: kotak 20x10 dengan pena Z, lalu busur setengah lingkaran G2, dan mode tanpa pena
    GcodeResult g = parseGcode("G21 G90\nG0 X0 Y0\nG1 Z-1\nG1 X20\nG1 Y10\nG1 X0\nG1 Y0\nG0 Z5\nG0 X30 Y0\nG1 Z-1\nG2 X50 Y0 I10 J0\nG0 Z5\n");
    CHECK(g.error.isEmpty());
    CHECK(g.paths.size() == 2);
    CHECK(std::abs(g.draw - (60 + 31.4159)) < 1.0);
    QRectF gb = boundsOf(g.paths);
    CHECK(std::abs(gb.width() - 50) < 0.1 && std::abs(gb.height() - 10) < 0.2);
    GcodeResult g2 = parseGcode("G0 X0 Y0\nG1 X10 Y0\nG1 X10 Y10\nG0 X50 Y50\nG1 X60 Y50\n");
    CHECK(g2.paths.size() == 2);
    GcodeResult g3 = parseGcode("G20\nG0 X0 Y0\nM3\nG1 X1 Y0\nM5\n");
    CHECK(g3.paths.size() == 1 && std::abs(g3.draw - 25.4) < 0.01);

    std::printf(fails ? "\n%d GAGAL\n" : "\nSEMUA OK\n", fails);
    return fails ? 1 : 0;
}
