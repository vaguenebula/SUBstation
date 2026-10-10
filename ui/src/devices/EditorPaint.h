#pragma once

// Small drawing helpers the device editors' items share: QPainter habits of
// the old editors that SgPainter has no call for (a colour at an alpha, dashed
// lines, Bézier outlines), number formatting as Python's f-strings did it, the
// graphs' logarithmic axes (frequency, time) with their decade grid, and what
// the dynamics and modulation editors animate with: meters with ballistics,
// values easing towards a target per display tick, a glowing line.

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
// The colour `t` of the way from `a` to `b` (0..1, clamped), alpha too.
QColor mixColor(const QColor& a, const QColor& b, double t);
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
// A cubic Bézier from `from`, flattened into `out` (from excluded). `from` is a copy: it is often
// out.back(), which appending moves.
void appendCubic(std::vector<QPointF>& out, QPointF from, const QPointF& c1, const QPointF& c2, const QPointF& to,
                 int steps = 12);

// --- Animation --------------------------------------------------------------------------------
// Editors animate in refreshDisplays() (about 60 times a second, DisplayClock): they move these on
// by the time since the last tick, then update(); paint() only reads them.

// A meter's reading as the eye follows it: it rises at once, falls smoothly (`fallDbPerSecond`),
// and its peak holds for `holdSeconds` before falling as fast. Levels in dB, floored at `floorDb`.
struct MeterBallistics {
    double level = -120.0;
    double peak = -120.0;
    double held = 0.0;  // seconds the peak has held

    void update(double db, double dtSeconds, double fallDbPerSecond = 24.0, double holdSeconds = 1.0,
                double floorDb = -120.0);
    void reset(double floorDb = -120.0) {
        level = peak = floorDb;
        held = 0.0;
    }
};

// A value easing towards its target: each step moves it `fraction` of the way (a one-pole at the
// tick's rate), snapping once within `epsilon`. step() says whether it is still moving (an editor
// keeps repainting until everything settles).
struct Eased {
    double value = 0.0;
    double target = 0.0;

    bool step(double fraction, double epsilon = 1e-4) {
        if (value == target)
            return false;
        value += (target - value) * std::clamp(fraction, 0.0, 1.0);
        if (std::abs(target - value) <= epsilon)
            value = target;
        return true;
    }
    void snap(double to) { value = target = to; }
};

// The fraction a one-pole of time constant `seconds` moves in `dtSeconds`: Eased::step's
// argument for an easing that looks the same whatever the tick's rate.
inline double easeFraction(double dtSeconds, double seconds) {
    return seconds <= 0.0 ? 1.0 : 1.0 - std::exp(-dtSeconds / seconds);
}

// 0 below 0, 1 above 1, and an S-curve between, flat at both ends (smoothstep): fades and
// highlights that start and stop softly.
inline double smoothstep(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// --- Meters and lines -------------------------------------------------------------------------

// `db` on a vertical scale over `rect`: `ceilingDb` at the top, `floorDb` at the bottom (held there).
double dbToY(double db, const QRectF& rect, double floorDb, double ceilingDb);

// The house level meter, upright in `rect`: a dark well, the level filled green, turning yellow
// above -12 dB and red above -3 dB (of the ceiling), faint lines every 12 dB, and the held peak as
// a bright line (red once it reaches the ceiling).
void drawLevelMeter(SgPainter& painter, const QRectF& rect, double levelDb, double peakDb, double floorDb = -60.0,
                    double ceilingDb = 0.0);
// A gain reduction meter: the reduction grows down from the top of `rect` in the accent colour,
// `rangeDb` at the bottom, with faint lines every 6 dB.
void drawReductionMeter(SgPainter& painter, const QRectF& rect, double reductionDb, double rangeDb = 24.0);
// A line with a soft glow: two wider, fainter strokes of `color` under it.
void drawGlowPolyline(SgPainter& painter, const std::vector<QPointF>& points, const QColor& color, double width = 1.5);

}  // namespace sub::ui
