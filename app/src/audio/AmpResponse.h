#pragma once
// The Amp device's curves, as its editor draws them: worked out by the engine's
// own design (engine/src/builtin/AmpDesign.h: the model's voicing, its tone
// stack and the stages' curve), so what is drawn is what plays.

#include <QList>

namespace sub::app {

// The Amp's tone section (the model's tone stack with its make-up, and Presence)
// in dB at each of `frequencies` (Hz), as the engine plays it at `sampleRate`.
// `model` is the Amp Type's index (held to 0..6); the dials are 0..10.
QList<double> ampToneResponseDb(int model, double bass, double middle, double treble, double presence,
                                double sampleRate, const QList<double>& frequencies);

// The Amp's static transfer for a 1 kHz tone: what comes out for each input
// sample value of `xs` (1.0: 0 dBFS) with these settings and the power stage's
// drive `sagDb` lower (the supply sagging).
QList<double> ampTransfer(int model, double gain, double bass, double middle, double treble, double presence,
                          double volume, double sagDb, double sampleRate, const QList<double>& xs);

}  // namespace sub::app
