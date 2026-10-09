#include "similarity/FeatureExtractor.h"

#include <algorithm>

#include "similarity/EssentiaExtractor.h"

namespace sub::intelligence {

MonoAudio FeatureExtractor::decode(const std::string& path, double start, double seconds) {
    return readMono(path, start, seconds);
}

Extraction FeatureExtractor::extractFile(const std::string& path, double start, double length, float* out,
                                         const CancelFlag* cancel) {
    if (cancel && cancel->load(std::memory_order_relaxed)) return Extraction::Cancelled;
    const double wanted = kLeadInSeconds + kAnalysisSeconds;
    const double seconds = length < 0.0 ? wanted : std::min(length, wanted);
    const MonoAudio audio = decode(path, start, seconds);
    SoundBuffer sound;
    sound.samples = audio.samples.data();
    sound.count = audio.samples.size();
    sound.sampleRate = audio.sampleRate;
    sound.fileSeconds = length >= 0.0 ? length : audio.fileSeconds - std::max(0.0, start);
    sound.truncated = length >= 0.0 ? length > seconds : audio.truncated;
    return extract(sound, out, cancel);
}

std::vector<float> FeatureExtractor::extractFile(const std::string& path, double start, double length) {
    std::vector<float> out(schema().dims());
    if (extractFile(path, start, length, out.data()) != Extraction::Done) out.clear();
    return out;
}

ExtractorFactory defaultExtractorFactory() {
    return [] { return std::make_unique<EssentiaExtractor>(); };
}

}  // namespace sub::intelligence
