#pragma once

// Small drawing helpers the device editors' items share: QPainter habits of
// the old editors that SgPainter has no call for (a colour at an alpha, dashed
// lines, Bézier outlines), number formatting as Python's f-strings did it, and
// the graphs' logarithmic axes (frequency, time) with their decade grid.

#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QString>

#include <algorithm>
#include <cmath>
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

// A logarithmic axis: `low`..`high` (> 0: Hz, ms) over `length` pixels from
// `from` (a negative length runs the other way: up, for a vertical axis).
struct LogAxis {
    double low = 1.0, high = 10.0;
    double from = 0.0, length = 1.0;

    double position(double value) const { return from + std::log(value / low) / std::log(high / low) * length; }
    // The value at pixel `at`, held to low..high.
    double valueAt(double at) const {
        const double fraction = std::clamp((at - from) / length, 0.0, 1.0);
        return low * std::pow(high / low, fraction);
    }
};

// The device graphs' frequency grid over `plot`, `axis` across it: a line per
// decade, fainter ones at the multiples between.
void drawDecadeGrid(SgPainter& painter, const QRectF& plot, const LogAxis& axis);

// A polyline dashed as QPen's Qt::DashLine: dashes of 4 widths, gaps of 2.
void drawDashedPolyline(SgPainter& painter, const std::vector<QPointF>& points, const QColor& color, double width);
// A cubic Bézier from `from`, flattened into `out` (from excluded).
void appendCubic(std::vector<QPointF>& out, const QPointF& from, const QPointF& c1, const QPointF& c2,
                 const QPointF& to, int steps = 12);

}  // namespace sub::ui
