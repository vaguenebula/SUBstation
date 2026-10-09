// What a fingerprint holds, as the extractor that makes it describes it: its
// features in order, each with a name, the aspect it belongs to and a small
// spread; the aspects' weights tuned for them; and what identifies the
// fingerprints it makes (its name, its version, its settings), so saved ones
// are used only by the extractor that made them, as it was.
//
// Features are grouped into *aspects*, which comparing weighs as wholes
// (Similarity.h): an aspect counts by its weight however many features it has,
// so an extractor can describe one aspect finely without it outweighing the
// others. Embedding is for learned embeddings (a model's output, or a
// projection of it) added later as an aspect of their own.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sub::intelligence {

enum class Aspect : uint8_t {
    Timbre,
    TimbreMotion,
    Spectrum,
    Envelope,
    Pitch,
    Rhythm,
    TimbreSpread,
    Contrast,
    SpectralShape,
    Tonality,
    Embedding,
};
inline constexpr size_t kAspects = 11;

const char* aspectName(Aspect aspect);

// How much each aspect counts (relative to the others; 0 leaves it out).
struct AspectWeights {
    std::array<float, kAspects> weight{1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f};

    float& operator[](Aspect aspect) { return weight[static_cast<size_t>(aspect)]; }
    float operator[](Aspect aspect) const { return weight[static_cast<size_t>(aspect)]; }
    static AspectWeights only(Aspect aspect);
};

struct FeatureInfo {
    std::string name;
    Aspect aspect = Aspect::Timbre;
    // A small spread for it: the comparison never divides a difference by less
    // (a library whose sounds hardly differ in it doesn't magnify noise).
    float minSpread = 1.f;
};

struct FeatureSchema {
    std::string extractor;  // "builtin", "essentia"...
    uint32_t version = 1;   // of what it computes: bumped whenever that changes
    std::string settings;   // its extraction settings, as text ("rate=44100 frame=1024 ...")
    std::vector<FeatureInfo> features;
    AspectWeights weights;  // tuned for these features (sound_similarity_bench --tune)

    size_t dims() const { return features.size(); }
    // What saved fingerprints must match to be used (FNV-1a of the extractor,
    // its version, its settings and the features' names and aspects).
    uint64_t key() const;
};

}  // namespace sub::intelligence
