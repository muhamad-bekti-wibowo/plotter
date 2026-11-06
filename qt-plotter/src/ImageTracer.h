// Raster -> kontur (marching squares) -> polyline yang disederhanakan (Douglas-Peucker).
#pragma once
#include <QImage>
#include "Types.h"

struct TraceOptions {
    int threshold = 128;      // piksel dengan kecerahan < threshold dianggap gelap
    double simplify = 0.8;    // toleransi penyederhanaan (piksel)
    int maxDim = 700;         // gambar diperkecil ke sisi terpanjang ini sebelum di-trace
    double minLength = 6.0;   // buang kontur lebih pendek dari ini (piksel) = noise
    bool invert = false;
};

// Hasil dalam koordinat piksel gambar yang sudah diperkecil (y ke bawah); size diisi ukuran akhir.
Paths traceImage(const QImage& src, const TraceOptions& opt, QSizeF* size);

Polyline simplifyPolyline(const Polyline& p, double eps);
