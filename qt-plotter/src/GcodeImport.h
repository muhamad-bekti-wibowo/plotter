// Parser G-code -> polyline (G0/G1/G2/G3, G90/G91, G20/G21, pena lewat Z atau M3/M5/M4).
#pragma once
#include <QByteArray>
#include <QString>
#include "Types.h"

struct GcodeResult {
    Paths paths;      // mm; Y sudah dibalik (y ke bawah) supaya sejalur dengan SVG dan dibalik lagi oleh PathPlanner
    QString error;
    QString warning;
    int lines = 0;    // baris perintah yang dibaca
    double travel = 0, draw = 0;   // panjang perjalanan pena naik / menggambar (mm)
};

GcodeResult parseGcode(const QByteArray& data);
