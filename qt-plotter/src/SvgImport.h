// Parser SVG sederhana -> polyline (path, line, polyline, polygon, rect, circle, ellipse + transform).
#pragma once
#include <QByteArray>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include "Types.h"

struct SvgResult {
    Paths paths;      // koordinat satuan pengguna SVG (y ke bawah)
    QString error;
    QString warning;
};

SvgResult parseSvg(const QByteArray& data);

// Tulis Paths menjadi dokumen SVG (stroke hitam, tanpa isi).
QString pathsToSvg(const Paths& paths, const QRectF& bounds);
