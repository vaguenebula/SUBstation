#include "core/AudioReader.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <numbers>
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

// Decodes at the rate `rateFor` picks for the file's own (0: its own).
template <typename RateFor>
MonoAudio decodeMono(const std::string& path, double startSeconds, double maxSeconds, RateFor rateFor) {
    Decoder native(path, ma_decoder_config_init(ma_format_f32, 0, 0));
    const uint32_t channels = native.get()->outputChannels;
    const uint32_t rate = native.get()->outputSampleRate;
    if (channels == 0 || rate == 0) throw AudioError("no audio");
    const uint32_t wantedRate = rateFor(rate);
    const uint32_t outRate = wantedRate ? wantedRate : rate;
    std::optional<Decoder> resampled;  // only at another rate than the file's
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

}  // namespace

MonoAudio readMono(const std::string& path, double startSeconds, double maxSeconds, uint32_t maxSampleRate) {
    return decodeMono(path, startSeconds, maxSeconds, [maxSampleRate](uint32_t rate) { return std::min(rate, maxSampleRate); });
}

MonoAudio readMonoAt(const std::string& path, double startSeconds, double maxSeconds, uint32_t sampleRate) {
    MonoAudio audio = readMono(path, startSeconds, maxSeconds, std::numeric_limits<uint32_t>::max());
    if (audio.sampleRate != sampleRate) {
        audio.samples = resampleMono(audio.samples.data(), audio.samples.size(), audio.sampleRate, sampleRate);
        audio.sampleRate = sampleRate;
        if (audio.samples.empty()) throw AudioError("no audio");
    }
    return audio;
}

namespace {

// Band-limited interpolation (J. O. Smith's): each output sample is the input
// convolved with a sinc, windowed (Kaiser) to kZeroCrossings zero crossings on
// either side, its cutoff at kPassband of the lower rate's Nyquist frequency.
// The kernel is tabulated once, kTableSteps points a zero crossing, and read
// with linear interpolation.
constexpr int kZeroCrossings = 16;
constexpr int kTableSteps = 512;
constexpr double kPassband = 0.97;
constexpr double kKaiserBeta = 9.0;  // (about -90 dB beyond the cutoff)

double besselI0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 50; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

const std::vector<float>& sincTable() {
    static const std::vector<float> table = [] {
        std::vector<float> t(static_cast<size_t>(kZeroCrossings * kTableSteps) + 2, 0.f);
        const double norm = besselI0(kKaiserBeta);
        for (size_t i = 0; i < t.size(); ++i) {
            const double u = static_cast<double>(i) / kTableSteps;  // in zero crossings
            const double r = u / kZeroCrossings;
            if (r > 1.0) break;
            const double sinc = u == 0.0 ? 1.0 : std::sin(std::numbers::pi * u) / (std::numbers::pi * u);
            t[i] = static_cast<float>(sinc * besselI0(kKaiserBeta * std::sqrt(1.0 - r * r)) / norm);
        }
        return t;
    }();
    return table;
}

}  // namespace

std::vector<float> resampleMono(const float* samples, size_t count, uint32_t from, uint32_t to) {
    if (from == to || count == 0 || from == 0 || to == 0) return std::vector<float>(samples, samples + count);
    const std::vector<float>& table = sincTable();
    // The kernel's time scale: zero crossings per input sample (stretched when
    // going down, so its cutoff is the output's Nyquist frequency).
    const double scale = std::min(1.0, static_cast<double>(to) / from) * kPassband;
    const double reach = kZeroCrossings / scale;  // input samples on either side
    const double step = static_cast<double>(from) / to;
    const auto wanted = static_cast<size_t>(std::llround(static_cast<double>(count) * to / from));
    std::vector<float> out(wanted);
    const auto last = static_cast<ptrdiff_t>(count) - 1;
    for (size_t j = 0; j < wanted; ++j) {
        const double t = static_cast<double>(j) * step;
        const auto first = std::max<ptrdiff_t>(0, static_cast<ptrdiff_t>(std::ceil(t - reach)));
        const auto end = std::min<ptrdiff_t>(last, static_cast<ptrdiff_t>(std::floor(t + reach)));
        double sum = 0.0;
        for (ptrdiff_t k = first; k <= end; ++k) {
            const double u = std::fabs(t - static_cast<double>(k)) * scale * kTableSteps;
            const auto i = static_cast<size_t>(u);
            const double a = u - static_cast<double>(i);
            const double w = table[i] + a * (table[i + 1] - table[i]);
            sum += w * samples[k];
        }
        const auto y = static_cast<float>(sum * scale);
        out[j] = std::isfinite(y) ? y : 0.f;
    }
    return out;
}

}  // namespace sub::intelligence
