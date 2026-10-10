#pragma once
// The Spectral Compressor's curves as its editor draws them, from the engine's
// own maths (engine/src/builtin/SpectralDesign.h), so the line drawn is the
// threshold that plays. Levels are the engine's pink-referenced dB: pink noise
// of RMS L dBFS reads L dB at every frequency, so the threshold, the spectra
// and the levels compared share one axis.

#include <QList>

namespace sub::app {

// The threshold in pink-referenced dB at each of `frequencies` (Hz): Threshold, turned about 1 kHz by Tilt
// (dB per octave).
QList<double> spectralThresholdDb(double threshold, double tilt, const QList<double>& frequencies);
// The upward (Below) threshold there: Below, never above Threshold, tilted the same.
QList<double> spectralBelowDb(double threshold, double below, double tilt, const QList<double>& frequencies);
// The share of its gain each frequency gets from the Focus band `low`..`high` (Hz): 0..1.
QList<double> spectralFocusWeights(double low, double high, const QList<double>& frequencies);
// The engine's display points (Hz: 128, log-spaced 20 Hz..20 kHz): what each value of its spectral displays
// stands for.
QList<double> spectralDisplayFrequencies();

}  // namespace sub::app
