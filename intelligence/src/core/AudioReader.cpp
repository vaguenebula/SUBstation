#include "core/AudioReader.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <memory>
#include <numbers>
#include <numeric>
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

MonoAudio readMonoAt(const std::string& path, double startSeconds, double maxSeconds, uint32_t sampleRate,
                     const std::atomic<bool>* cancel) {
    MonoAudio audio = readMono(path, startSeconds, maxSeconds, std::numeric_limits<uint32_t>::max());
    if (audio.sampleRate != sampleRate) {
        audio.samples = resampleMono(audio.samples.data(), audio.samples.size(), audio.sampleRate, sampleRate, cancel);
        audio.sampleRate = sampleRate;
        if (audio.samples.empty() && !(cancel && cancel->load(std::memory_order_relaxed))) throw AudioError("no audio");
    }
    return audio;
}

namespace {

// Band-limited interpolation (J. O. Smith's): each output sample is the input
// convolved with a sinc, windowed (Kaiser) to kZeroCrossings zero crossings on
// either side, its cutoff at kPassband of the lower rate's Nyquist frequency.
// The kernel is tabulated once, kTableSteps points a zero crossing, and read
// with linear interpolation into a filter for each phase an output sample can
// fall at between two input samples (polyphase): `to` / gcd(from, to) of them.
constexpr int kZeroCrossings = 16;
constexpr int kTableSteps = 512;
constexpr double kPassband = 0.97;
constexpr double kKaiserBeta = 9.0;  // (about -90 dB beyond the cutoff)
constexpr int kLanes = 8;                       // taps summed side by side (each phase's filter padded to them)
constexpr uint64_t kMaxPhases = 1024;           // more (an odd rate: 44101 Hz), and each output's filter is made for it
constexpr uint64_t kMaxPhaseFloats = 1u << 22;  // (16 MB)
constexpr size_t kCancelEvery = 8192;           // output samples between looks at the cancel flag

double besselI0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 50; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

const std::vector<double>& sincTable() {
    static const std::vector<double> table = [] {
        std::vector<double> t(static_cast<size_t>(kZeroCrossings * kTableSteps) + 2, 0.0);
        const double norm = besselI0(kKaiserBeta);
        for (size_t i = 0; i < t.size(); ++i) {
            const double u = static_cast<double>(i) / kTableSteps;  // in zero crossings
            const double r = u / kZeroCrossings;
            if (r > 1.0) break;
            const double sinc = u == 0.0 ? 1.0 : std::sin(std::numbers::pi * u) / (std::numbers::pi * u);
            t[i] = sinc * besselI0(kKaiserBeta * std::sqrt(1.0 - r * r)) / norm;
        }
        return t;
    }();
    return table;
}

// The filter from one rate to another.
struct Polyphase {
    uint32_t from = 0, to = 0;
    uint64_t up = 0, down = 0;  // to and from over their greatest common divisor
    double scale = 0.0;         // the kernel's time scale: zero crossings per input sample
    int half = 0;               // input samples either side of an output's
    size_t taps = 0;            // 2 * half + 1, padded to kLanes
    std::vector<float> phases;  // `up` filters of `taps`, if not too many

    Polyphase(uint32_t f, uint32_t t) : from(f), to(t) {
        const uint64_t g = std::gcd(f, t);
        up = t / g;
        down = f / g;
        // (Stretched when going down, so the cutoff is the output's Nyquist frequency.)
        scale = std::min(1.0, static_cast<double>(t) / f) * kPassband;
        half = static_cast<int>(std::ceil(kZeroCrossings / scale));
        taps = (static_cast<size_t>(2 * half + 1) + kLanes - 1) / kLanes * kLanes;
        if (up <= kMaxPhases && up * taps <= kMaxPhaseFloats) {
            phases.resize(static_cast<size_t>(up) * taps);
            for (uint64_t p = 0; p < up; ++p) filter(static_cast<double>(p) / static_cast<double>(up), &phases[p * taps]);
        }
    }

    // The filter for an output `fraction` of an input sample after input n: tap
    // k weighs input n - half + k.
    void filter(double fraction, float* out) const {
        const std::vector<double>& table = sincTable();
        for (size_t k = 0; k < taps; ++k) {
            const double u = std::fabs(static_cast<double>(static_cast<int>(k) - half) - fraction) * scale * kTableSteps;
            double w = 0.0;
            if (u < kZeroCrossings * kTableSteps) {
                const auto i = static_cast<size_t>(u);
                w = table[i] + (u - static_cast<double>(i)) * (table[i + 1] - table[i]);
            }
            out[k] = static_cast<float>(w * scale);
        }
    }
};

float dot(const float* a, const float* b, size_t n) {
    float lanes[kLanes] = {};
    for (size_t k = 0; k < n; k += kLanes)
        for (int i = 0; i < kLanes; ++i) lanes[i] += a[k + i] * b[k + i];
    float sum = 0.f;
    for (const float v : lanes) sum += v;
    return sum;
}

}  // namespace

std::vector<float> resampleMono(const float* samples, size_t count, uint32_t from, uint32_t to,
                                const std::atomic<bool>* cancel) {
    if (from == to || count == 0 || from == 0 || to == 0) return std::vector<float>(samples, samples + count);
    // The last rates' filter, kept: a thread resamples one library's files, mostly at one rate.
    thread_local std::unique_ptr<Polyphase> kept;
    if (!kept || kept->from != from || kept->to != to) kept = std::make_unique<Polyphase>(from, to);
    const Polyphase& f = *kept;

    const auto wanted = static_cast<size_t>(std::llround(static_cast<double>(count) * to / from));
    // The input with `half` zeros before it, and enough after it for the last
    // output's filter: output j is at input j * down / up.
    const size_t lastInput = wanted ? static_cast<size_t>((wanted - 1) * f.down / f.up) : 0;
    std::vector<float> padded(std::max(count, lastInput + 1) + f.taps + static_cast<size_t>(f.half), 0.f);
    std::copy(samples, samples + count, padded.begin() + f.half);
    std::vector<float> scratch(f.phases.empty() ? f.taps : 0);
    std::vector<float> out(wanted);
    for (size_t j = 0; j < wanted; ++j) {
        if (j % kCancelEvery == 0 && cancel && cancel->load(std::memory_order_relaxed)) return {};
        const uint64_t position = j * f.down;
        const auto n = static_cast<size_t>(position / f.up);
        const uint64_t phase = position % f.up;
        const float* filter = scratch.data();
        if (f.phases.empty())
            f.filter(static_cast<double>(phase) / static_cast<double>(f.up), scratch.data());
        else
            filter = &f.phases[phase * f.taps];
        const float y = dot(filter, padded.data() + n, f.taps);  // (padded[n] is input n - half)
        out[j] = std::isfinite(y) ? y : 0.f;
    }
    return out;
}

}  // namespace sub::intelligence
