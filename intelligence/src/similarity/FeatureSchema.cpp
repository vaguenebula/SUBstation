#include "similarity/FeatureSchema.h"

namespace sub::intelligence {

const char* aspectName(Aspect aspect) {
    switch (aspect) {
        case Aspect::Timbre: return "timbre";
        case Aspect::TimbreMotion: return "timbreMotion";
        case Aspect::Spectrum: return "spectrum";
        case Aspect::Envelope: return "envelope";
        case Aspect::Pitch: return "pitch";
        case Aspect::Rhythm: return "rhythm";
        case Aspect::TimbreSpread: return "timbreSpread";
        case Aspect::Contrast: return "contrast";
        case Aspect::SpectralShape: return "spectralShape";
        case Aspect::Tonality: return "tonality";
        case Aspect::Embedding: return "embedding";
    }
    return "";
}

AspectWeights AspectWeights::only(Aspect aspect) {
    AspectWeights w;
    w.weight.fill(0.f);
    w[aspect] = 1.f;
    return w;
}

uint64_t FeatureSchema::key() const {
    uint64_t h = 1469598103934665603ull;  // FNV-1a
    auto add = [&h](const void* data, size_t size) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < size; ++i) {
            h ^= p[i];
            h *= 1099511628211ull;
        }
    };
    auto text = [&](const std::string& s) {
        add(s.data(), s.size());
        add("", 1);  // (a separator)
    };
    text(extractor);
    const unsigned char v[4] = {static_cast<unsigned char>(version), static_cast<unsigned char>(version >> 8),
                                static_cast<unsigned char>(version >> 16), static_cast<unsigned char>(version >> 24)};
    add(v, 4);
    text(settings);
    for (const FeatureInfo& f : features) {
        text(f.name);
        const auto aspect = static_cast<unsigned char>(f.aspect);
        add(&aspect, 1);
    }
    return h;
}

}  // namespace sub::intelligence
