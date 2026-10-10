#pragma once
// The Gate device's maths, as its editor draws it: from the engine's own
// (engine/src/builtin/GateDesign.h), so the key filter's curve drawn is the one
// that keys the gate, and the gain shown is the one it applies.

#include <QList>

namespace sub::app {

// The key EQ's response in dB at each of `frequencies` (Hz; above Nyquist:
// Nyquist's): filter `type` (0..5: low shelf, bell, high shelf, low-pass,
// band-pass, high-pass, as the device's S/C EQ Type) at `freq` with `q` and
// `gainDb`, at `sampleRate`.
QList<double> gateKeyFilterDb(int type, double freq, double q, double gainDb, double sampleRate,
                              const QList<double>& frequencies);
// Whether filter `type` uses the gain (the shelves and the bell) or the Q (the bell and the pass filters).
bool gateKeyFilterUsesGain(int type);
bool gateKeyFilterUsesQ(int type);

// The floor's gain: 0 (silence) at the bottom of its range.
double gateFloorGain(double floorDb);
bool gateFloorIsSilent(double floorDb);
double gateFloorMinDb();  // -75
// The gain in dB for `pass` (0..1: how much passes, the display "open") at a floor; at a silent floor with
// nothing passing, -infinity (callers clamp).
double gateGainDb(double pass, double floorDb);
// The level (dB) below which an open gate closes again.
double gateCloseDb(double thresholdDb, double returnDb);
// Lookahead choice `index` (0, 1, 10 ms) in samples.
int gateLookaheadSamples(int index, double sampleRate);
// Audio samples per value of the device's displays (256).
int gateDisplaySamples();

}  // namespace sub::app
