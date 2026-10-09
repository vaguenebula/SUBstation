// Built-in "Delay" device, after Ableton's: a stereo delay with a time per
// side (synced to the tempo in sixteenths, with a swing-like offset, or free
// in ms; the right side can follow the left), feedback (or freezing what is in
// the delay), a band-pass filter on the echoes, ping pong, and three ways of
// changing the time: Repitch glides to it (pitching what is in the delay, as a tape
// delay does), Fade crossfades to it, Jump switches at once.
//
// Signal flow per side: what the delay line holds is read (`echo`), filtered
// (`wet`: the output, and what feeds back), and the line is written with the
// input plus feedback times wet. Frozen, the line is written with its own
// echo instead (unfiltered, so the loop doesn't fade) and the input is ignored.
// Ping pong: the input, summed to mono, goes into the left line only, and each
// side feeds the other.
//
// Its editor draws a spectrum of the input behind the filter's curve: the
// display "input" is the input summed to mono, one value per sample.

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>
#include <vector>

#include "builtin/BuiltinProcessor.h"
#include "builtin/BuiltinRegistry.h"
#include "builtin/Dsp.h"
#include "rt/RtUtils.h"

namespace sub {
namespace {

constexpr float kPi = std::numbers::pi_v<float>;
constexpr double kMaxDelaySeconds = 10.0;  // the longest time either side can be set to
constexpr float kDivisions[] = {1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 8.f, 16.f};  // in sixteenths

class DelayProcessor final : public BuiltinProcessor {
public:
    enum Param {
        LeftSync = 0, LeftDivision, LeftTime, LeftOffset,
        RightSync, RightDivision, RightTime, RightOffset,
        Link, Feedback, Freeze,
        FilterOn, FilterFreq, FilterWidth,
        Mode, PingPong, Mix,
        NumParams
    };
    enum TimeMode { Repitch = 0, Fade, Jump };

    enum Display { Input = 0 };

    DelayProcessor() : BuiltinProcessor(infos(), {{"input", 1}}) {}

    std::string typeId() const override { return "builtin:delay"; }
    std::string name() const override { return "Delay"; }

    // Frozen it rings for ever; otherwise until the feedback has taken it down 60 dB.
    int tailSamples() const override {
        const double longest = std::max(timeSeconds(0, 120.0), timeSeconds(1, 120.0));
        const float feedback = param(Feedback) / 100.f;
        const double repeats = feedback > 0.001f ? std::min(200.0, -3.0 / std::log10(feedback)) : 1.0;
        return static_cast<int>(std::min(60.0, longest * (repeats + 1.0)) * sampleRate_);
    }

    void prepare(double sampleRate, int) override {
        sampleRate_ = sampleRate;
        const size_t size = std::bit_ceil(static_cast<size_t>(kMaxDelaySeconds * sampleRate) + 8);
        for (auto& line : lines_) line.assign(size, 0.f);
        mask_ = size - 1;
        mix_.reset(sampleRate, 0.02);
        feedback_.reset(sampleRate, 0.02);
        glide_ = static_cast<float>(onePoleCoefficient(0.12, sampleRate));
        fadeLength_ = std::max(1, static_cast<int>(0.06 * sampleRate));
        reset();
    }

