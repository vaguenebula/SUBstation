#pragma once
// The Disperser device's group delay, as its editor draws it: worked out from
// the engine's own stages (engine/src/builtin/DisperserDesign.h), so the curve
// drawn is the delay that plays.

#include <QList>

namespace sub::app {

// The group delay in milliseconds, at each of `frequencies` (Hz; above Nyquist:
// Nyquist's), of `stages` stages tuned to `freq` (Hz; kept below Nyquist, as
// the engine keeps it) with `pinch` (their Q), at `sampleRate`.
QList<double> disperserGroupDelayMs(int stages, double freq, double pinch, double sampleRate,
                                    const QList<double>& frequencies);

// The frequency the stages are tuned to for `freq` (Hz): the same, kept below
// Nyquist (0.45 of the sample rate).
double disperserTunedFrequency(double freq, double sampleRate);

// Where that delay peaks (Hz; 0 for a pinch so low it peaks at 0 Hz). High up,
// the peak is narrower than a spread of frequencies may catch: a curve drawn
// from them takes this one too.
double disperserPeakFrequency(double freq, double pinch, double sampleRate);

}  // namespace sub::app
