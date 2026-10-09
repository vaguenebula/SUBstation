// What makes fingerprints: a FeatureExtractor turns a sound (mono PCM, or a
// file decoded for it) into its schema's fixed number of features. The index
// takes a factory of them (SoundIndexOptions::extractor), one extractor per
// thread, each made on its thread; the one SUBstation uses is
// EssentiaExtractor (EssentiaExtractor.h).
//
// Another is a matter of writing one and handing its factory over: a learned
// embedding, say, as an extractor whose schema has an Embedding aspect beside
// Essentia's descriptors (which tell one kick from another better than
// embeddings trained on what a sound *is*). Nothing else changes: the index,
// the store and the comparison follow the schema, and fingerprints saved by
// another extractor are made again.
//
// An extractor holds its buffers and algorithms: one per thread. Extraction
// polls `cancel` and gives up (Cancelled) when it is set, so closing the index
// or replacing a search doesn't wait for a long file.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/AudioReader.h"
#include "similarity/FeatureSchema.h"

namespace sub::intelligence {

// How much of a sound is analysed, from where it starts (its leading silence
// skipped), and how much more is decoded for that silence.
inline constexpr double kAnalysisSeconds = 6.0;
inline constexpr double kLeadInSeconds = 0.5;

// Mono PCM to extract from.
struct SoundBuffer {
    const float* samples = nullptr;
    size_t count = 0;
    uint32_t sampleRate = 0;
    double fileSeconds = 0.0;  // the whole sound's length (<= 0: the samples')
    bool truncated = false;    // whether the sound goes on after the samples
};

enum class Extraction : uint8_t {
    Done,
    Silent,     // nothing to hear (below -100 dBFS): no fingerprint
    Cancelled,  // `cancel` was set
};

using CancelFlag = std::atomic<bool>;

class FeatureExtractor {
public:
    virtual ~FeatureExtractor() = default;

    virtual const FeatureSchema& schema() const = 0;

    // Writes schema().dims() features to `out` (when Done). Any sample rate,
    // any length: very short sounds are padded, not refused.
    virtual Extraction extract(const SoundBuffer& sound, float* out, const CancelFlag* cancel = nullptr) = 0;

    // Decodes a file (UTF-8, the system's form) and extracts: from `start`
    // seconds in, `length` seconds of it (< 0: to the end), as much as is
    // analysed. Throws AudioError if it can't be decoded.
    Extraction extractFile(const std::string& path, double start, double length, float* out,
                           const CancelFlag* cancel = nullptr);

    // The same, into a vector (empty unless Done).
    std::vector<float> extractFile(const std::string& path, double start = 0.0, double length = -1.0);

protected:
    // Decodes `seconds` of the file from `start` (mono): at its own rate up to
    // 48 kHz, unless the extractor analyses at a rate of its own. Gives up
    // (returns no samples) when `cancel` is set.
    virtual MonoAudio decode(const std::string& path, double start, double seconds, const CancelFlag* cancel);
};

// Makes extractors: the schema they make fingerprints of is known before any
// is made (an extractor's set-up, its algorithms or a model, is the work of the
// thread that uses it, not of whoever creates the index).
struct ExtractorFactory {
    FeatureSchema schema;
    std::function<std::unique_ptr<FeatureExtractor>()> make;

    explicit operator bool() const { return static_cast<bool>(make); }
    // One extractor. Throws (std::runtime_error, or what `make` throws) if it
    // makes none, or one of another schema.
    std::unique_ptr<FeatureExtractor> operator()() const;
};

// Essentia's (EssentiaExtractor).
ExtractorFactory defaultExtractorFactory();

}  // namespace sub::intelligence
