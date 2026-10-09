// The fingerprint of a sound, made with Essentia (third_party/essentia): a
// fixed set of numbers describing how it sounds, so that sounds can be
// compared by how far apart their fingerprints are.
//
// It is made of *aspects*, each a group of features, which the comparison
// weighs as wholes (FeatureSchema.h, Similarity.h):
//
//   Timbre         the spectral envelope: MFCCs 1-12 (Essentia's MFCC: 40 HTK mel
//                  bands, 20 Hz to 16 kHz, power in dB, DCT-II), their mean over
//                  the sound, louder frames counting more
//   TimbreMotion   how the timbre moves: MFCCs 1-4 of the attack (the first 30
//                  ms), the body (to 250 ms) and the tail (after it)
//   TimbreSpread   how much it moves: each MFCC's spread over the sound
//   Spectrum       brightness and its spread (Centroid, its spread over time,
//                  CentralMoments' spread as a bandwidth, RollOff at 85%),
//                  noisiness (Flatness), how much is sub-bass (EnergyBand below
//                  120 Hz) and air (above 8 kHz), and the attack's brightness and
//                  noisiness: what tells a click from a thump in a short one-shot
//   Contrast       peaks against valleys in six bands (SpectralContrast): a
//                  tone's spectrum is all peaks, noise's all valley
//   SpectralShape  the spectrum's shape beyond its centre and spread: skewness and
//                  kurtosis (DistributionShape), Crest, HFC, ZeroCrossingRate, how
//                  it changes (Flux) and slopes (Decrease)
//   Tonality       how many peaks it has (SpectralComplexity), how they beat
//                  (Dissonance: a cymbal's metal, a detuned stab), how strongly
//                  the spectrum repeats at a harmonic spacing (PitchSalience)
//   Envelope       how it starts and dies away, on its 2 ms amplitude envelope:
//                  LogAttackTime (20% to 90% of the peak), EffectiveDuration
//                  (time within 30 dB of the peak), the temporal centroid
//                  (Centroid over time), and the level in eight octave-wide
//                  windows after the peak (20 ms to 2.6 s)
//   Pitch          how pitched it is (PitchYin, YIN, on 93 ms windows from the
//                  peak, down to 21.5 Hz), and the pitch where it is
//   Rhythm         onsets per second after the first (one-shots have none, loops
//                  many: peaks of the mel bands' flux), and the file's length
//
// The first six are the descriptors SUBstation's own extractor had, made with
// Essentia's algorithms; the next four are Essentia's that it hadn't, each
// kept because it made the nearest sounds more often of a query's kind on a
// held-out half of a labelled library (docs/intelligence.md).
//
// Log attack time and the spectral centroid are what listeners' timbre spaces
// of percussive sounds are organised by (Lakatos 2000; McAdams), and MFCCs are
// the standard description of the spectral envelope. Frame features are summed
// up by weighted means (the attack, body and tail separately for what changes
// most in a one-shot), so a fingerprint is the same size for a 5 ms click and
// a 6 s loop.
//
// What doesn't count: the level (everything is relative to the sound's own
// peak, and MFCC 0, the frame's level, is left out); the sample rate (every
// sound is analysed at 44.1 kHz, decoded or resampled to it, so a 22 kHz and a
// 96 kHz copy of a sound are measured alike); leading silence (skipped, and a
// fixed 2 ms pre-roll put before the start). Only the start of a sound is
// analysed (kAnalysisSeconds from where it starts); the length feature is the
// whole file's.
//
// Essentia computes the frames' descriptors and the envelope's; the module
// decodes, frames and sums up (Descriptors.h). Essentia's algorithms are made
// once per extractor and kept: one extractor per thread.

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "similarity/FeatureExtractor.h"
#include "similarity/FeatureSchema.h"

