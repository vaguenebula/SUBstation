#include "AudioSource.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

#include "PathUtils.h"
#include "miniaudio.h"

namespace sub {
namespace {

// RAII wrapper so every error path uninitialises the decoder.
class Decoder {
public:
    Decoder(const std::string& utf8Path, const ma_decoder_config& config) {
        const ma_result result = ma_decoder_init_file_w(widen(utf8Path).c_str(), &config, &decoder_);
        if (result != MA_SUCCESS) {
            throw std::runtime_error("Could not open audio file '" + utf8Path + "': " + ma_result_description(result));
        }
    }
    ~Decoder() { ma_decoder_uninit(&decoder_); }
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    ma_decoder* get() { return &decoder_; }

private:
    ma_decoder decoder_{};
};

}  // namespace

AudioFileInfo AudioSource::probe(const std::string& utf8Path) {
    const ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
    Decoder decoder(utf8Path, config);
    AudioFileInfo info;
    info.channels = decoder.get()->outputChannels;
    info.sampleRate = decoder.get()->outputSampleRate;
    ma_uint64 length = 0;
    if (ma_decoder_get_length_in_pcm_frames(decoder.get(), &length) != MA_SUCCESS || length == 0) {
        // Some streams do not report a length; count the frames instead.
        std::vector<float> scratch(4096 * std::max<uint32_t>(info.channels, 1));
        length = 0;
        for (;;) {
            ma_uint64 read = 0;
            ma_decoder_read_pcm_frames(decoder.get(), scratch.data(), 4096, &read);
            if (read == 0) break;
            length += read;
        }
    }
    info.frames = static_cast<int64_t>(length);
    info.duration = info.sampleRate ? static_cast<double>(length) / info.sampleRate : 0.0;
    return info;
}

std::shared_ptr<AudioSource> AudioSource::load(const std::string& utf8Path, uint32_t sampleRate) {
    if (sampleRate == 0) throw std::invalid_argument("sample rate must be positive");

    uint32_t nativeChannels = 0;
    uint32_t nativeRate = 0;
    {
        const ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
        Decoder probe(utf8Path, config);
        nativeChannels = probe.get()->outputChannels;
        nativeRate = probe.get()->outputSampleRate;
    }
    const uint32_t channels = nativeChannels >= 2 ? 2 : 1;

    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, channels, sampleRate);
    config.resampling.linear.lpfOrder = MA_MAX_FILTER_ORDER;
    Decoder decoder(utf8Path, config);

    ma_uint64 expected = 0;
    ma_decoder_get_length_in_pcm_frames(decoder.get(), &expected);

    std::vector<std::vector<float>> planar(channels);
    for (auto& ch : planar) ch.reserve(static_cast<size_t>(expected));

    constexpr ma_uint64 kChunk = 16384;
    std::vector<float> interleaved(kChunk * channels);
    for (;;) {
        ma_uint64 read = 0;
        const ma_result result = ma_decoder_read_pcm_frames(decoder.get(), interleaved.data(), kChunk, &read);
        for (uint32_t c = 0; c < channels; ++c) {
            auto& out = planar[c];
            const size_t base = out.size();
            out.resize(base + static_cast<size_t>(read));
            for (ma_uint64 i = 0; i < read; ++i) out[base + i] = interleaved[i * channels + c];
        }
        if (read < kChunk || result != MA_SUCCESS) break;
    }

    auto source = std::shared_ptr<AudioSource>(new AudioSource());
    source->path_ = utf8Path;
    source->channels_ = channels;
    source->sampleRate_ = sampleRate;
    source->fileSampleRate_ = nativeRate;
    source->frames_ = static_cast<int64_t>(planar[0].size());
    if (source->frames_ == 0) throw std::runtime_error("Audio file '" + utf8Path + "' contains no audio");

    source->data_.resize(static_cast<size_t>(source->frames_) * channels);
    for (uint32_t c = 0; c < channels; ++c) {
        std::copy(planar[c].begin(), planar[c].end(), source->data_.begin() + static_cast<size_t>(c) * source->frames_);
    }
    source->buildPeaks();
    return source;
}

int AudioSource::samplesPerPeak(int level) { return kBaseSamplesPerPeak << (2 * level); }

int64_t AudioSource::numPeaks(int level) const {
    if (level < 0 || level >= kNumPeakLevels) return 0;
    return static_cast<int64_t>(peaks_[level].size() / (static_cast<size_t>(channels_) * 2));
}

const float* AudioSource::peaks(int level) const {
    if (level < 0 || level >= kNumPeakLevels) return nullptr;
    return peaks_[level].data();
}

void AudioSource::buildPeaks() {
    peaks_.assign(kNumPeakLevels, {});
    const int64_t spp = kBaseSamplesPerPeak;
    const int64_t count0 = (frames_ + spp - 1) / spp;
    auto& level0 = peaks_[0];
    level0.resize(static_cast<size_t>(count0) * channels_ * 2);
    for (uint32_t c = 0; c < channels_; ++c) {
        const float* samples = channelData(c);
        for (int64_t p = 0; p < count0; ++p) {
            const int64_t begin = p * spp;
            const int64_t end = std::min(frames_, begin + spp);
            float lo = std::numeric_limits<float>::max();
            float hi = std::numeric_limits<float>::lowest();
            for (int64_t i = begin; i < end; ++i) {
                lo = std::min(lo, samples[i]);
                hi = std::max(hi, samples[i]);
            }
            const size_t at = (static_cast<size_t>(p) * channels_ + c) * 2;
            level0[at] = lo;
            level0[at + 1] = hi;
        }
    }
    // Each coarser level combines four peaks of the previous one.
    for (int level = 1; level < kNumPeakLevels; ++level) {
        const auto& prev = peaks_[level - 1];
        const int64_t prevCount = static_cast<int64_t>(prev.size() / (static_cast<size_t>(channels_) * 2));
        const int64_t count = (prevCount + 3) / 4;
        auto& cur = peaks_[level];
        cur.resize(static_cast<size_t>(count) * channels_ * 2);
        for (int64_t p = 0; p < count; ++p) {
            for (uint32_t c = 0; c < channels_; ++c) {
                float lo = std::numeric_limits<float>::max();
                float hi = std::numeric_limits<float>::lowest();
                for (int64_t q = p * 4; q < std::min(prevCount, p * 4 + 4); ++q) {
                    const size_t at = (static_cast<size_t>(q) * channels_ + c) * 2;
                    lo = std::min(lo, prev[at]);
                    hi = std::max(hi, prev[at + 1]);
                }
                const size_t at = (static_cast<size_t>(p) * channels_ + c) * 2;
                cur[at] = lo;
                cur[at + 1] = hi;
            }
        }
    }
}

}  // namespace sub