    void reset() override {
        for (auto& line : lines_) std::fill(line.begin(), line.end(), 0.f);
        writePos_ = 0;
        for (auto& side : sides_) side = Side{};
        for (auto& f : filters_) f.reset();
        mix_.snapTo(param(Mix) / 100.f);  // (silent now: no need to ramp)
        feedback_.snapTo(param(Feedback) / 100.f);
    }

protected:
    void render(const ProcessContext& ctx, float* const* ch, int numChannels, int numFrames) override {
        const int n = std::min(numChannels, 2);
        if (n <= 0 || lines_[0].empty()) return;
        const auto mode = choice<TimeMode>(Mode);
        const bool freeze = isOn(Freeze);
        const bool pingPong = isOn(PingPong);
        const bool filterOn = isOn(FilterOn);
        if (filterOn) {  // a high-pass and a low-pass, `width` octaves apart around the frequency
            const float cutoff = param(FilterFreq);
            const float halfWidth = 0.5f * param(FilterWidth);
            const float nyquist = static_cast<float>(0.49 * sampleRate_);
            const float low = std::min(cutoff * std::exp2(-halfWidth), nyquist);
            const float high = std::min(cutoff * std::exp2(halfWidth), nyquist);
            highPass_ = dsp::SvfCoefficients(std::tan(kPi * low / static_cast<float>(sampleRate_)), kButterworthK);
            lowPass_ = dsp::SvfCoefficients(std::tan(kPi * high / static_cast<float>(sampleRate_)), kButterworthK);
        }
        mix_.setTarget(param(Mix) / 100.f);
        feedback_.setTarget(param(Feedback) / 100.f);

        float target[2];
        for (int s = 0; s < 2; ++s) {
            target[s] = static_cast<float>(std::clamp(timeSeconds(s, ctx.tempo), 0.001, kMaxDelaySeconds) *
                                           sampleRate_);
        }

        for (int i = 0; i < numFrames; ++i) {
            const float feedback = freeze ? 1.f : feedback_.next();
            const float mix = mix_.next();
            float in[2] = {ch[0][i], n > 1 ? ch[1][i] : ch[0][i]};
            publish(Input, 0.5f * (in[0] + in[1]));
            float wet[2], echo[2];
            for (int s = 0; s < 2; ++s) {
                echo[s] = readSide(s, target[s], mode);
                float y = echo[s];
                if (filterOn) {
                    const dsp::Svf::Outputs high = filters_[2 * s].tick(highPass_, y);
                    y = y - kButterworthK * high.band - high.low;
                    y = filters_[2 * s + 1].tick(lowPass_, y).low;
                }
                wet[s] = y;
            }

            float write[2];
            if (freeze) {
                write[0] = echo[0];
                write[1] = echo[1];
            } else if (pingPong) {
                write[0] = 0.5f * (in[0] + in[1]) + feedback * wet[1];
                write[1] = feedback * wet[0];
            } else {
                write[0] = in[0] + feedback * wet[0];
                write[1] = in[1] + feedback * wet[1];
            }
            for (int s = 0; s < 2; ++s) lines_[s][writePos_] = write[s];
            writePos_ = (writePos_ + 1) & mask_;

            if (n == 1) {
                ch[0][i] = in[0] + mix * (0.5f * (wet[0] + wet[1]) - in[0]);
            } else {
                for (int c = 0; c < 2; ++c) ch[c][i] = in[c] + mix * (wet[c] - in[c]);
            }
        }
    }

private:
    static constexpr float kButterworthK = 1.41421356f;  // 1/Q, Q = 1/sqrt(2)

    struct Side {
        float current = -1.f;  // the delay in samples (gliding to the target in Repitch); < 0: not yet set
        float fadeFrom = 0.f;  // Fade: the time faded away from
        int fadeLeft = 0;      // Fade: samples of the crossfade still to go
    };

    // Side s's time in seconds (the right one's is the left's while linked).
    double timeSeconds(int s, double tempo) const noexcept {
        if (s == 1 && isOn(Link)) s = 0;
        const int base = s == 0 ? LeftSync : RightSync;
        if (!isOn(base)) return param(base + 2) * 0.001;
        const auto index = std::clamp(choiceIndex(base + 1), 0, 7);
        const double sixteenth = 15.0 / std::max(1.0, tempo);
        return kDivisions[index] * sixteenth * (1.0 + param(base + 3) / 100.0);
    }

