#pragma once
// The Reverb device's curves, as its editor draws them: worked out from the
// engine's own maths (engine/src/builtin/ReverbDesign.h), so what is drawn is
// what plays: how long each frequency rings, the band the reverb hears, where
// the early reflections are and how Spin swings them.

#include <QList>

namespace sub::app {

// What the decay curve depends on, as the device's parameters hold them.
struct ReverbDecaySettings {
    double decayMs = 1200.0, size = 100.0, scale = 50.0;
    int density = 3;  // 0 Sparse .. 3 High
    bool loShelf = true, hiFilter = true, hiLowpass = false, freeze = false, flat = true, cut = true;
    double loFreq = 90.0, loGain = 75.0, hiFreq = 4500.0, hiGain = 70.0;  // Hz, % of the decay
};

// The tail's decay time (s, to -60 dB) at each of `frequencies` (Hz), as the
// network plays it at `sampleRate`: about 1000 s frozen with Cut and Flat; +inf
// where nothing is lost.
QList<double> reverbDecaySeconds(const ReverbDecaySettings& settings, double sampleRate,
                                 const QList<double>& frequencies);

// The input filter's gain in dB at each of `frequencies`: the band around
// `freq` (Hz), `width` octaves wide, with its low and high cuts as switched.
QList<double> reverbInputFilterDb(double freq, double width, bool loCut, bool hiCut, double sampleRate,
                                  const QList<double>& frequencies);

// An early reflection at rest: its time after the predelay (ms), its gain
// (signed) and pan (-1..1).
struct ReverbTap {
    double ms = 0.0, gain = 0.0, pan = 0.0;
};
// All twelve, in order (gain 0 for those `density` doesn't use), for Size
// `size` and Shape `shape` (%).
QList<ReverbTap> reverbEarlyTaps(double size, double shape, int density);
// Where Spin has reflection `k` (0..11) in the stereo field (-1..1), its amount
// 0..1, its LFO at `phase` (cycles): the reflection's own pan at amount 0.
double reverbSpinPan(int k, double amount, double phase);

// How long after the predelay the diffuse tail starts (ms): the network hears
// the input Shape's onset later, and its first echo comes out a pass of its
// shortest line after that (the diffusers pass some of it straight through),
// at Size `size`, Shape `shape` (%) and `density` (0 Sparse .. 3 High).
double reverbDiffuseOnsetMs(double size, double shape, int density);

}  // namespace sub::app
