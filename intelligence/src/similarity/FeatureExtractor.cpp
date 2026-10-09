#include "similarity/FeatureExtractor.h"

#include <algorithm>
#include <stdexcept>

#include "similarity/EssentiaExtractor.h"

namespace sub::intelligence {

MonoAudio FeatureExtractor::decode(const std::string& path, double start, double seconds, const CancelFlag*) {
    return readMono(path, start, seconds);
}

Extraction FeatureExtractor::extractFile(const std::string& path, double start, double length, float* out,
                                         const CancelFlag* cancel) {
    if (cancel && cancel->load(std::memory_order_relaxed)) return Extraction::Cancelled;
    const double wanted = kLeadInSeconds + kAnalysisSeconds;
    const double seconds = length < 0.0 ? wanted : std::min(length, wanted);
    const MonoAudio audio = decode(path, start, seconds, cancel);
    if (cancel && cancel->load(std::memory_order_relaxed)) return Extraction::Cancelled;
    SoundBuffer sound;
    sound.samples = audio.samples.data();
    sound.count = audio.samples.size();
    sound.sampleRate = audio.sampleRate;
    // A part is as long as asked, or to the file's end if that comes first;
    // it goes on after what was decoded if it is longer and the file does too.
    const double toEnd = std::max(0.0, audio.fileSeconds - std::max(0.0, start));
    sound.fileSeconds = length >= 0.0 ? std::min(length, toEnd) : toEnd;
    sound.truncated = audio.truncated && (length < 0.0 || length > seconds);
    return extract(sound, out, cancel);
}

std::vector<float> FeatureExtractor::extractFile(const std::string& path, double start, double length) {
    std::vector<float> out(schema().dims());
    if (extractFile(path, start, length, out.data()) != Extraction::Done) out.clear();
    return out;
}

std::unique_ptr<FeatureExtractor> ExtractorFactory::operator()() const {
    if (!make) throw std::runtime_error("no extractor");
    std::unique_ptr<FeatureExtractor> extractor = make();
    if (!extractor) throw std::runtime_error("no extractor");
    if (extractor->schema().key() != schema.key())
        throw std::runtime_error("the extractor makes " + extractor->schema().extractor + "'s fingerprints, not " +
                                 schema.extractor + "'s");
    return extractor;
}

ExtractorFactory defaultExtractorFactory() {
    return {essentiaSchema(), [] { return std::make_unique<EssentiaExtractor>(); }};
}

}  // namespace sub::intelligence
