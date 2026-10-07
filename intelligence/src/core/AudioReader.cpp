#include "core/AudioReader.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>

#include "core/Platform.h"
#include "miniaudio.h"

namespace sub::intelligence {

namespace {

class Decoder {
public:
    Decoder(const std::string& path, const ma_decoder_config& config) {
#ifdef _WIN32
        const ma_result result = ma_decoder_init_file_w(platform::toWide(path).c_str(), &config, &decoder_);
#else
        const ma_result result = ma_decoder_init_file(path.c_str(), &config, &decoder_);
#endif
        if (result != MA_SUCCESS) throw AudioError(std::string("cannot decode: ") + ma_result_description(result));
    }
    ~Decoder() { ma_decoder_uninit(&decoder_); }
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    ma_decoder* get() { return &decoder_; }

private:
    ma_decoder decoder_{};
};

// MP3 has no length in its header: asking for one decodes the whole file.
bool isMp3(const std::string& path) {
    if (path.size() < 4) return false;
    std::string ext = path.substr(path.size() - 4);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".mp3";
}

}  // namespace

MonoAudio readMono(const std::string& path, double startSeconds, double maxSeconds, uint32_t maxSampleRate) {
    Decoder native(path, ma_decoder_config_init(ma_format_f32, 0, 0));
    const uint32_t channels = native.get()->outputChannels;
    const uint32_t rate = native.get()->outputSampleRate;
    if (channels == 0 || rate == 0) throw AudioError("no audio");
    const uint32_t outRate = std::min(rate, maxSampleRate);
    std::optional<Decoder> resampled;  // only above the highest rate analysed
    if (outRate != rate) {
        ma_decoder_config config = ma_decoder_config_init(ma_format_f32, channels, outRate);
        config.resampling.linear.lpfOrder = MA_MAX_FILTER_ORDER;
        resampled.emplace(path, config);
    }
    ma_decoder* decoder = resampled ? resampled->get() : native.get();

    MonoAudio audio;
    audio.sampleRate = outRate;
    ma_uint64 length = 0;
    const bool knowsLength =
        !isMp3(path) && ma_decoder_get_length_in_pcm_frames(decoder, &length) == MA_SUCCESS && length > 0;
    const auto start = static_cast<ma_uint64>(std::max(0.0, startSeconds) * outRate + 0.5);
    if (start > 0 && ma_decoder_seek_to_pcm_frame(decoder, start) != MA_SUCCESS) throw AudioError("cannot seek");
    const auto wanted = static_cast<size_t>(std::max(0.0, maxSeconds) * outRate + 0.5);
    audio.samples.reserve(knowsLength ? std::min<size_t>(wanted, static_cast<size_t>(length)) : wanted);

    constexpr ma_uint64 kChunk = 8192;
    std::vector<float> interleaved(static_cast<size_t>(kChunk) * channels);
    const float scale = 1.f / static_cast<float>(channels);
    while (audio.samples.size() < wanted) {
        const ma_uint64 ask = std::min<ma_uint64>(kChunk, wanted - audio.samples.size());
        ma_uint64 read = 0;
        const ma_result result = ma_decoder_read_pcm_frames(decoder, interleaved.data(), ask, &read);
        for (ma_uint64 i = 0; i < read; ++i) {
            float sum = 0.f;
            for (uint32_t c = 0; c < channels; ++c) sum += interleaved[static_cast<size_t>(i) * channels + c];
            // A broken file's NaN or infinity would poison every feature.
            audio.samples.push_back(std::isfinite(sum) ? sum * scale : 0.f);
        }
        if (read < ask || result != MA_SUCCESS) break;
    }
    if (audio.samples.empty()) throw AudioError("no audio");
    const double decodedEnd = static_cast<double>(start + audio.samples.size()) / outRate;
    if (knowsLength) {
        audio.fileSeconds = static_cast<double>(length) / outRate;
        audio.truncated = decodedEnd + 1e-9 < audio.fileSeconds;
    } else {
        audio.fileSeconds = decodedEnd;
        audio.truncated = audio.samples.size() >= wanted;
    }
    return audio;
}

}  // namespace sub::intelligence
