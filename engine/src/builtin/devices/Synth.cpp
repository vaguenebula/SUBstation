// Built-in "Synth" instrument: a simple polyphonic subtractive synthesizer.
// One oscillator per voice (sine, triangle, band-limited saw or square), an
// ADSR amplitude envelope and a resonant low-pass filter. Mono, on both channels.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

#include "builtin/BuiltinProcessor.h"
#include "builtin/BuiltinRegistry.h"
#include "rt/RtUtils.h"

namespace sub {
namespace {

class SynthProcessor final : public BuiltinProcessor {
public:
    enum Param { Wave = 0, Attack, Decay, Sustain, Release, Cutoff, Resonance, Volume, NumParams };
    enum class Waveform : uint8_t { Sine, Triangle, Saw, Square };
    static constexpr int kMaxVoices = 16;

    SynthProcessor();

    std::string typeId() const override { return "builtin:synth"; }
    std::string name() const override { return "Synth"; }

    void prepare(double sampleRate, int maxBlockSize) override;
    void reset() override;
    int tailSamples() const override;

protected:
    // Writes (does not add) the synth's output: it is the first device on a MIDI track.
    void render(const ProcessContext& ctx, float* const* channels, int numChannels, int numFrames) override;

private:
    enum class Stage : uint8_t { Attack, Decay, Release };  // Decay ends at, and holds, the sustain level

    struct Voice {
        bool active = false;
        bool held = true;  // no note-off yet
        uint8_t key = 0;
        uint64_t started = 0;  // note counter, for stealing the oldest voice
        float gain = 0.f;      // velocity
        double phase = 0.0;    // 0..1
        double increment = 0.0;
        Stage stage = Stage::Attack;
        float level = 0.f;     // envelope
        float ic1 = 0.f, ic2 = 0.f;  // filter state
    };

    // Per-block settings derived from the parameters.
    struct Envelope {
        float attackStep;
        float decayCoef;
        float sustain;
        float releaseCoef;
    };

    void noteOn(uint8_t key, uint8_t velocity);
    void noteOff(uint8_t key);
    void renderVoices(int from, int to, Waveform wave, const Envelope& env);
    Envelope envelope() const;

    std::array<Voice, kMaxVoices> voices_{};
    uint64_t noteCounter_ = 0;
    double sampleRate_ = 48000.0;

    // Filter coefficients per sample of the block (the cutoff glides), and the mix.
    std::vector<float> a1_, a2_, a3_, mix_;
    float cutoff_ = 0.f;  // smoothed, Hz
    float cutoffGlide_ = 0.f;
    SmoothedValue volume_;
};


constexpr double kPi = 3.14159265358979323846;
constexpr float kVoiceGain = 0.25f;  // one voice's peak at full velocity
constexpr float kSilent = 1e-4f;     // -80 dB: a releasing voice ends here
constexpr double kFadeTo = 1e-3;     // decay and release times are to -60 dB

// PolyBLEP: smooths an oscillator's step at phase 0 over one sample on each
// side, which removes most of the aliasing of a naive saw or square.
inline double polyBlep(double t, double dt) noexcept {
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0;
    }
    if (t > 1.0 - dt) {
        t = (t - 1.0) / dt;
        return t * t + t + t + 1.0;
    }
    return 0.0;
}

const std::vector<ParamInfo>& infos() {
    static const std::vector<ParamInfo> kInfos = [] {
        std::vector<ParamInfo> list = {
            {"wave", "Wave", "", 0.f, 3.f, 2.f},
            {"attack", "Attack", "ms", 1.f, 5000.f, 3.f, true},
            {"decay", "Decay", "ms", 1.f, 5000.f, 300.f, true},
            {"sustain", "Sustain", "%", 0.f, 100.f, 70.f},
            {"release", "Release", "ms", 1.f, 10000.f, 200.f, true},
            {"cutoff", "Cutoff", "Hz", 20.f, 20000.f, 4000.f, true},
            {"resonance", "Resonance", "%", 0.f, 100.f, 10.f},
            {"volume", "Volume", "dB", -60.f, 6.f, 0.f},
        };
        list[SynthProcessor::Wave].valueLabels = {"Sine", "Triangle", "Saw", "Square"};
        return list;
    }();
    return kInfos;
}

}  // namespace

