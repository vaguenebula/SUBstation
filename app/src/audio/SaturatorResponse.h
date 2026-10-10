#pragma once
// The Saturator device's shaping and Color, as its editor draws them: worked
// out by the engine's own functions (engine/src/builtin/SaturatorDesign.h), so
// the curve drawn is the shaping that plays and the EQ drawn is Color's. And
// what else the editor needs to know of the engine: which Type entries have
// controls of their own, Hi-Quality's latency, the parameters' ranges.

#include <QList>
#include <QString>

#include <algorithm>

namespace sub::app {

// A Saturator's shaping, in its parameters' units.
struct SaturatorShape {
    int type = 0;  // the Type list's index (2 Bass Shaper, 7 Waveshaper)
    double driveDb = 0.0;
    double thresholdDb = -18.0;  // the Bass Shaper's
    double wsDrive = 50.0, wsLin = 50.0, wsCurve = 50.0, wsDamp = 0.0, wsDepth = 0.0, wsPeriod = 0.0;  // %
    int clip = 0;  // Post Clip: 0 none, 1 soft, 2 hard
    bool operator==(const SaturatorShape&) const = default;
};

// The curve's output for each input level in `inputs` (-1..1 is ±0 dBFS, before Drive): Drive, the
// curve and Post Clip, in the engine's float arithmetic. Color's emphasis cancels where the curve
// is straight, and Dry/Wet and Output aren't part of it.
QList<double> saturatorCurve(const SaturatorShape& shape, const QList<double>& inputs);
double saturatorCurveAt(const SaturatorShape& shape, double input);
// The curve's slope at 0 (its gain for quiet input), measured at 1e-4. It can be about 0 (the
// Waveshaper's Damp at WS Drive 100 %) or large (its ripples): callers guard divisions by it.
double saturatorSlope(const SaturatorShape& shape);
// Color's emphasis (before the curve) in dB at each of `frequencies` (Hz), at `sampleRate`: the
// low shelf of `baseDb` and the peak at `freq` (Hz) of `depthDb`, `width` (%) wide.
QList<double> saturatorColorDb(double baseDb, double freq, double width, double depthDb, double sampleRate,
                               const QList<double>& frequencies);
// The Bass Shaper's threshold as an input level (linear, before Drive): where its curve leaves the
// straight line.
double saturatorThresholdInput(double thresholdDb, double driveDb);

// The Type list's entries whose curves have controls of their own (the engine's saturator::Type):
// the Bass Shaper's Threshold, the Waveshaper's six.
int saturatorBassShaperType();
int saturatorWaveshaperType();
// Hi-Quality's latency, in samples (its 4x filters').
int saturatorHqLatency();

// A parameter's range in its units, as the engine declares it: what the graphs' drags stay within.
struct SaturatorRange {
    double low = 0.0, high = 1.0;
    double clamp(double value) const { return std::clamp(value, low, high); }
};
SaturatorRange saturatorRange(const QString& paramId);

}  // namespace sub::app
