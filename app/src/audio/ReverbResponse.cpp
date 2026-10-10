#include "audio/ReverbResponse.h"

#include "builtin/ReverbDesign.h"

#include <algorithm>
#include <array>
#include <cstddef>

namespace sub::app {

static_assert(kReverbTaps == sub::reverb::kMaxTaps && kReverbMeterFloorDb == double(sub::reverb::kMeterFloorDb));
static_assert(kReverbMinInFreq == sub::reverb::kMinInFreq && kReverbMaxInFreq == sub::reverb::kMaxInFreq &&
              kReverbMinInWidth == sub::reverb::kMinInWidth && kReverbMaxInWidth == sub::reverb::kMaxInWidth &&
              kReverbMinSpinRate == sub::reverb::kMinSpinRate && kReverbMaxSpinRate == sub::reverb::kMaxSpinRate);
static_assert(kReverbMinShelfFreq == sub::reverb::kMinShelfFreq && kReverbMaxLoFreq == sub::reverb::kMaxLoFreq &&
              kReverbMaxHiFreq == sub::reverb::kMaxHiFreq && kReverbMinShelfGain == sub::reverb::kMinShelfGain &&
              kReverbMaxShelfGain == sub::reverb::kMaxShelfGain && kReverbMinDecayMs == sub::reverb::kMinDecayMs &&
              kReverbMaxDecayMs == sub::reverb::kMaxDecayMs);

QList<double> reverbDecaySeconds(const ReverbDecaySettings& settings, double sampleRate,
                                 const QList<double>& frequencies) {
    sub::reverb::DecaySettings p;
    p.decayMs = settings.decayMs;
    p.size = settings.size;
    p.scale = settings.scale;
    p.density = settings.density;
    p.loShelf = settings.loShelf;
    p.hiFilter = settings.hiFilter;
    p.hiLowpass = settings.hiLowpass;
    p.freeze = settings.freeze;
    p.flat = settings.flat;
    p.cut = settings.cut;
    p.loFreq = settings.loFreq;
    p.loGain = settings.loGain;
    p.hiFreq = settings.hiFreq;
    p.hiGain = settings.hiGain;
    QList<double> seconds;
    seconds.reserve(frequencies.size());
    for (const double f : frequencies) seconds.append(sub::reverb::decaySeconds(p, f, sampleRate));
    return seconds;
}

QList<double> reverbInputFilterDb(double freq, double width, bool loCut, bool hiCut, double sampleRate,
                                  const QList<double>& frequencies) {
    QList<double> db;
    db.reserve(frequencies.size());
    for (const double f : frequencies)
        db.append(sub::reverb::inputFilterDb(freq, width, loCut, hiCut, f, sampleRate));
    return db;
}

QList<ReverbTap> reverbEarlyTaps(double size, double shape, int density) {
    std::array<sub::reverb::Tap, sub::reverb::kMaxTaps> taps{};
    sub::reverb::earlyTaps(sub::reverb::sizeFactor(size), shape, sub::reverb::densityAt(density), taps);
    QList<ReverbTap> out;
    out.reserve(sub::reverb::kMaxTaps);
    for (const sub::reverb::Tap& tap : taps) out.append({tap.ms, tap.gain, tap.pan});
    return out;
}

double reverbSpinPan(int k, double amount, double phase) {
    return sub::reverb::spinPan(std::clamp(k, 0, sub::reverb::kMaxTaps - 1), amount, phase);
}

double reverbStereoWidth(double stereo) { return sub::reverb::stereoWidth(stereo); }

double reverbDiffuseOnsetMs(double size, double shape, int density) {
    const double s = sub::reverb::sizeFactor(size);
    double shortest = sub::reverb::kLineMs.back();
    for (const int line : sub::reverb::activeLines(sub::reverb::densityAt(density)))
        shortest = std::min(shortest, sub::reverb::kLineMs[static_cast<std::size_t>(line)]);
    return sub::reverb::onsetMs(s, shape) + shortest * s;
}

}  // namespace sub::app
