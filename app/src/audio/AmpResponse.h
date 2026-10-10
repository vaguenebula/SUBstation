#pragma once
// The Amp device's curves, as its editor draws them: worked out by the engine's
// own design (engine/src/builtin/AmpDesign.h: the model's voicing, its tone
// stack, its filters and the stages' curve), so what is drawn is what plays;
// and the figures the editor reads the device by (its models, its displays'
// rate and floor), the device's own.

#include <QList>
#include <QStringList>

#include <memory>

namespace sub::app {

// The models, in the Amp Type parameter's order (Clean, Boost, Blues, Rock, Lead, Heavy,
// Bass), and how many there are.
QStringList ampModelNames();
int ampModelCount();

// The device's displays: audio samples per value (256), and the floor they read (dB: -90).
int ampDisplaySamples();
double ampDisplayFloorDb();

// The Amp's tone section (the model's tone stack with its make-up, and Presence)
// in dB at each of `frequencies` (Hz), as the engine plays it at `sampleRate`.
// `model` is the Amp Type's index (held to 0..6); the dials are 0..10.
QList<double> ampToneResponseDb(int model, double bass, double middle, double treble, double presence,
                                double sampleRate, const QList<double>& frequencies);

// The Amp's transfer for a 1 kHz tone (sub::amp::Transfer: the tone played
// through the engine's stages and filters until it has settled): at each x of
// `xs`, the output's highest value for a tone of peak x (1.0: 0 dBFS), or for
// x < 0 its lowest for a tone of peak -x, with these settings and the power
// stage's drive `sagDb` lower (the supply sagging).
QList<double> ampTransfer(int model, double gain, double bass, double middle, double treble, double presence,
                          double volume, double sagDb, double sampleRate, const QList<double>& xs);

// The same curve, made in two parts so that the sag moving alone costs a
// fraction: the preamp's part for the settings and the xs once (prepare()),
// then the power stage's for any sag (at()). at() gives what ampTransfer()
// does, bit for bit.
class AmpTransferCurve {
public:
    AmpTransferCurve();
    ~AmpTransferCurve();
    AmpTransferCurve(const AmpTransferCurve&) = delete;
    AmpTransferCurve& operator=(const AmpTransferCurve&) = delete;

    void prepare(int model, double gain, double bass, double middle, double treble, double presence, double volume,
                 double sampleRate, const QList<double>& xs);
    bool isEmpty() const { return !parts_; }
    // The curve at each x prepared, with the power drive `sagDb` lower.
    QList<double> at(double sagDb) const;
    // Its slope through 0: what a small signal comes out as, over what goes in.
    double smallSignalGain(double sagDb) const;

private:
    struct Parts;
    std::unique_ptr<Parts> parts_;
};

}  // namespace sub::app
