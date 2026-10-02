// Built-in "Sampler" instrument: plays one audio file across the keyboard,
// pitched from its root key (as Ableton's Simpler does in Classic mode).
// Polyphonic, with an ADSR amplitude envelope; plays from Start to End of the
// sample, or loops between them while a note holds (and as it releases).
//
// The sample is the device's state besides its parameters ("sample": its
// path). setState() loads it off the audio thread (through the engine's file
// cache), then hands it to the rendering thread without a lock: it goes into
// `pending_`, the rendering thread takes it at the start of a block and hands
// back the one it replaces through `retired_`, which the main side frees.
//
// Its editor draws one display: where the newest note plays in the sample (0..1
// of its length; -1 while none plays), one value per kMeterSamples.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "builtin/BuiltinProcessor.h"
#include "builtin/BuiltinRegistry.h"
#include "rt/RtUtils.h"

namespace gil {
namespace {

constexpr int kMeterSamples = 256;
constexpr float kSilent = 1e-4f;   // -80 dB: a releasing voice ends here
constexpr double kFadeTo = 1e-3;   // decay and release times are to -60 dB

// 4-point, 3rd-order Hermite interpolation of `data` (`count` frames) at `position`.
inline float hermite(const float* data, int64_t count, double position) noexcept {
    const auto i = static_cast<int64_t>(position);
    const auto t = static_cast<float>(position - static_cast<double>(i));
    const auto at = [&](int64_t k) { return data[std::clamp<int64_t>(k, 0, count - 1)]; };
    const float xm1 = at(i - 1), x0 = at(i), x1 = at(i + 1), x2 = at(i + 2);
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

class SamplerProcessor final : public BuiltinProcessor {
public:
    enum Param {
        Root = 0, Tune, Fine, Start, End, Loop,                 // the sample
        Attack, Decay, Sustain, Release, Velocity, Volume,      // the amplitude
        NumParams
    };
    static constexpr int kMaxVoices = 32;

    SamplerProcessor();
    ~SamplerProcessor() override;

    std::string typeId() const override { return "builtin:sampler"; }
    std::string name() const override { return "Sampler"; }

    void prepare(double sampleRate, int maxBlockSize) override;
    void reset() override;
    int tailSamples() const override { return static_cast<int>(param(Release) * 0.001 * sampleRate_); }
    bool idle() override;

protected:
    StateValues stateValues() const override;
    void setStateValues(const StateValues& values) override;
    // Writes (does not add) the sampler's output: it is the first device on a MIDI track.
    void render(const ProcessContext& ctx, float* const* channels, int numChannels, int numFrames) override;

private:
    // What the rendering thread plays; null `source`: nothing.
    struct Sample {
        std::shared_ptr<const AudioSource> source;
    };
    enum class Stage : uint8_t { Attack, Decay, Release };  // Decay ends at, and holds, the sustain level

    struct Voice {
        bool active = false;
        bool held = true;  // no note-off yet
        uint8_t key = 0;
        uint64_t started = 0;  // note counter, for stealing the oldest voice
        float gain = 0.f;      // velocity
        double position = 0.0; // in the sample's frames
        Stage stage = Stage::Attack;
        float level = 0.f;     // envelope
    };

    void publishSample(std::shared_ptr<const AudioSource> source);  // main side, under mutex_
    void collectRetired();                                           // main side, under mutex_
    void takeSample() noexcept;                                      // rendering thread
    void noteOn(uint8_t key, uint8_t velocity, int64_t start);
    void noteOff(uint8_t key);
    // Renders frames [from, to) of the block (just counts them with `left`
    // null), publishing the playhead at every kMeterSamples.
    void advance(int from, int to, float* left, float* right);
    void renderVoices(int from, int to, float* left, float* right);
    float playhead() const noexcept;

    // Main side: the sample's path (as the state has it, even if it couldn't be
    // loaded) and what was last handed over.
    mutable std::mutex mutex_;
    std::string path_;
    std::shared_ptr<const AudioSource> published_;
    std::atomic<Sample*> pending_{nullptr};
    SpscQueue<Sample*, 8> retired_;

    // Rendering thread.
    Sample* active_ = nullptr;
    std::array<Voice, kMaxVoices> voices_{};
    uint64_t noteCounter_ = 0;
    double sampleRate_ = 48000.0;
    SmoothedValue volume_;
    int meterCount_ = 0;
    // This block's settings (render() sets them for renderVoices()).
    int64_t startFrame_ = 0, endFrame_ = 0;
    bool loop_ = false;
    float attackStep_ = 1.f, decayCoef_ = 0.f, sustain_ = 1.f, releaseCoef_ = 0.f;
    double rateRatio_ = 1.0;  // the sample's frames per output frame at its root key
    double transpose_ = 0.0;  // semitones, from Tune and Fine
    int root_ = 60;
};

const std::vector<ParamInfo>& infos() {
    static const std::vector<ParamInfo> kInfos = [] {
        std::vector<ParamInfo> list = {
            {"root", "Root Key", "note", 0.f, 127.f, 60.f},
            {"tune", "Transpose", "st", -48.f, 48.f, 0.f},
            {"fine", "Detune", "ct", -100.f, 100.f, 0.f},
            {"start", "Start", "%", 0.f, 100.f, 0.f},
            {"end", "End", "%", 0.f, 100.f, 100.f},
            {"loop", "Loop", "", 0.f, 1.f, 0.f},
            {"attack", "Attack", "ms", 0.1f, 5000.f, 1.f, true},
            {"decay", "Decay", "ms", 1.f, 10000.f, 1000.f, true},
            {"sustain", "Sustain", "%", 0.f, 100.f, 100.f},
            {"release", "Release", "ms", 1.f, 10000.f, 50.f, true},
            {"velocity", "Velocity", "%", 0.f, 100.f, 50.f},
            {"volume", "Volume", "dB", -60.f, 6.f, 0.f},
        };
        list[SamplerProcessor::Root].steps = 127;
        list[SamplerProcessor::Tune].steps = 96;
        list[SamplerProcessor::Loop].valueLabels = {"Off", "On"};
        return list;
    }();
    return kInfos;
}

}  // namespace

SamplerProcessor::SamplerProcessor() : BuiltinProcessor(infos(), {{"position", kMeterSamples}}) {}

SamplerProcessor::~SamplerProcessor() {
    // Nothing renders it any more.
    delete active_;
    delete pending_.load();
    Sample* old = nullptr;
    while (retired_.pop(old)) delete old;
}

void SamplerProcessor::prepare(double sampleRate, int /*maxBlockSize*/) {
    sampleRate_ = sampleRate;
    volume_.reset(sampleRate, 0.02);
    reset();
}

void SamplerProcessor::reset() {
    for (Voice& voice : voices_) voice.active = false;
    volume_.snapTo(dbToGain(param(Volume)));
}

bool SamplerProcessor::idle() {
    std::lock_guard lock(mutex_);
    collectRetired();
    return false;
}

// --- The sample -------------------------------------------------------------

BuiltinProcessor::StateValues SamplerProcessor::stateValues() const {
    std::lock_guard lock(mutex_);
    if (path_.empty()) return {};
    return {{"sample", path_}};
}

void SamplerProcessor::setStateValues(const StateValues& values) {
    const auto it = values.find("sample");
    const std::string path = it == values.end() ? std::string() : it->second;
    {
        std::lock_guard lock(mutex_);
        path_ = path;
    }
    std::shared_ptr<const AudioSource> source;
    if (!path.empty()) {
        try {
            source = loadSource(path);  // may take a while: not under the lock
        } catch (const std::exception&) {
            std::lock_guard lock(mutex_);
            if (path_ == path) publishSample(nullptr);  // silent until it can be loaded
            throw;
        }
    }
    std::lock_guard lock(mutex_);
    if (path_ == path) publishSample(std::move(source));  // (unless another state came meanwhile)
}

void SamplerProcessor::publishSample(std::shared_ptr<const AudioSource> source) {
    collectRetired();
    if (source == published_) return;  // the same file: notes go on
    published_ = source;
    // The rendering thread never took a sample still pending: free it here.
    delete pending_.exchange(new Sample{std::move(source)}, std::memory_order_acq_rel);
}

void SamplerProcessor::collectRetired() {
    Sample* old = nullptr;
    while (retired_.pop(old)) delete old;
}

void SamplerProcessor::takeSample() noexcept {
    if (!pending_.load(std::memory_order_acquire)) return;
    if (active_ && !retired_.push(active_)) return;  // the main side hasn't freed the last ones: next block
    // Only this thread empties pending_, so it holds a sample still (perhaps a newer one).
    active_ = pending_.exchange(nullptr, std::memory_order_acq_rel);
    for (Voice& voice : voices_) voice.active = false;  // they played the old one
}

// --- Notes ------------------------------------------------------------------

void SamplerProcessor::noteOn(uint8_t key, uint8_t velocity, int64_t start) {
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
    const float sensitivity = std::clamp(param(Velocity) / 100.f, 0.f, 1.f);
    Voice& voice = *target;
    voice = Voice{};
    voice.active = true;
    voice.key = key;
    voice.started = ++noteCounter_;
    voice.gain = 1.f - sensitivity * (1.f - static_cast<float>(velocity) / 127.f);
    voice.position = static_cast<double>(start);
}

void SamplerProcessor::noteOff(uint8_t key) {
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

void SamplerProcessor::renderVoices(int from, int to, float* left, float* right) {
    const AudioSource& source = *active_->source;
    const int64_t frames = source.frames();
    const float* dataLeft = source.channelData(0);
    const float* dataRight = source.channels() > 1 ? source.channelData(1) : dataLeft;
    const auto loopLength = static_cast<double>(endFrame_ - startFrame_);
    for (Voice& voice : voices_) {
        if (!voice.active) continue;
        const double increment = rateRatio_ * std::exp2((voice.key - root_ + transpose_) / 12.0);
        for (int i = from; i < to; ++i) {
            if (voice.position >= static_cast<double>(endFrame_)) {
                if (!loop_ || loopLength < 1.0) {
                    voice.active = false;
                    break;
                }
                voice.position = startFrame_ + std::fmod(voice.position - startFrame_, loopLength);
            }
            switch (voice.stage) {
                case Stage::Attack:
                    voice.level += attackStep_;
                    if (voice.level >= 1.f) {
                        voice.level = 1.f;
                        voice.stage = Stage::Decay;
                    }
                    break;
                case Stage::Decay:  // then holds the sustain level (and follows it if it changes)
                    voice.level = sustain_ + (voice.level - sustain_) * decayCoef_;
                    break;
                case Stage::Release:
                    voice.level *= releaseCoef_;
                    break;
            }
            const float gain = voice.level * voice.gain;
            left[i] += hermite(dataLeft, frames, voice.position) * gain;
            right[i] += hermite(dataRight, frames, voice.position) * gain;
            voice.position += increment;
        }
        const bool faded = voice.stage == Stage::Release || (voice.stage == Stage::Decay && sustain_ < kSilent);
        if (faded && voice.level < kSilent) voice.active = false;
    }
}

void SamplerProcessor::render(const ProcessContext& ctx, float* const* channels, int numChannels, int numFrames) {
    if (numChannels <= 0) return;
    takeSample();
    float* left = channels[0];
    float* right = numChannels > 1 ? channels[1] : nullptr;
    std::fill_n(left, numFrames, 0.f);
    if (right) std::fill_n(right, numFrames, 0.f);

    const AudioSource* source = active_ ? active_->source.get() : nullptr;
    const int64_t frames = source ? source->frames() : 0;
    const bool silent = ctx.inEvents.count == 0 &&
                        std::none_of(voices_.begin(), voices_.end(), [](const Voice& voice) { return voice.active; });
    if (frames <= 0 || silent) {  // nothing to play: skip the work, and start the next note at the current volume
        volume_.snapTo(dbToGain(param(Volume)));
        advance(0, numFrames, nullptr, nullptr);
    } else {
        startFrame_ = std::clamp(static_cast<int64_t>(param(Start) / 100.0 * frames), int64_t{0}, frames - 1);
        endFrame_ = std::clamp(static_cast<int64_t>(param(End) / 100.0 * frames), int64_t{0}, frames);
        loop_ = param(Loop) >= 0.5f;
        const double samplesPerMs = sampleRate_ * 0.001;
        const auto fade = [&](float ms) {
            return static_cast<float>(std::exp(std::log(kFadeTo) / std::max(1.0, ms * samplesPerMs)));
        };
        attackStep_ = static_cast<float>(1.0 / std::max(1.0, param(Attack) * samplesPerMs));
        decayCoef_ = fade(param(Decay));
        sustain_ = std::clamp(param(Sustain) / 100.f, 0.f, 1.f);
        releaseCoef_ = fade(param(Release));
        rateRatio_ = static_cast<double>(source->sampleRate()) / sampleRate_;
        root_ = static_cast<int>(std::lround(param(Root)));
        transpose_ = std::round(param(Tune)) + param(Fine) / 100.0;

        // Voices mix into the output (both sides into one channel, if that's all
        // there is); the volume applies after.
        float* mixRight = right ? right : left;
        int position = 0;
        for (size_t e = 0; e < ctx.inEvents.count; ++e) {
            const ProcessEvent& event = ctx.inEvents.events[e];
            const int at = std::clamp(static_cast<int>(event.sampleOffset), position, numFrames);
            advance(position, at, left, mixRight);
            position = at;
            if (event.type == ProcessEvent::Type::NoteOn && event.velocity() > 0) {
                if (endFrame_ > startFrame_) noteOn(event.key(), event.velocity(), startFrame_);
            } else if (event.type == ProcessEvent::Type::NoteOn || event.type == ProcessEvent::Type::NoteOff) {
                noteOff(event.key());
            }
        }
        advance(position, numFrames, left, mixRight);

        volume_.setTarget(dbToGain(param(Volume)));
        const float sides = right ? 1.f : 0.5f;
        for (int i = 0; i < numFrames; ++i) {
            const float gain = volume_.next() * sides;
            left[i] *= gain;
            if (right) right[i] *= gain;
        }
    }
}

void SamplerProcessor::advance(int from, int to, float* left, float* right) {
    while (from < to) {
        const int until = std::min(to, from + (kMeterSamples - meterCount_));
        if (left) renderVoices(from, until, left, right);
        meterCount_ += until - from;
        from = until;
        if (meterCount_ >= kMeterSamples) {
            meterCount_ = 0;
            publish(0, playhead());
        }
    }
}

float SamplerProcessor::playhead() const noexcept {
    const int64_t frames = active_ && active_->source ? active_->source->frames() : 0;
    const Voice* newest = nullptr;
    for (const Voice& voice : voices_) {
        if (voice.active && (!newest || voice.started > newest->started)) newest = &voice;
    }
    if (!newest || frames <= 0) return -1.f;
    return static_cast<float>(std::min(1.0, newest->position / static_cast<double>(frames)));
}

GIL_REGISTER_BUILTIN(SamplerProcessor, Instrument);

}  // namespace gil
