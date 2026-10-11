#pragma once
// The Gate device's maths, as its editor draws it: from the engine's own
// (engine/src/builtin/GateDesign.h), so the key filter's curve drawn is the one
// that keys the gate, the gain shown is the one it applies, and what the graphs
// drag stays in the parameters' ranges.

#include <QList>

#include <algorithm>

namespace sub::app {

// A parameter's range, as the device has it.
struct GateRange {
    double min = 0.0, max = 0.0;
    double clamp(double value) const { return std::clamp(value, min, max); }
};
// The ranges of what the editor's graphs drag: Threshold and Return (dB), the key EQ's Freq (Hz), Q and Gain (dB).
GateRange gateThresholdRange();
GateRange gateReturnRange();
GateRange gateKeyFreqRange();
GateRange gateKeyQRange();
GateRange gateKeyGainRange();

// The key EQ's response in dB at each of `frequencies` (Hz; above Nyquist:
// Nyquist's): filter `type` (0..5: low shelf, bell, high shelf, low-pass,
// band-pass, high-pass, as the device's S/C EQ Type) at `freq` with `q` and
// `gainDb`, at `sampleRate`.
QList<double> gateKeyFilterDb(int type, double freq, double q, double gainDb, double sampleRate,
                              const QList<double>& frequencies);
// Whether filter `type` uses the gain (the shelves and the bell) or the Q (the bell and the pass filters).
bool gateKeyFilterUsesGain(int type);
bool gateKeyFilterUsesQ(int type);

// Whether Floor at `floorDb` is silence (the bottom of its range).
bool gateFloorIsSilent(double floorDb);
// The gain in dB for `pass` (0..1: how much passes, the display "open") at a floor; at a silent floor with
// nothing passing, -infinity (callers clamp).
double gateGainDb(double pass, double floorDb);
// The level (dB) below which an open gate closes again.
double gateCloseDb(double thresholdDb, double returnDb);
// Audio samples per value of the device's displays (256).
int gateDisplaySamples();

}  // namespace sub::app