SynthProcessor::SynthProcessor() : BuiltinProcessor(infos()) {}

void SynthProcessor::prepare(double sampleRate, int maxBlockSize) {
    sampleRate_ = sampleRate;
    for (auto* buffer : {&a1_, &a2_, &a3_, &mix_}) buffer->assign(static_cast<size_t>(maxBlockSize), 0.f);
    cutoffGlide_ = static_cast<float>(1.0 - std::exp(-1.0 / (0.005 * sampleRate)));  // ~5 ms
    volume_.reset(sampleRate, 0.02);
    reset();
}

void SynthProcessor::reset() {
    for (Voice& voice : voices_) voice.active = false;
    cutoff_ = param(Cutoff);
    volume_.snapTo(dbToGain(param(Volume)));
}

int SynthProcessor::tailSamples() const { return static_cast<int>(param(Release) * 0.001 * sampleRate_); }

SynthProcessor::Envelope SynthProcessor::envelope() const {
    const double samplesPerMs = sampleRate_ * 0.001;
    const auto fade = [&](float ms) {
        return static_cast<float>(std::exp(std::log(kFadeTo) / std::max(1.0, ms * samplesPerMs)));
    };
    return {
        static_cast<float>(1.0 / std::max(1.0, param(Attack) * samplesPerMs)),
        fade(param(Decay)),
        std::clamp(param(Sustain) / 100.f, 0.f, 1.f),
        fade(param(Release)),
    };
}

void SynthProcessor::noteOn(uint8_t key, uint8_t velocity) {
    // A free voice; else the quietest releasing one; else the oldest.
    Voice* target = nullptr;
    for (Voice& voice : voices_) {
        if (!voice.active) {
            target = &voice;
            break;
        }
    }
    if (!target) {
        for (Voice& voice : voices_) {
            if (voice.stage == Stage::Release && (!target || voice.level < target->level)) target = &voice;
        }
    }
    if (!target) {
        for (Voice& voice : voices_) {
            if (!target || voice.started < target->started) target = &voice;
        }
    }
    Voice& voice = *target;
    voice = Voice{};
    voice.active = true;
    voice.key = key;
    voice.started = ++noteCounter_;
    voice.gain = kVoiceGain * static_cast<float>(velocity) / 127.f;
    voice.increment = 440.0 * std::pow(2.0, (key - 69) / 12.0) / sampleRate_;
}

void SynthProcessor::noteOff(uint8_t key) {
    // With the same key held twice, the older note ends first.
    Voice* target = nullptr;
    for (Voice& voice : voices_) {
        if (voice.active && voice.held && voice.key == key && (!target || voice.started < target->started)) {
            target = &voice;
        }
    }
    if (target) {
        target->held = false;
        target->stage = Stage::Release;
    }
}

