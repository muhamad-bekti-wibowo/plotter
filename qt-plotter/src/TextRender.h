// Teks -> jalur plot: font satu goresan (Hershey) atau font Windows (kontur, bisa diarsir).
#pragma once
#include <QDateTime>
#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QVector>
#include "Types.h"

struct TextOptions {
    QString text = "Halo";          // sudah diekspansi variabelnya (lihat expandTextVars)
    bool singleStroke = true;       // true: font goresan; false: font sistem (kontur)
    QString strokeFont = "futural"; // id font goresan (lihat strokeFonts())
    QString family = "Arial";       // font sistem
    bool bold = false, italic = false;     // font sistem
    int strokePasses = 1;           // goresan: 1..3 lintasan (lebih tebal)
    double penWidth = 0.4;          // mm, jarak antar lintasan tebal
    double height = 10;             // mm, tinggi huruf kapital
    double letterSpacing = 0;       // mm tambahan antar huruf
    double lineSpacing = 1.5;       // kelipatan tinggi huruf antar baris
    int align = 0;                  // 0 kiri, 1 tengah, 2 kanan
    double rotation = 0;            // derajat, positif = berlawanan arah jarum jam
    bool hatch = false;             // font sistem: isi huruf dengan arsiran
    double hatchSpacing = 0.6;      // mm
    double hatchAngle = 45;         // derajat
};

struct StrokeFontInfo { QString id, label; };

QVector<StrokeFontInfo> strokeFonts();
// Hasil dalam mm, y ke bawah (sama seperti SVG).
Paths renderText(const TextOptions& o, QString* error = nullptr);
// {n} {n:4} nomor (dengan nol di depan), {tanggal}/{date}, {jam}/{time}
QString expandTextVars(const QString& text, int counter, const QDateTime& now);
