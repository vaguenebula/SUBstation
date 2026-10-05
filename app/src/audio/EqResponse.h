#pragma once
// The EQ device's curves, as its editor draws them: a band's response is the
// engine's own (engine/src/builtin/EqDesign.h), so the curve drawn is the one
// that plays.

#include <QList>

namespace sub::app {

// A band's response in dB at each of `frequencies` (Hz; above Nyquist:
// Nyquist's). `type` and `slope` are its parameters' list indexes (Bell, Low
// Shelf, Low Cut, High Shelf, High Cut, Notch, Band Pass, Tilt Shelf; 6 to 96
// dB/octave), `freq` in Hz, `gain` in dB.
QList<double> eqResponseDb(int type, double freq, double gain, double q, int slope, double sampleRate,
                           const QList<double>& frequencies);

}  // namespace sub::app