    float readSide(int s, float target, TimeMode mode) noexcept {
        Side& side = sides_[s];
        if (side.current < 0.f) side.current = target;
        switch (mode) {
            case Repitch:
                side.current = target + glide_ * (side.current - target);
                side.fadeLeft = 0;
                return read(s, side.current);
            case Fade:
                if (side.fadeLeft == 0 && std::abs(target - side.current) >= 1.f) {
                    side.fadeFrom = side.current;
                    side.current = target;
                    side.fadeLeft = fadeLength_;
                }
                if (side.fadeLeft > 0) {
                    const float t = static_cast<float>(side.fadeLeft--) / static_cast<float>(fadeLength_);
                    const float from = std::sin(0.5f * kPi * t), to = std::cos(0.5f * kPi * t);  // equal power
                    return from * read(s, side.fadeFrom) + to * read(s, side.current);
                }
                return read(s, side.current);
            case Jump:
            default:
                side.current = target;
                side.fadeLeft = 0;
                return read(s, side.current);
        }
    }

    // Line s, `delay` samples ago (fractional: 4-point, 3rd-order Hermite).
    float read(int s, float delay) const noexcept {
        delay = std::clamp(delay, 1.f, static_cast<float>(mask_ - 4));
        const double position = static_cast<double>(writePos_) - delay;
        const double floor = std::floor(position);
        const float frac = static_cast<float>(position - floor);
        const auto i = static_cast<size_t>(static_cast<int64_t>(floor) + static_cast<int64_t>(mask_ + 1));
        const std::vector<float>& line = lines_[s];
        return dsp::hermite(line[(i - 1) & mask_], line[i & mask_], line[(i + 1) & mask_], line[(i + 2) & mask_], frac);
    }

    static const std::vector<ParamInfo>& infos() {
        const std::vector<std::string>& kOnOff = offOnLabels();
        static const std::vector<std::string> kSixteenths = {"1", "2", "3", "4", "5", "6", "8", "16"};
        static const std::vector<ParamInfo> kInfos = {
            {"l_sync", "L Sync", "", 0.f, 1.f, 1.f, false, kOnOff},
            {"l_division", "L 16th", "", 0.f, 7.f, 2.f, false, kSixteenths},
            {"l_time", "L Time", "ms", 1.f, 5000.f, 250.f, true},
            {"l_offset", "L Offset", "%", -33.f, 33.f, 0.f},
            {"r_sync", "R Sync", "", 0.f, 1.f, 1.f, false, kOnOff},
            {"r_division", "R 16th", "", 0.f, 7.f, 3.f, false, kSixteenths},
            {"r_time", "R Time", "ms", 1.f, 5000.f, 375.f, true},
            {"r_offset", "R Offset", "%", -33.f, 33.f, 0.f},
            {"link", "Link", "", 0.f, 1.f, 0.f, false, kOnOff},
            {"feedback", "Feedback", "%", 0.f, 95.f, 50.f},
            {"freeze", "Freeze", "", 0.f, 1.f, 0.f, false, kOnOff},
            {"filter", "Filter", "", 0.f, 1.f, 1.f, false, kOnOff},
            {"freq", "Filter Freq", "Hz", 50.f, 18000.f, 1000.f, true},
            {"width", "Filter Width", "", 0.5f, 9.f, 8.f},
            {"mode", "Mode", "", 0.f, 2.f, 0.f, false, {"Repitch", "Fade", "Jump"}},
            {"ping_pong", "Ping Pong", "", 0.f, 1.f, 0.f, false, kOnOff},
            {"mix", "Dry/Wet", "%", 0.f, 100.f, 50.f},
        };
        return kInfos;
    }

    double sampleRate_ = 48000.0;
    std::vector<float> lines_[2];
    size_t mask_ = 0;
    size_t writePos_ = 0;
    Side sides_[2];
    dsp::Svf filters_[4];  // per side: high-pass, low-pass
    dsp::SvfCoefficients highPass_, lowPass_;
    float glide_ = 0.f;
    int fadeLength_ = 1;
    SmoothedValue mix_, feedback_;
};

}  // namespace

SUB_REGISTER_BUILTIN(DelayProcessor, AudioEffect);

}  // namespace sub
