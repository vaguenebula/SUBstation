#pragma once

// Small drawing helpers the device editors' items share: QPainter habits of
// the old editors that SgPainter has no call for (a colour at an alpha, dashed
// lines, Bézier outlines), and number formatting as Python's f-strings did it.

#include <QColor>
#include <QPointF>
#include <QString>

#include <vector>

namespace sub::ui {

class SgPainter;

// `color` at `alpha` (0..255, clamped), as the editors' _alpha().
QColor withAlpha(const QColor& color, int alpha);
// f"{value:.{decimals}f}"
QString pythonFixed(double value, int decimals);
// f"{value:+.{decimals}f}": always signed.
QString pythonSigned(double value, int decimals);
// f"{value:g}" for the values the editors show (ranges, frequencies).
QString pythonGeneral(double value);

// A polyline dashed as QPen's Qt::DashLine: dashes of 4 widths, gaps of 2.
void drawDashedPolyline(SgPainter& painter, const std::vector<QPointF>& points, const QColor& color, double width);
// A cubic Bézier from `from`, flattened into `out` (from excluded).
void appendCubic(std::vector<QPointF>& out, const QPointF& from, const QPointF& c1, const QPointF& c2,
                 const QPointF& to, int steps = 12);

}  // namespace sub::ui
