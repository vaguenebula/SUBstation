// The fingerprint of a sound: a fixed set of numbers describing how it sounds,
// so that sounds can be compared by how far apart their fingerprints are.
//
// It is made of *aspects*, each a group of features, which the comparison
// weighs as wholes (Similarity.h):
//
//   Timbre         the spectral envelope: MFCCs 1-12 (of 40 mel bands, 20 Hz to
//                  16 kHz), their mean over the sound, louder moments counting more
//   TimbreMotion   how the timbre moves: MFCCs 1-4 of the attack (the first 30
//                  ms), the body (to 250 ms) and the tail (after it)
//   Spectrum       brightness and its spread (spectral centroid, its spread over
//                  time, bandwidth, roll-off), noisiness (flatness), how much is
//                  sub-bass (below 120 Hz) and air (above 8 kHz), and the
//                  brightness of the attack
//   Envelope       how it starts and dies away: log attack time, effective
//                  duration (time within 30 dB of the peak), temporal centroid,
//                  and the level in eight octave-wide windows after the peak
//                  (20 ms to 2.6 s)
//   Pitch          how pitched it is (YIN), and the pitch where it is
//   Rhythm         onsets per second after the first (one-shots have none, loops
//                  many), and the file's length
//
// Log attack time and the spectral centroid are what listeners' timbre spaces
// of percussive sounds are organised by (Lakatos 2000; McAdams), and MFCCs are
// the standard description of the spectral envelope. Levels are relative to the
// sound's own peak, so a quieter copy of a sound has the same fingerprint.
//
// Only the start of a sound is analysed: from where it starts (its leading
// silence skipped) for kAnalysisSeconds. The length feature is the whole file's.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace sub::intelligence {

enum class Aspect : uint8_t { Timbre, TimbreMotion, Spectrum, Envelope, Pitch, Rhythm };
inline constexpr size_t kAspects = 6;

// Where each feature is in a fingerprint.
namespace feature {
enum : uint32_t {
    MfccMean = 0,     // 12: MFCC 1-12, mean (dB)
    PartMfcc = 12,    // 12: MFCC 1-4 of the attack, the body and the tail (dB)
    Centroid = 24,    // log2(spectral centroid / 1 kHz)
    CentroidSpread,   // its standard deviation over the sound
    Bandwidth,        // log2(spectral spread / 1 kHz)
    Flatness,         // spectral flatness (dB, -60..0)
    Rolloff,          // log2(85% roll-off / 1 kHz)
    SubBass,          // share of energy below 120 Hz (dB)
    Air,              // share of energy above 8 kHz (dB)
    AttackCentroid,   // log2(centroid of the first 30 ms / 1 kHz)
    AttackTime,       // log10(seconds from 20% to 90% of the peak amplitude)
    Duration,         // log10(seconds within 30 dB of the peak)
    TemporalCentroid, // log10(seconds: the energy's centre in time)
    Contour,          // 8: level after the peak (dB, -60..0) around 20, 40, ... 2560 ms
    PitchConfidence = Contour + 8,  // 0..1: how much of the sound is periodic
    Pitch,            // confidence * log2(f0 / 220 Hz)
    OnsetRate,        // log2(1 + onsets per second after the first)
    Length,           // log10(the file's seconds)
    Count
};
}  // namespace feature

inline constexpr size_t kDims = feature::Count;
inline constexpr int kMfccs = 12;
inline constexpr int kParts = 3;      // attack, body, tail
inline constexpr int kPartMfccs = 4;
inline constexpr int kContourPoints = 8;

// Bump when what SoundAnalyzer computes changes: fingerprints saved by an
// earlier version are then made again.
inline constexpr uint32_t kFeatureVersion = 1;

// How much of a sound is analysed, from where it starts.
inline constexpr double kAnalysisSeconds = 6.0;
// How much more is decoded, for leading silence before the start.
inline constexpr double kLeadInSeconds = 0.5;

using Fingerprint = std::array<float, kDims>;

struct FeatureInfo {
    const char* name;
    Aspect aspect;
    // A small spread for it: the comparison never divides a difference by less
    // (a library whose sounds hardly differ in it doesn't magnify noise).
    float minSpread;
};
const std::array<FeatureInfo, kDims>& featureInfo();
const char* aspectName(Aspect aspect);

// Makes fingerprints. Holds its FFTs and buffers: one per thread.
class SoundAnalyzer {
public:
    SoundAnalyzer();
    ~SoundAnalyzer();
    SoundAnalyzer(const SoundAnalyzer&) = delete;
    SoundAnalyzer& operator=(const SoundAnalyzer&) = delete;

    // The fingerprint of mono samples at `sampleRate`; `fileSeconds` is the
    // whole file's length (<= 0: the samples' own), `truncated` whether the
    // sound goes on after them. None if they are silent.
    std::optional<Fingerprint> analyze(const float* samples, size_t count, uint32_t sampleRate, double fileSeconds = 0.0,
                                       bool truncated = false);

    // Decodes a file (UTF-8, the system's form) and analyses it: from `start`
    // seconds in, `length` seconds of it (< 0: to the end). None if silent;
    // throws AudioError if it can't be decoded.
    std::optional<Fingerprint> analyzeFile(const std::string& path, double start = 0.0, double length = -1.0);

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace sub::intelligence