namespace sub::intelligence {

// Where each feature is in a fingerprint.
namespace feature {
enum : uint32_t {
    MfccMean = 0,      // 12: MFCC 1-12, mean (dB)
    PartMfcc = 12,     // 12: MFCC 1-4 of the attack, the body and the tail (dB)
    Centroid = 24,     // log2(spectral centroid / 1 kHz)
    CentroidSpread,    // its standard deviation over the sound (octaves)
    Bandwidth,         // log2(spectral spread / 1 kHz)
    Rolloff,           // log2(85% roll-off / 1 kHz)
    Flatness,          // spectral flatness (dB, -60..0)
    SubBass,           // share of energy below 120 Hz (dB)
    Air,               // share of energy above 8 kHz (dB)
    AttackCentroid,    // log2(centroid of the first 30 ms / 1 kHz)
    AttackFlatness,    // flatness of the first 30 ms (dB, -60..0)
    AttackTime,        // log10(seconds from 20% to 90% of the peak amplitude)
    Duration,          // log10(seconds within 30 dB of the peak)
    TemporalCentroid,  // log10(seconds: the energy's centre in time)
    Contour,           // 8: level after the peak (dB, -60..0) around 20, 40, ... 2560 ms
    PitchConfidence = Contour + 8,  // 0..1: how much of the sound is periodic
    Pitch,             // confidence * log2(f0 / 220 Hz)
    OnsetRate,         // log2(1 + onsets per second after the first)
    Length,            // log10(the file's seconds)
    MfccSpread,        // 12: MFCC 1-12, standard deviation over the sound (dB)
    Contrast = MfccSpread + 12,  // 6: spectral contrast in six octave-ish bands (SpectralContrast)
    Valley = Contrast + 6,       // 6: the bands' valleys (log magnitude)
    Skewness = Valley + 6,       // the spectrum's skewness (DistributionShape), sign(x) ln(1 + |x|)
    Kurtosis,          // ln of its fourth standardized moment
    Crest,             // log10(the loudest bin / the mean)
    Hfc,               // log2(high-frequency content / energy / 1 kHz)
    ZeroCrossings,     // log2(1 + zero crossings per second)
    Flux,              // the spectrum's rise from frame to frame (relative to its loudest bin)
    Decrease,          // the spectrum's slope (relative to its loudest bin)
    Complexity,        // log2(1 + spectral peaks within 60 dB of the loudest)
    Dissonance,        // 0..1: how much the peaks beat against each other (Dissonance)
    Salience,          // 0..1: how strongly the spectrum repeats 200 Hz-5 kHz apart (PitchSalience;
                       // harmonics do, and noise's even spectrum scores high too)
    Count
};
}  // namespace feature

inline constexpr size_t kDims = feature::Count;
inline constexpr int kMfccs = 12;
inline constexpr int kParts = 3;  // attack, body, tail
inline constexpr int kPartMfccs = 4;
inline constexpr int kContourPoints = 8;
inline constexpr int kContrastBands = 6;

// The rate every sound is analysed at.
inline constexpr uint32_t kAnalysisRate = 44100;

// Bump when what EssentiaExtractor computes changes: fingerprints saved by an
// earlier version are then made again. (Its settings and Essentia's version
// are part of what saved fingerprints must match too: FeatureSchema::key.)
inline constexpr uint32_t kFeatureVersion = 1;

// The fingerprint's features (kDims of them) and schema ("essentia").
const std::vector<FeatureInfo>& featureInfo();
const FeatureSchema& essentiaSchema();

class EssentiaExtractor final : public FeatureExtractor {
public:
    EssentiaExtractor();
    ~EssentiaExtractor() override;
    EssentiaExtractor(const EssentiaExtractor&) = delete;
    EssentiaExtractor& operator=(const EssentiaExtractor&) = delete;

    const FeatureSchema& schema() const override { return essentiaSchema(); }
    Extraction extract(const SoundBuffer& sound, float* out, const CancelFlag* cancel = nullptr) override;

protected:
    // Decoded at kAnalysisRate.
    MonoAudio decode(const std::string& path, double start, double seconds) override;

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace sub::intelligence
