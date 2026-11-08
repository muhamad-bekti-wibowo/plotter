// Menyesuaikan jalur ke area kerja (mm), mengurutkan, dan menghasilkan perintah PU/MV/PD.
#pragma once
#include <QRectF>
#include <QStringList>
#include "Types.h"

struct PlanOptions {
    double areaW = 100, areaH = 100;   // mm
    double margin = 5;                 // mm
    double scalePct = 100;             // 100 = pas di area (dikurangi margin)
    double offsetX = 0, offsetY = 0;   // geser (mm) dari posisi tengah
    bool keepSize = false;             // true: ukuran sumber (mm) dipertahankan x skala% (untuk teks), bukan dipaskan ke area
    bool flipY = true;                 // SVG y ke bawah -> plotter y ke atas (konversi standar)
    bool mirrorX = false;              // cermin kiri<->kanan
    bool mirrorY = false;              // cermin atas<->bawah (tambahan di atas flipY)
    bool optimize = true;              // urutan nearest-neighbor + balik arah
    double minSegment = 0.15;          // buang titik yang lebih dekat dari ini (mm)
};

struct PlanResult {
    Paths paths;        // dalam mm koordinat mesin (0..areaW, 0..areaH)
    double drawLength = 0, travelLength = 0;
    int points = 0;
};

PlanResult planPaths(const Paths& src, const PlanOptions& opt);
QStringList toCommands(const PlanResult& plan);
QRectF boundsOf(const Paths& p);