void SynthProcessor::renderVoices(int from, int to, Waveform wave, const Envelope& env) {
    for (Voice& voice : voices_) {
        if (!voice.active) continue;
        const double dt = voice.increment;
        for (int i = from; i < to; ++i) {
            switch (voice.stage) {
                case Stage::Attack:
                    voice.level += env.attackStep;
                    if (voice.level >= 1.f) {
                        voice.level = 1.f;
                        voice.stage = Stage::Decay;
                    }
                    break;
                case Stage::Decay:  // then holds the sustain level (and follows it if it changes)
                    voice.level = env.sustain + (voice.level - env.sustain) * env.decayCoef;
                    break;
                case Stage::Release:
                    voice.level *= env.releaseCoef;
                    break;
            }

            const double t = voice.phase;
            double sample = 0.0;
            switch (wave) {
                case Waveform::Sine: sample = std::sin(2.0 * kPi * t); break;
                case Waveform::Triangle: sample = 1.0 - 4.0 * std::abs(t - 0.5); break;
                case Waveform::Saw: sample = 2.0 * t - 1.0 - polyBlep(t, dt); break;
                case Waveform::Square: {
                    const double half = t < 0.5 ? t + 0.5 : t - 0.5;
                    sample = (t < 0.5 ? 1.0 : -1.0) + polyBlep(t, dt) - polyBlep(half, dt);
                    break;
                }
            }
            voice.phase += dt;
            if (voice.phase >= 1.0) voice.phase -= 1.0;

            // Low-pass: a TPT state-variable filter (Zavalishin/Simper), stable
            // at any cutoff and resonance.
            const float v3 = static_cast<float>(sample) - voice.ic2;
            const float v1 = a1_[i] * voice.ic1 + a2_[i] * v3;
            const float v2 = voice.ic2 + a2_[i] * voice.ic1 + a3_[i] * v3;
            voice.ic1 = 2.f * v1 - voice.ic1;
            voice.ic2 = 2.f * v2 - voice.ic2;
            mix_[i] += v2 * voice.level * voice.gain;
        }
        const bool faded = voice.stage == Stage::Release || (voice.stage == Stage::Decay && env.sustain < kSilent);
        if (faded && voice.level < kSilent) voice.active = false;
    }
}

void SynthProcessor::render(const ProcessContext& ctx, float* const* channels, int numChannels, int numFrames) {
    const bool silent = ctx.inEvents.count == 0 &&
                        std::none_of(voices_.begin(), voices_.end(), [](const Voice& voice) { return voice.active; });
    if (silent) {  // nothing playing: skip the work, and start the next note at the current settings
        cutoff_ = param(Cutoff);
        volume_.snapTo(dbToGain(param(Volume)));
        for (int c = 0; c < std::min(numChannels, 2); ++c) std::fill_n(channels[c], numFrames, 0.f);
        return;
    }

    const int frames = std::min(numFrames, static_cast<int>(mix_.size()));  // prepare() said no more
    const auto wave = static_cast<Waveform>(std::clamp(static_cast<int>(std::lround(param(Wave))), 0, 3));
    const Envelope env = envelope();

    // Filter coefficients for each sample: the cutoff glides to its target so
    // turning the knob doesn't step; the resonance applies per block.
    const float target = param(Cutoff);
    const auto highest = static_cast<float>(sampleRate_ * 0.45);
    const float damping = 2.f - 1.9f * param(Resonance) / 100.f;  // 2 (none) .. 0.1 (Q = 10)
    for (int i = 0; i < frames; ++i) {
        cutoff_ += (target - cutoff_) * cutoffGlide_;
        const auto g = static_cast<float>(std::tan(kPi * std::min(cutoff_, highest) / sampleRate_));
        a1_[i] = 1.f / (1.f + g * (g + damping));
        a2_[i] = g * a1_[i];
        a3_[i] = g * a2_[i];
    }

    std::fill_n(mix_.data(), frames, 0.f);
    int position = 0;
    for (size_t e = 0; e < ctx.inEvents.count; ++e) {
        const ProcessEvent& event = ctx.inEvents.events[e];
        const int at = std::clamp(static_cast<int>(event.sampleOffset), position, frames);
        renderVoices(position, at, wave, env);
        position = at;
        if (event.type == ProcessEvent::Type::NoteOn && event.velocity() > 0) {
            noteOn(event.key(), event.velocity());
        } else if (event.type == ProcessEvent::Type::NoteOn || event.type == ProcessEvent::Type::NoteOff) {
            noteOff(event.key());
        }
    }
    renderVoices(position, frames, wave, env);

    volume_.setTarget(dbToGain(param(Volume)));
    float* left = channels[0];
    float* right = numChannels > 1 ? channels[1] : nullptr;
    for (int i = 0; i < numFrames; ++i) {
        const float out = i < frames ? mix_[i] * volume_.next() : 0.f;
        left[i] = out;
        if (right) right[i] = out;
    }
}

SUB_REGISTER_BUILTIN(SynthProcessor, Instrument);

}  // namespace sub
