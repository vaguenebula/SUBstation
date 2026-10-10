#include "similarity/FeatureSchema.h"

#include "platform/Bytes.h"

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
    uint64_t h = platform::kFnvOffsetBasis;
    auto text = [&h](const std::string& s) {
        h = platform::fnv1a(s.data(), s.size() + 1, h);  // (with its terminating 0: a separator)
    };
    text(extractor);
    const unsigned char v[4] = {static_cast<unsigned char>(version), static_cast<unsigned char>(version >> 8),
                                static_cast<unsigned char>(version >> 16), static_cast<unsigned char>(version >> 24)};
    h = platform::fnv1a(v, 4, h);
    text(settings);
    for (const FeatureInfo& f : features) {
        text(f.name);
        const auto aspect = static_cast<unsigned char>(f.aspect);
        h = platform::fnv1a(&aspect, 1, h);
    }
    return h;
}

}  // namespace sub::intelligence
