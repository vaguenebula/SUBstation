// Built-in "Sampler" instrument: plays one audio file across the keyboard, as
// Ableton's Simpler does, in one of three modes:
//
//  - Classic: polyphonic (up to Voices notes), pitched from its Root Key, with an
//    ADSR envelope; plays from Start to End, or loops (Loop Start to End, its end
//    crossfaded into what comes before Loop Start over Loop Fade) while a note
//    holds and as it releases. With one voice and Glide, notes play legato, the
//    pitch gliding from one to the next.
//  - 1-Shot: one note at a time, pitched; plays from Start to End whatever the
//    note's length (Trigger) or until the note ends (Gate), fading in, and out
//    before End and after a Gate's note-off.
//  - Slice: the sample cut at its transients, at beats or into equal regions
//    (builtin/SampleSlicing.h), a slice per key from C1 up, at the root's pitch;
//    Mono (a slice cuts the one before), Poly, or Thru (a slice plays on to End,
//    mono). Fades, Trigger and Gate as 1-Shot's.
//
// Around them: Gain; Reverse (the sample plays backwards: Start, End, the loop
// and the slices are places in it, played that way); Snap (Start, End, Loop
// Start and slices move to the nearest zero crossing); Warp (the whole sample
// lasts Warp Length beats at the song's tempo: resampled in Re-Pitch, so the
// pitch follows; else stretched by Signalsmith Stretch, a stretcher per note,
// the keys transposing it); a filter on what the voices play (low-, high-,
// band-pass or notch, 12 or 24 dB/octave, TPT state-variable sections); an LFO
// (six shapes, in Hz or synced to the song, restarting with each note or not)
// on the volume, the pitch, the filter's cutoff and the pan; Pan, Vol < Vel and
// Volume. A voice cut short (stolen, or a mono note replaced) fades out over a
// few milliseconds instead of clicking.
//
// The sample is the device's state besides its parameters ("sample": its
// path). setState() loads it off the audio thread (through the engine's file
// cache) and finds its transients, then hands it to the rendering thread
// without a lock: it goes into `pending_`, the rendering thread takes it at the
// start of a block and hands back the one it replaces through `retired_`, which
// the main side frees. Stretchers are made on the main side while Warp
// stretches (idle(), prepare(), resetOffline(): never on the audio thread) and
// handed over the same way (`pendingPool_`, `retiredPools_`); until they come,
// warped notes are resampled.
//
// Its editor draws one display: where the newest note plays in the sample (0..1
// of its length, in the order it plays: from its end reversed; -1 while none
// plays), one value per kMeterSamples.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "Warp.h"
#include "builtin/BuiltinProcessor.h"
#include "builtin/BuiltinRegistry.h"
#include "builtin/SampleSlicing.h"
#include "rt/RtUtils.h"

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "signalsmith-stretch.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace sub {
namespace {

using Stretcher = signalsmith::stretch::SignalsmithStretch<float>;

constexpr int kMeterSamples = 256;
constexpr int kChunk = 32;                     // the LFO, glides and the cutoff move every this many samples
constexpr int kMaxVoices = 32;                 // notes at once (the most Voices allows)
constexpr int kVoiceSlots = kMaxVoices + 8;    // and voices cut short, fading out
constexpr int kMaxStretched = 8;               // stretched (warped) notes at once
constexpr double kMaxStretchRate = 4.0;        // faster than this, warped notes are resampled (a stretcher's work grows with it)
constexpr float kSilent = 1e-4f;               // -80 dB: a releasing voice ends here
constexpr double kFadeTo = 1e-3;               // decay and release times are to -60 dB
constexpr double kKillSeconds = 0.004;         // a voice cut short fades out over this
constexpr double kCutoffGlideSeconds = 0.005;  // the cutoff follows its knob (and the LFO) this fast
constexpr double kLfoFilterOctaves = 4.0;      // how far the LFO moves the cutoff either way at 100 %
constexpr double kMaxResonanceQ = 10.0;
constexpr double kPi = 3.14159265358979323846;

enum class Mode : uint8_t { Classic, OneShot, Slice };
enum class Playback : uint8_t { Mono, Poly, Thru };
enum class FilterType : uint8_t { LowPass, HighPass, BandPass, Notch };
enum class LfoWave : uint8_t { Sine, Triangle, SawUp, SawDown, Square, Random };

// The LFO's synced rates, in beats, as its list names them.
constexpr double kLfoBeats[] = {0.125, 0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0, 32.0};

// 4-point, 3rd-order Hermite interpolation between x0 and x1, `t` of the way.
inline float hermite(float xm1, float x0, float x1, float x2, float t) noexcept {
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

// A number in [-1, 1] from a counter: the Random LFO's value for a cycle.
inline float randomOf(uint64_t n) noexcept {
    n += 0x9e3779b97f4a7c15ull;  // splitmix64
    n = (n ^ (n >> 30)) * 0xbf58476d1ce4e5b9ull;
    n = (n ^ (n >> 27)) * 0x94d049bb133111ebull;
    n ^= n >> 31;
    return static_cast<float>(static_cast<double>(n >> 11) / static_cast<double>(1ull << 53) * 2.0 - 1.0);
}

// A TPT state-variable filter section (Zavalishin/Simper), all its outputs.
struct Svf {
    float ic1 = 0.f, ic2 = 0.f;
    void reset() noexcept { ic1 = ic2 = 0.f; }
};

// A section's coefficients: g = tan(pi f / sr), k = 1/Q.
struct SvfCoefficients {
    float k = 1.f, a1 = 1.f, a2 = 0.f, a3 = 0.f;
    SvfCoefficients() = default;
    SvfCoefficients(float g, float damping) noexcept : k(damping) {
        a1 = 1.f / (1.f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
};

inline float filterSample(Svf& s, const SvfCoefficients& c, FilterType type, float x) noexcept {
    const float v3 = x - s.ic2;
    const float v1 = c.a1 * s.ic1 + c.a2 * v3;
    const float v2 = s.ic2 + c.a2 * s.ic1 + c.a3 * v3;
    s.ic1 = 2.f * v1 - s.ic1;
    s.ic2 = 2.f * v2 - s.ic2;
    switch (type) {
        case FilterType::LowPass: return v2;
        case FilterType::HighPass: return x - c.k * v1 - v2;
        case FilterType::BandPass: return c.k * v1;  // unity at the centre
        case FilterType::Notch: return x - c.k * v1;
    }
    return x;
}

// The sample as the voices read it this block: in the order it plays, the loop
// (Classic's) wrapped and crossfaded, silence outside the file.
struct Reader {
    const float* data[2] = {nullptr, nullptr};  // a mono sample's one channel twice
    int64_t frames = 0;
    bool reverse = false;
    bool loop = false;
    int64_t loopStart = 0, loopEnd = 0, fade = 0;  // [loopStart, loopEnd), its last `fade` frames crossfaded
    int64_t fastEnd = 0;  // below this (from frame 1), frames read straight from the file

    float raw(int c, int64_t i) const noexcept {
        if (i < 0 || i >= frames) return 0.f;
        return data[c][reverse ? frames - 1 - i : i];
    }
    // Frame `i` as it plays: past the loop's end, wrapped into it; at its end, faded into what leads to its start.
    float at(int c, int64_t i) const noexcept {
        if (!loop) return raw(c, i);
        const int64_t length = loopEnd - loopStart;
        if (i >= loopEnd) i = loopStart + (i - loopStart) % length;
        if (fade > 0 && i >= loopEnd - fade) {
            const float t = (static_cast<float>(i - (loopEnd - fade)) + 0.5f) / static_cast<float>(fade);
            return raw(c, i) * std::sqrt(1.f - t) + raw(c, i - length) * std::sqrt(t);  // equal power
        }
        return raw(c, i);
    }
    // Channel `c` at a fractional frame.
    float interpolated(int c, double position) const noexcept {
        const auto i = static_cast<int64_t>(position);
        const auto t = static_cast<float>(position - static_cast<double>(i));
        if (i >= 1 && i + 2 < fastEnd) {
            if (!reverse) {
                const float* d = data[c] + i;
                return hermite(d[-1], d[0], d[1], d[2], t);
            }
            const float* d = data[c] + (frames - 1 - i);
            return hermite(d[1], d[0], d[-1], d[-2], t);
        }
        return hermite(at(c, i - 1), at(c, i), at(c, i + 1), at(c, i + 2), t);
    }
    // A frame past the loop's end, wrapped into it (any other is as it is).
    double wrapped(double position) const noexcept {
        if (!loop || position < static_cast<double>(loopEnd)) return position;
        const auto start = static_cast<double>(loopStart);
        return start + std::fmod(position - start, static_cast<double>(loopEnd - loopStart));
    }
    int64_t wrapped(int64_t frame) const noexcept {
        if (!loop || frame < loopEnd) return frame;
        return loopStart + (frame - loopStart) % (loopEnd - loopStart);
    }
};

// What a stretcher reads: a voice's frames from `from` on (`input[c][i]`: frame
// from + i as it plays), silent from `end` unless it `loops` (Classic's loop).
struct StretchInput {
    const Reader* reader;
    int64_t from;
    int64_t end;
    bool loops;

    struct Channel {
        const StretchInput* input;
        int c;
        float operator[](int i) const noexcept { return input->frame(c, input->from + i); }
    };
    Channel operator[](int c) const noexcept { return {this, c}; }
    float frame(int c, int64_t i) const noexcept {
        if (loops) return reader->at(c, i);
        return i < end ? reader->raw(c, i) : 0.f;
    }
};

class SamplerProcessor final : public BuiltinProcessor {
public:
    enum Param {
        ModeParam = 0,
        Root, Tune, Fine,                                        // pitch
        Start, End, Gain, Reverse, Snap,                         // the sample
        Warp, WarpBeats, WarpModeParam,                          // warping
        Loop, LoopStart, LoopFade,                               // Classic
        Attack, Decay, Sustain, Release, Voices, Glide,          // Classic
        TriggerMode, FadeIn, FadeOut,                            // 1-Shot and Slice
        SliceBy, Sensitivity, SliceBeat, Regions, PlaybackParam, // Slice
        FilterOn, FilterTypeParam, FilterSlope, FilterFreq, FilterRes,
        LfoOn, LfoWaveParam, LfoSync, LfoRate, LfoBeats, LfoRetrig, LfoVolume, LfoPitch, LfoFilter, LfoPan,
        Pan, Velocity, Volume,
        NumParams
    };

    SamplerProcessor();
    ~SamplerProcessor() override;

    std::string typeId() const override { return "builtin:sampler"; }
    std::string name() const override { return "Sampler"; }

    void prepare(double sampleRate, int maxBlockSize) override;
    void reset() override;
    void resetOffline() override;
    int tailSamples() const override;
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
        std::vector<slicing::Onset> onsets[2];  // its transients played forwards, and reversed
    };
    // The stretchers warped notes use; none while Warp doesn't stretch.
    struct StretchPool {
        StretchConfig config = StretchConfig::Standard;
        double sampleRate = 0.0;
        std::vector<std::unique_ptr<Stretcher>> stretchers;
    };
    // Classic: Attack, Decay (ends at, and holds, the sustain level), Release.
    // The others: Attack (Fade In), Hold, Release (Fade Out).
    enum class Stage : uint8_t { Attack, Decay, Hold, Release };

    struct Voice {
        bool active = false;
        bool held = true;      // no note-off yet
        bool classic = true;   // its envelope: ADSR, else fades
        bool killing = false;  // cut short: fading out
        uint8_t key = 0;
        uint64_t started = 0;  // note counter, for stealing the oldest voice
        float gain = 0.f;      // velocity
        double position = 0.0; // the frame it plays, as the sample plays
        int64_t end = -1;      // where it stops (a slice's end); -1: End
        double pitch = 0.0;    // semitones from the root (gliding to pitchTarget)
        double pitchTarget = 0.0;
        double glideStep = 0.0;  // semitones a sample
        Stage stage = Stage::Attack;
        float level = 0.f;     // envelope
        float kill = 1.f;      // the fade of a voice cut short
        // Stretched: its stretcher in the pool (-1: none), the next frame it
        // reads, the fraction of a frame it is behind, its transposition.
        int stretch = -1;
        int64_t input = 0;
        double inputDebt = 0.0;
        float stretchSemitones = 0.f;
    };

    // This block's settings, from the parameters.
    struct Block {
        Mode mode = Mode::Classic;
        int64_t start = 0, end = 0;  // as the sample plays
        double rate = 1.0;           // the sample's frames per output frame at its root
        bool stretch = false;        // warped notes are stretched
        bool formants = false;
        double transpose = 0.0;      // semitones
        int root = 60;
        float attackStep = 1.f, decayCoef = 0.f, sustain = 1.f, releaseCoef = 0.f;  // Classic
        float fadeInStep = 1.f, fadeOutStep = 1.f;  // the others, a sample
        double fadeOutSamples = 1.0;
        bool gate = false;
        Playback playback = Playback::Mono;
        int voices = kMaxVoices;
        double glideSamples = 0.0;
        float killStep = 1.f;
    };

    void publishSample(std::unique_ptr<Sample> sample);  // main side, under mutex_
    void collectRetired();                               // main side, under mutex_
    void updatePool(bool fresh);                         // main side
    void takeSample() noexcept;                          // rendering thread
    void takePool() noexcept;                            // rendering thread

    void readBlock(const ProcessContext& ctx);
    int64_t snap(int64_t frame) const noexcept;
    void noteOn(uint8_t key, uint8_t velocity);
    void noteOff(uint8_t key);
    // The keys held, newest last (for legato), kept even while nothing can play.
    void holdKey(uint8_t key) noexcept;
    void releaseKey(uint8_t key) noexcept;
    Voice* startVoice(uint8_t key, uint8_t velocity, int64_t from, int64_t end, double pitch);
    void makeRoom(int limit);
    void killAll();
    void kill(Voice& voice) noexcept;
    Voice* monoVoice() noexcept;
    void glideTo(Voice& voice, double pitch) noexcept;
    int freeStretcher() noexcept;

    // Renders frames [from, to) of the block (just counts them with `left` null),
    // a chunk at a time, publishing the playhead at every kMeterSamples.
    void advance(const ProcessContext& ctx, int from, int to, float* left, float* right);
    void renderChunk(const ProcessContext& ctx, int from, int to, float* left, float* right);
    void renderVoice(Voice& voice, int from, int to, double semitones, float* left, float* right);
    float envelope(Voice& voice) const noexcept;
    bool finished(const Voice& voice) const noexcept;
    float lfoValue(double phase, uint64_t cycle) const noexcept;
    void filterChunk(int from, int to, float* left, float* right, float lfo);
    bool filterRinging() const noexcept;
    float playhead() const noexcept;

    // Main side: the sample's path (as the state has it, even if it couldn't be
    // loaded) and what was last handed over.
    mutable std::mutex mutex_;
    std::string path_;
    std::shared_ptr<const AudioSource> published_;
    std::atomic<Sample*> pending_{nullptr};
    SpscQueue<Sample*, 8> retired_;
    std::atomic<StretchPool*> pendingPool_{nullptr};
    SpscQueue<StretchPool*, 8> retiredPools_;
    StretchConfig poolConfig_ = StretchConfig::Standard;  // the stretchers last handed over: their kind,
    size_t poolSize_ = 0;                                 // how many,
    double poolRate_ = 0.0;                               // and their rate

    // Rendering thread.
    Sample* active_ = nullptr;
    StretchPool* pool_ = nullptr;
    std::array<Voice, kVoiceSlots> voices_{};
    std::array<uint8_t, 128> heldKeys_{};  // the keys held, in the order they were pressed
    int heldCount_ = 0;
    uint64_t noteCounter_ = 0;
    double sampleRate_ = 48000.0;
    SmoothedValue volume_, gain_, pan_;
    int meterCount_ = 0;
    Block block_;
    Reader reader_;
    // The LFO: where it is in its cycle, and which cycle (the Random shape's value).
    double lfoPhase_ = 0.0;
    uint64_t lfoCycle_ = 0;
    // The filter: a section per stage and channel, its cutoff (gliding), whether it was on.
    std::array<Svf, 4> filters_{};
    double cutoff_ = 22000.0;
    bool filtering_ = false;
};

const std::vector<ParamInfo>& infos() {
    static const std::vector<ParamInfo> kInfos = [] {
        using P = SamplerProcessor;
        const std::vector<std::string> offOn = {"Off", "On"};
        std::vector<ParamInfo> list = {
            {"mode", "Mode", "", 0.f, 2.f, 0.f},
            {"root", "Root Key", "note", 0.f, 127.f, 60.f},
            {"tune", "Transpose", "st", -48.f, 48.f, 0.f},
            {"fine", "Detune", "ct", -100.f, 100.f, 0.f},
            {"start", "Start", "%", 0.f, 100.f, 0.f},
            {"end", "End", "%", 0.f, 100.f, 100.f},
            {"gain", "Gain", "dB", -24.f, 24.f, 0.f},
            {"reverse", "Reverse", "", 0.f, 1.f, 0.f},
            {"snap", "Snap", "", 0.f, 1.f, 0.f},
            {"warp", "Warp", "", 0.f, 1.f, 0.f},
            {"warp_beats", "Warp Length", "beats", 1.f, 256.f, 4.f},
            {"warp_mode", "Warp Mode", "", 0.f, 4.f, 1.f},
            {"loop", "Loop", "", 0.f, 1.f, 0.f},
            {"loop_start", "Loop Start", "%", 0.f, 100.f, 0.f},
            {"loop_fade", "Loop Fade", "%", 0.f, 100.f, 0.f},
            {"attack", "Attack", "ms", 0.1f, 5000.f, 1.f, true},
            {"decay", "Decay", "ms", 1.f, 10000.f, 1000.f, true},
            {"sustain", "Sustain", "%", 0.f, 100.f, 100.f},
            {"release", "Release", "ms", 1.f, 10000.f, 50.f, true},
            {"voices", "Voices", "#", 1.f, 32.f, 32.f},
            {"glide", "Glide", "ms", 0.f, 2000.f, 0.f},
            {"trigger", "Trigger Mode", "", 0.f, 1.f, 0.f},
            {"fade_in", "Fade In", "ms", 0.1f, 2000.f, 0.1f, true},
            {"fade_out", "Fade Out", "ms", 0.1f, 2000.f, 0.1f, true},
            {"slice_by", "Slice By", "", 0.f, 2.f, 0.f},
            {"sensitivity", "Sensitivity", "%", 0.f, 100.f, 50.f},
            {"slice_beat", "Slice Division", "", 0.f, 6.f, 1.f},
            {"regions", "Regions", "#", 2.f, 64.f, 8.f},
            {"playback", "Playback", "", 0.f, 2.f, 0.f},
            {"filter", "Filter", "", 0.f, 1.f, 0.f},
            {"filter_type", "Filter Type", "", 0.f, 3.f, 0.f},
            {"filter_slope", "Filter Slope", "", 0.f, 1.f, 1.f},
            {"filter_freq", "Filter Freq", "Hz", 20.f, 22000.f, 22000.f, true},
            {"filter_res", "Resonance", "%", 0.f, 100.f, 0.f},
            {"lfo", "LFO", "", 0.f, 1.f, 0.f},
            {"lfo_wave", "LFO Wave", "", 0.f, 5.f, 0.f},
            {"lfo_sync", "LFO Sync", "", 0.f, 1.f, 0.f},
            {"lfo_rate", "LFO Rate", "Hz", 0.01f, 30.f, 1.f, true},
            {"lfo_beats", "LFO Synced Rate", "", 0.f, 8.f, 3.f},
            {"lfo_retrig", "LFO Retrigger", "", 0.f, 1.f, 0.f},
            {"lfo_volume", "LFO > Volume", "%", 0.f, 100.f, 0.f},
            {"lfo_pitch", "LFO > Pitch", "ct", 0.f, 1200.f, 0.f},
            {"lfo_filter", "LFO > Filter", "%", 0.f, 100.f, 0.f},
            {"lfo_pan", "LFO > Pan", "%", 0.f, 100.f, 0.f},
            {"pan", "Pan", "", -1.f, 1.f, 0.f},
            {"velocity", "Vol < Vel", "%", 0.f, 100.f, 50.f},
            {"volume", "Volume", "dB", -60.f, 6.f, 0.f},
        };
        list[P::ModeParam].valueLabels = {"Classic", "1-Shot", "Slice"};
        list[P::Root].steps = 127;
        list[P::Tune].steps = 96;
        for (const int p : {P::Reverse, P::Snap, P::Warp, P::Loop, P::FilterOn, P::LfoOn, P::LfoSync, P::LfoRetrig}) {
            list[static_cast<size_t>(p)].valueLabels = offOn;
        }
        list[P::WarpBeats].steps = 255;
        list[P::WarpModeParam].valueLabels = {"Transients", "Standard", "Smooth", "Formants", "Re-Pitch"};
        list[P::Voices].steps = 31;
        list[P::TriggerMode].valueLabels = {"Trigger", "Gate"};
        list[P::SliceBy].valueLabels = {"Transient", "Beat", "Region"};
        list[P::SliceBeat].valueLabels = {"1/16", "1/8", "1/4", "1/2", "1 Bar", "2 Bars", "4 Bars"};
        list[P::Regions].steps = 62;
        list[P::PlaybackParam].valueLabels = {"Mono", "Poly", "Thru"};
        list[P::FilterTypeParam].valueLabels = {"Low-pass", "High-pass", "Band-pass", "Notch"};
        list[P::FilterSlope].valueLabels = {"12 dB", "24 dB"};
        list[P::LfoWaveParam].valueLabels = {"Sine", "Triangle", "Saw Up", "Saw Down", "Square", "Random"};
        list[P::LfoBeats].valueLabels = {"1/32", "1/16", "1/8", "1/4", "1/2", "1 Bar", "2 Bars", "4 Bars", "8 Bars"};
        return list;
    }();
    return kInfos;
}

template <typename E>
E choice(float value) noexcept {
    return static_cast<E>(static_cast<int>(std::lround(value)));
}

}  // namespace

SamplerProcessor::SamplerProcessor() : BuiltinProcessor(infos(), {{"position", kMeterSamples}}) {}

SamplerProcessor::~SamplerProcessor() {
    // Nothing renders it any more.
    delete active_;
    delete pending_.load();
    Sample* old = nullptr;
    while (retired_.pop(old)) delete old;
    delete pool_;
    delete pendingPool_.load();
    StretchPool* oldPool = nullptr;
    while (retiredPools_.pop(oldPool)) delete oldPool;
}

void SamplerProcessor::prepare(double sampleRate, int /*maxBlockSize*/) {
    sampleRate_ = sampleRate;
    for (SmoothedValue* value : {&volume_, &gain_, &pan_}) value->reset(sampleRate, 0.02);
    updatePool(false);
    reset();
}

void SamplerProcessor::reset() {
    for (Voice& voice : voices_) voice.active = false;
    heldCount_ = 0;
    noteCounter_ = 0;
    volume_.snapTo(dbToGain(param(Volume)));
    gain_.snapTo(dbToGain(param(Gain)));
    pan_.snapTo(param(Pan));
    lfoPhase_ = 0.0;
    lfoCycle_ = 0;
    for (Svf& filter : filters_) filter.reset();
    filtering_ = false;
}

void SamplerProcessor::resetOffline() { updatePool(true); }  // fresh stretchers: the render comes out the same every time

int SamplerProcessor::tailSamples() const {
    return static_cast<int>(std::max(param(Release), param(FadeOut)) * 0.001 * sampleRate_);
}

bool SamplerProcessor::idle() {
    updatePool(false);
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
            if (path_ == path) publishSample(std::make_unique<Sample>());  // silent until it can be loaded
            throw;
        }
    }
    {
        std::lock_guard lock(mutex_);
        // Another state came meanwhile; or the same file: notes go on.
        if (path_ != path || source == published_) return;
    }
    // Its transients, as it plays forwards and reversed (not under the lock either).
    auto sample = std::make_unique<Sample>();
    sample->source = std::move(source);
    if (sample->source) {
        const AudioSource& file = *sample->source;
        const float* channels[2] = {file.channelData(0), file.channelData(file.channels() > 1 ? 1u : 0u)};
        const int count = std::min<int>(2, static_cast<int>(file.channels()));
        for (const bool reversed : {false, true}) {
            sample->onsets[reversed ? 1 : 0] =
                slicing::detectOnsets(channels, count, file.frames(), file.sampleRate(), reversed);
        }
    }
    std::lock_guard lock(mutex_);
    if (path_ == path) publishSample(std::move(sample));
}

void SamplerProcessor::publishSample(std::unique_ptr<Sample> sample) {
    collectRetired();
    if (sample->source == published_) return;  // the same file: notes go on
    published_ = sample->source;
    // The rendering thread never took a sample still pending: free it here.
    delete pending_.exchange(sample.release(), std::memory_order_acq_rel);
}

void SamplerProcessor::collectRetired() {
    Sample* old = nullptr;
    while (retired_.pop(old)) delete old;
    StretchPool* oldPool = nullptr;
    while (retiredPools_.pop(oldPool)) delete oldPool;
}

void SamplerProcessor::takeSample() noexcept {
    if (!pending_.load(std::memory_order_acquire)) return;
    if (active_ && !retired_.push(active_)) return;  // the main side hasn't freed the last ones: next block
    // Only this thread empties pending_, so it holds a sample still (perhaps a newer one).
    active_ = pending_.exchange(nullptr, std::memory_order_acq_rel);
    for (Voice& voice : voices_) voice.active = false;  // they played the old one
}

// --- Stretchers ---------------------------------------------------------------

void SamplerProcessor::updatePool(bool fresh) {
    // How many stretched notes may sound at once, and the fading ones they cut short.
    const auto mode = choice<Mode>(param(ModeParam));
    const auto voices = static_cast<size_t>(std::clamp<long>(std::lround(param(Voices)), 1, kMaxStretched));
    const bool monophonic = mode == Mode::OneShot ||
                            (mode == Mode::Slice && choice<Playback>(param(PlaybackParam)) != Playback::Poly) ||
                            (mode == Mode::Classic && voices == 1);
    const auto warpMode = choice<WarpMode>(param(WarpModeParam));
    const bool stretching = param(Warp) >= 0.5f && warpMode != WarpMode::RePitch;
    const size_t wanted = stretching ? (monophonic ? 1 : voices) + 2 : 0;
    const StretchConfig config = stretchConfigFor(warpMode);

    std::lock_guard lock(mutex_);
    collectRetired();
    if (wanted == 0) {
        if (poolSize_ == 0) return;
    } else if (!fresh && poolSize_ >= wanted && poolConfig_ == config && poolRate_ == sampleRate_) {
        return;
    }
    auto pool = std::make_unique<StretchPool>();
    pool->config = config;
    pool->sampleRate = sampleRate_;
    if (wanted > 0) {
        const StretchTiming timing = stretchTiming(config);
        const size_t count = std::max(wanted, fresh && poolConfig_ == config ? poolSize_ : 0);
        for (size_t i = 0; i < count; ++i) {
            auto stretcher = std::make_unique<Stretcher>(kStretchSeed);
            // Split computation spreads each block's work over the interval after it, as the clips' do.
            stretcher->configure(2, static_cast<int>(sampleRate_ * timing.blockSeconds),
                                 static_cast<int>(sampleRate_ * timing.intervalSeconds), true);
            pool->stretchers.push_back(std::move(stretcher));
        }
    }
    poolConfig_ = config;
    poolSize_ = pool->stretchers.size();
    poolRate_ = sampleRate_;
    delete pendingPool_.exchange(pool.release(), std::memory_order_acq_rel);
}

void SamplerProcessor::takePool() noexcept {
    if (!pendingPool_.load(std::memory_order_acquire)) return;
    if (pool_ && !retiredPools_.push(pool_)) return;  // next block
    pool_ = pendingPool_.exchange(nullptr, std::memory_order_acq_rel);
    for (Voice& voice : voices_) {
        if (voice.stretch < 0) continue;
        voice.stretch = -1;  // their stretchers went: they fade out, resampled
        kill(voice);
    }
}

int SamplerProcessor::freeStretcher() noexcept {
    if (!pool_ || pool_->stretchers.empty()) return -1;
    const auto count = static_cast<int>(pool_->stretchers.size());
    for (int s = 0; s < count; ++s) {
        const bool used = std::any_of(voices_.begin(), voices_.end(),
                                      [s](const Voice& voice) { return voice.active && voice.stretch == s; });
        if (!used) return s;
    }
    // All in use: the quietest of the voices fading out gives its up.
    Voice* quietest = nullptr;
    for (Voice& voice : voices_) {
        if (voice.active && voice.killing && voice.stretch >= 0 && (!quietest || voice.kill < quietest->kill)) {
            quietest = &voice;
        }
    }
    if (!quietest) return -1;
    quietest->active = false;
    return quietest->stretch;
}

// --- The block's settings --------------------------------------------------------

int64_t SamplerProcessor::snap(int64_t frame) const noexcept {
    if (param(Snap) < 0.5f) return frame;
    const AudioSource& source = *active_->source;
    const float* channels[2] = {source.channelData(0), source.channelData(source.channels() > 1 ? 1u : 0u)};
    const auto reach = static_cast<int64_t>(slicing::kSnapSeconds * source.sampleRate());
    return slicing::nearestZeroCrossing(channels, std::min<int>(2, static_cast<int>(source.channels())),
                                        source.frames(), reader_.reverse, frame, reach);
}

void SamplerProcessor::readBlock(const ProcessContext& ctx) {
    const AudioSource& source = *active_->source;
    const int64_t frames = source.frames();
    Block& b = block_;
    Reader& r = reader_;
    b.mode = choice<Mode>(param(ModeParam));
    r.data[0] = source.channelData(0);
    r.data[1] = source.channelData(source.channels() > 1 ? 1u : 0u);
    r.frames = frames;
    r.reverse = param(Reverse) >= 0.5f;
    const auto at = [&](int p) { return static_cast<int64_t>(param(p) / 100.0 * static_cast<double>(frames)); };
    b.start = std::clamp(snap(at(Start)), int64_t{0}, frames - 1);
    b.end = std::clamp(snap(at(End)), int64_t{0}, frames);
    r.loop = false;
    if (b.mode == Mode::Classic && param(Loop) >= 0.5f && b.end > b.start) {
        r.loopStart = std::clamp(snap(std::max(at(LoopStart), b.start)), b.start, b.end - 1);
        r.loopEnd = b.end;
        const int64_t length = r.loopEnd - r.loopStart;
        r.loop = length >= 1;
        // Its end fades into what leads to its start, as much as there is of that.
        r.fade = std::min(static_cast<int64_t>(std::llround(param(LoopFade) / 100.0 * static_cast<double>(length))),
                          r.loopStart);
    }
    r.fastEnd = r.loop ? r.loopEnd - r.fade : frames;

    // Speed: the file's rate to the engine's; warped, the whole sample in Warp Length beats.
    b.rate = static_cast<double>(source.sampleRate()) / sampleRate_;
    b.stretch = false;
    if (param(Warp) >= 0.5f) {
        const double tempo = ctx.tempo > 0.0 ? ctx.tempo : 120.0;
        const double beats = std::max(1.0, std::round(static_cast<double>(param(WarpBeats))));
        b.rate = static_cast<double>(frames) / (beats * 60.0 / tempo * sampleRate_);
        const auto mode = choice<WarpMode>(param(WarpModeParam));
        b.stretch = mode != WarpMode::RePitch && b.rate <= kMaxStretchRate && pool_ && !pool_->stretchers.empty();
        b.formants = mode == WarpMode::Formants;
    }
    b.root = static_cast<int>(std::lround(param(Root)));
    b.transpose = std::round(param(Tune)) + param(Fine) / 100.0;

    const double samplesPerMs = sampleRate_ * 0.001;
    const auto fade = [&](float ms) {
        return static_cast<float>(std::exp(std::log(kFadeTo) / std::max(1.0, ms * samplesPerMs)));
    };
    b.attackStep = static_cast<float>(1.0 / std::max(1.0, param(Attack) * samplesPerMs));
    b.decayCoef = fade(param(Decay));
    b.sustain = std::clamp(param(Sustain) / 100.f, 0.f, 1.f);
    b.releaseCoef = fade(param(Release));
    b.fadeInStep = static_cast<float>(1.0 / std::max(1.0, param(FadeIn) * samplesPerMs));
    b.fadeOutSamples = std::max(1.0, param(FadeOut) * samplesPerMs);
    b.fadeOutStep = static_cast<float>(1.0 / b.fadeOutSamples);
    b.gate = param(TriggerMode) >= 0.5f;
    b.playback = choice<Playback>(param(PlaybackParam));
    b.voices = std::clamp(static_cast<int>(std::lround(param(Voices))), 1, kMaxVoices);
    if (b.stretch) b.voices = std::min(b.voices, kMaxStretched);
    b.glideSamples = param(Glide) * samplesPerMs;
    b.killStep = static_cast<float>(1.0 / std::max(1.0, kKillSeconds * sampleRate_));
}

// --- Notes ------------------------------------------------------------------

void SamplerProcessor::kill(Voice& voice) noexcept {
    if (voice.active) voice.killing = true;
}

void SamplerProcessor::killAll() {
    for (Voice& voice : voices_) kill(voice);
}

// Until fewer than `limit` notes sound: the quietest releasing one fades out, else the oldest.
void SamplerProcessor::makeRoom(int limit) {
    for (;;) {
        int sounding = 0;
        Voice* releasing = nullptr;
        Voice* oldest = nullptr;
        for (Voice& voice : voices_) {
            if (!voice.active || voice.killing) continue;
            ++sounding;
            if (voice.stage == Stage::Release && (!releasing || voice.level < releasing->level)) releasing = &voice;
            if (!oldest || voice.started < oldest->started) oldest = &voice;
        }
        if (sounding < limit) return;
        kill(releasing ? *releasing : *oldest);
    }
}

SamplerProcessor::Voice* SamplerProcessor::monoVoice() noexcept {
    Voice* newest = nullptr;
    for (Voice& voice : voices_) {
        if (voice.active && !voice.killing && (!newest || voice.started > newest->started)) newest = &voice;
    }
    return newest;
}

void SamplerProcessor::glideTo(Voice& voice, double pitch) noexcept {
    voice.pitchTarget = pitch;
    if (block_.glideSamples < 1.0) {
        voice.pitch = pitch;
        voice.glideStep = 0.0;
    } else {
        voice.glideStep = std::abs(pitch - voice.pitch) / block_.glideSamples;
    }
}

SamplerProcessor::Voice* SamplerProcessor::startVoice(uint8_t key, uint8_t velocity, int64_t from, int64_t end,
                                                      double pitch) {
    // A free slot; else the quietest voice fading out.
    Voice* target = nullptr;
    for (Voice& voice : voices_) {
        if (!voice.active) {
            target = &voice;
            break;
        }
    }
    if (!target) {
        for (Voice& voice : voices_) {
            if (voice.killing && (!target || voice.kill < target->kill)) target = &voice;
        }
    }
    if (!target) return nullptr;
    target->active = false;  // (its stretcher free for this note)
    const int stretch = block_.stretch ? freeStretcher() : -1;
    const float sensitivity = std::clamp(param(Velocity) / 100.f, 0.f, 1.f);
    Voice& voice = *target;
    voice = Voice{};
    voice.active = true;
    voice.key = key;
    voice.classic = block_.mode == Mode::Classic;
    voice.started = ++noteCounter_;
    voice.gain = 1.f - sensitivity * (1.f - static_cast<float>(velocity) / 127.f);
    voice.position = static_cast<double>(from);
    voice.end = end;
    voice.pitch = voice.pitchTarget = pitch;
    voice.stretch = stretch;
    if (stretch >= 0) {
        // Its first output is `from`, at once: the stretcher's latency computed ahead.
        Stretcher& stretcher = *pool_->stretchers[static_cast<size_t>(stretch)];
        voice.stretchSemitones = static_cast<float>(pitch + block_.transpose);
        stretcher.setTransposeSemitones(voice.stretchSemitones);
        stretcher.setFormantFactor(1.f, block_.formants);
        const int seekLength = stretcher.outputSeekLength(static_cast<float>(block_.rate));
        const bool loops = voice.classic && reader_.loop;
        const StretchInput input{&reader_, from, end >= 0 ? end : block_.end, loops};
        stretcher.outputSeek(input, seekLength);
        voice.input = loops ? reader_.wrapped(from + seekLength) : from + seekLength;
    }
    if (param(LfoOn) >= 0.5f && param(LfoRetrig) >= 0.5f) {
        lfoPhase_ = 0.0;
        lfoCycle_ = noteCounter_ << 32;  // a new random value too
    }
    return &voice;
}

void SamplerProcessor::holdKey(uint8_t key) noexcept {
    releaseKey(key);
    if (heldCount_ < static_cast<int>(heldKeys_.size())) heldKeys_[static_cast<size_t>(heldCount_++)] = key;
}

void SamplerProcessor::releaseKey(uint8_t key) noexcept {
    for (int i = 0; i < heldCount_; ++i) {
        if (heldKeys_[static_cast<size_t>(i)] == key) {
            std::copy(heldKeys_.begin() + i + 1, heldKeys_.begin() + heldCount_, heldKeys_.begin() + i);
            --heldCount_;
            return;
        }
    }
}

void SamplerProcessor::noteOn(uint8_t key, uint8_t velocity) {
    holdKey(key);
    const Block& b = block_;
    if (b.end <= b.start) return;  // nothing to play
    switch (b.mode) {
        case Mode::Classic: {
            const double pitch = key - b.root;
            if (b.voices == 1) {
                // Legato with a glide: the note playing goes on, at the new pitch.
                Voice* playing = monoVoice();
                if (b.glideSamples >= 1.0 && playing && playing->held && playing->classic) {
                    playing->key = key;
                    glideTo(*playing, pitch);
                    return;
                }
                killAll();
            } else {
                makeRoom(b.voices);
            }
            startVoice(key, velocity, b.start, -1, pitch);
            break;
        }
        case Mode::OneShot:
            killAll();
            startVoice(key, velocity, b.start, -1, key - b.root);
            break;
        case Mode::Slice: {
            const int index = key - slicing::kFirstSliceKey;
            if (index < 0) return;
            const std::vector<slicing::Onset>& onsets = active_->onsets[reader_.reverse ? 1 : 0];
            slicing::SliceSettings settings;
            settings.by = choice<slicing::SliceBy>(param(SliceBy));
            settings.sensitivity = param(Sensitivity) / 100.f;
            const double frames = static_cast<double>(reader_.frames);
            settings.regionBeats =
                std::max(1.0, std::round(static_cast<double>(param(WarpBeats)))) * static_cast<double>(b.end - b.start) / frames;
            const auto division = std::clamp(static_cast<int>(std::lround(param(SliceBeat))), 0, 6);
            settings.divisionBeats = slicing::kSliceDivisionBeats[division];
            settings.regions = static_cast<int>(std::lround(param(Regions)));
            std::array<int64_t, slicing::kMaxSlices> starts{};
            const int count = slicing::sliceStarts(settings, onsets.data(), onsets.size(), b.start, b.end,
                                                   active_->source->sampleRate(), starts.data());
            if (index >= count) return;
            const int64_t from = snap(starts[static_cast<size_t>(index)]);
            const int64_t to = b.playback == Playback::Thru || index + 1 >= count
                                   ? b.end
                                   : snap(starts[static_cast<size_t>(index) + 1]);
            if (to <= from) return;
            if (b.playback == Playback::Poly) makeRoom(b.voices);
            else killAll();
            startVoice(key, velocity, from, to, 0.0);
            break;
        }
    }
}

void SamplerProcessor::noteOff(uint8_t key) {
    releaseKey(key);
    const Block& b = block_;
    if (b.mode == Mode::Classic && b.voices == 1 && b.glideSamples >= 1.0) {
        // Legato: letting go of the key playing goes back to the newest still held.
        Voice* playing = monoVoice();
        if (playing && playing->held && playing->key == key && heldCount_ > 0) {
            playing->key = heldKeys_[static_cast<size_t>(heldCount_ - 1)];
            glideTo(*playing, playing->key - b.root);
            return;
        }
    }
    // With the same key held twice, the older note ends first.
    Voice* target = nullptr;
    for (Voice& voice : voices_) {
        if (voice.active && !voice.killing && voice.held && voice.key == key &&
            (!target || voice.started < target->started)) {
            target = &voice;
        }
    }
    if (!target) return;
    target->held = false;
    if (target->classic || b.gate) target->stage = Stage::Release;  // (Trigger: it plays on)
}

// --- Rendering -------------------------------------------------------------------

float SamplerProcessor::envelope(Voice& voice) const noexcept {
    const Block& b = block_;
    switch (voice.stage) {
        case Stage::Attack:
            voice.level += voice.classic ? b.attackStep : b.fadeInStep;
            if (voice.level >= 1.f) {
                voice.level = 1.f;
                voice.stage = voice.classic ? Stage::Decay : Stage::Hold;
            }
            break;
        case Stage::Decay:  // then holds the sustain level (and follows it if it changes)
            voice.level = b.sustain + (voice.level - b.sustain) * b.decayCoef;
            break;
        case Stage::Hold:
            break;
        case Stage::Release:
            if (voice.classic) voice.level *= b.releaseCoef;
            else voice.level = std::max(0.f, voice.level - b.fadeOutStep);
            break;
    }
    return voice.level;
}

bool SamplerProcessor::finished(const Voice& voice) const noexcept {
    if (voice.killing && voice.kill <= 0.f) return true;
    if (!voice.classic) return voice.stage == Stage::Release && voice.level <= 0.f;
    const bool faded = voice.stage == Stage::Release || (voice.stage == Stage::Decay && block_.sustain < kSilent);
    return faded && voice.level < kSilent;
}

void SamplerProcessor::renderVoice(Voice& voice, int from, int to, double semitones, float* left, float* right) {
    const Block& b = block_;
    const Reader& r = reader_;
    const bool loops = voice.classic && r.loop;
    const int64_t end = voice.end >= 0 ? voice.end : b.end;
    const auto stop = static_cast<double>(end);
    // The voice's gain at a sample: its envelope, its velocity, its fade if cut
    // short, and (1-Shot and Slice) its fade before it ends, `step` frames a sample.
    const auto gainAt = [&](double position, double step) {
        float gain = envelope(voice) * voice.gain;
        if (voice.killing) {
            voice.kill = std::max(0.f, voice.kill - b.killStep);
            gain *= voice.kill;
        }
        if (!voice.classic) {
            const double remaining = (stop - position) / std::max(step, 1e-9);
            if (remaining < b.fadeOutSamples) gain *= static_cast<float>(std::max(0.0, remaining) / b.fadeOutSamples);
        }
        return gain;
    };

    if (voice.stretch >= 0 && pool_) {
        Stretcher& stretcher = *pool_->stretchers[static_cast<size_t>(voice.stretch)];
        const auto wanted = static_cast<float>(semitones);
        if (std::abs(wanted - voice.stretchSemitones) > 1e-4f) {
            stretcher.setTransposeSemitones(wanted);
            voice.stretchSemitones = wanted;
        }
        const int frames = to - from;
        voice.inputDebt += b.rate * frames;
        const auto count = static_cast<int>(voice.inputDebt);
        voice.inputDebt -= count;
        std::array<float, kChunk> outL{}, outR{};
        float* outputs[2] = {outL.data(), outR.data()};
        stretcher.process(StretchInput{&r, voice.input, end, loops}, count, outputs, frames);
        voice.input = loops ? r.wrapped(voice.input + count) : voice.input + count;
        for (int i = 0; i < frames; ++i) {
            if (!loops && voice.position >= stop) {
                voice.active = false;
                return;
            }
            const float gain = gainAt(voice.position, b.rate);
            left[from + i] += outL[static_cast<size_t>(i)] * gain;
            right[from + i] += outR[static_cast<size_t>(i)] * gain;
            voice.position = loops ? r.wrapped(voice.position + b.rate) : voice.position + b.rate;
        }
    } else {
        const double increment = b.rate * std::exp2(semitones / 12.0);
        for (int i = from; i < to; ++i) {
            if (voice.position >= stop) {
                if (!loops) {
                    voice.active = false;
                    return;
                }
                voice.position = r.wrapped(voice.position);
            }
            const float gain = gainAt(voice.position, increment);
            left[i] += r.interpolated(0, voice.position) * gain;
            right[i] += r.interpolated(1, voice.position) * gain;
            voice.position += increment;
        }
        if (loops) voice.position = r.wrapped(voice.position);
    }
    if (finished(voice)) voice.active = false;
}

float SamplerProcessor::lfoValue(double phase, uint64_t cycle) const noexcept {
    const auto p = static_cast<float>(phase);
    switch (choice<LfoWave>(param(LfoWaveParam))) {
        case LfoWave::Sine: return static_cast<float>(std::sin(2.0 * kPi * phase));
        case LfoWave::Triangle: return p < 0.25f ? 4.f * p : p < 0.75f ? 2.f - 4.f * p : 4.f * p - 4.f;
        case LfoWave::SawUp: return 2.f * p - 1.f;
        case LfoWave::SawDown: return 1.f - 2.f * p;
        case LfoWave::Square: return p < 0.5f ? 1.f : -1.f;
        case LfoWave::Random: return randomOf(cycle);
    }
    return 0.f;
}

void SamplerProcessor::filterChunk(int from, int to, float* left, float* right, float lfo) {
    const auto type = choice<FilterType>(param(FilterTypeParam));
    const bool steep = param(FilterSlope) >= 0.5f;
    const double nyquistish = 0.45 * sampleRate_;
    const double target = std::clamp(
        static_cast<double>(param(FilterFreq)) * std::exp2(param(LfoFilter) / 100.0 * kLfoFilterOctaves * lfo),
        20.0, nyquistish);
    if (!filtering_) {  // switched on: from its cutoff, silent
        filtering_ = true;
        cutoff_ = target;
        for (Svf& filter : filters_) filter.reset();
    } else {
        const double glide = 1.0 - std::exp(-(to - from) / (kCutoffGlideSeconds * sampleRate_));
        cutoff_ *= std::pow(target / cutoff_, glide);
    }
    const auto g = static_cast<float>(std::tan(kPi * std::min(cutoff_, nyquistish) / sampleRate_));
    // Resonance raises the (last) section's Q from its own towards kMaxResonanceQ.
    const double resonance = std::clamp(param(FilterRes) / 100.0, 0.0, 1.0);
    const auto damping = [&](double q) { return static_cast<float>(1.0 / (q * std::pow(kMaxResonanceQ / q, resonance))); };
    const bool butterworth = type == FilterType::LowPass || type == FilterType::HighPass;
    // 24 dB: two sections, a Butterworth pair for low- and high-pass.
    const SvfCoefficients first(g, steep && butterworth ? static_cast<float>(1.0 / 0.5412) : damping(0.7071));
    const SvfCoefficients second(g, butterworth ? damping(1.3066) : damping(0.7071));
    float* sides[2] = {left, right};
    for (int c = 0; c < (right ? 2 : 1); ++c) {
        Svf& a = filters_[static_cast<size_t>(c) * 2];
        Svf& b = filters_[static_cast<size_t>(c) * 2 + 1];
        float* x = sides[c];
        for (int i = from; i < to; ++i) {
            float y = filterSample(a, first, type, x[i]);
            if (steep) y = filterSample(b, second, type, y);
            x[i] = y;
        }
    }
}

// The filter still rings after the last voice (a resonant one for tens of milliseconds).
bool SamplerProcessor::filterRinging() const noexcept {
    if (!filtering_) return false;
    return std::any_of(filters_.begin(), filters_.end(), [](const Svf& filter) {
        return std::abs(filter.ic1) > kSilent * 0.1f || std::abs(filter.ic2) > kSilent * 0.1f;
    });
}

void SamplerProcessor::renderChunk(const ProcessContext& ctx, int from, int to, float* left, float* right) {
    const int frames = to - from;
    // The LFO at the chunk's start and end (the gains glide between them).
    float lfoFrom = 0.f, lfoTo = 0.f;
    const bool lfoOn = param(LfoOn) >= 0.5f;
    if (lfoOn) {
        const bool synced = param(LfoSync) >= 0.5f;
        const double tempo = ctx.tempo > 0.0 ? ctx.tempo : 120.0;
        const double beats = kLfoBeats[std::clamp(static_cast<int>(std::lround(param(LfoBeats))), 0, 8)];
        const double hz = synced ? tempo / 60.0 / beats : static_cast<double>(param(LfoRate));
        if (synced && ctx.playing && param(LfoRetrig) < 0.5f) {
            // Locked to the song: its cycles start on the beat.
            const double cycles = (ctx.beatPos + from * tempo / 60.0 / sampleRate_) / beats;
            const double whole = std::floor(cycles);
            lfoCycle_ = static_cast<uint64_t>(static_cast<int64_t>(whole));
            lfoPhase_ = cycles - whole;
        }
        lfoFrom = lfoValue(lfoPhase_, lfoCycle_);
        lfoPhase_ += hz * frames / sampleRate_;
        if (lfoPhase_ >= 1.0) {
            const double whole = std::floor(lfoPhase_);
            lfoCycle_ += static_cast<uint64_t>(whole);
            lfoPhase_ -= whole;
        }
        lfoTo = lfoValue(lfoPhase_, lfoCycle_);
    }
    const float lfoMiddle = 0.5f * (lfoFrom + lfoTo);

    // The voices, each at its pitch (gliding, and the LFO's vibrato).
    const double vibrato = lfoOn ? param(LfoPitch) / 100.0 * lfoMiddle : 0.0;
    float* mixRight = right ? right : left;
    for (Voice& voice : voices_) {
        if (!voice.active) continue;
        if (voice.pitch != voice.pitchTarget) {
            const double step = voice.glideStep * frames;
            voice.pitch = voice.pitch < voice.pitchTarget ? std::min(voice.pitchTarget, voice.pitch + step)
                                                          : std::max(voice.pitchTarget, voice.pitch - step);
        }
        renderVoice(voice, from, to, voice.pitch + block_.transpose + vibrato, left, mixRight);
    }

    if (param(FilterOn) >= 0.5f) filterChunk(from, to, left, right, lfoOn ? lfoMiddle : 0.f);
    else filtering_ = false;

    // Gain, the LFO's tremolo and pan, pan, volume.
    gain_.setTarget(dbToGain(param(Gain)));
    volume_.setTarget(dbToGain(param(Volume)));
    pan_.setTarget(param(Pan));
    const float tremolo = lfoOn ? param(LfoVolume) / 100.f : 0.f;
    const float panDepth = lfoOn ? param(LfoPan) / 100.f : 0.f;
    const float sides = right ? 1.f : 0.5f;
    for (int i = 0; i < frames; ++i) {
        const float lfo = lfoFrom + (lfoTo - lfoFrom) * (static_cast<float>(i) + 1.f) / static_cast<float>(frames);
        const float gain = gain_.next() * volume_.next() * (1.f - tremolo * 0.5f * (1.f - lfo)) * sides;
        float l = 1.f, rr = 1.f;
        const float pan = pan_.next() + panDepth * lfo;
        if (right && pan != 0.f) balanceGains(pan, l, rr);
        left[from + i] *= gain * l;
        if (right) right[from + i] *= gain * rr;
    }
}

void SamplerProcessor::render(const ProcessContext& ctx, float* const* channels, int numChannels, int numFrames) {
    if (numChannels <= 0) return;
    takeSample();
    takePool();
    float* left = channels[0];
    float* right = numChannels > 1 ? channels[1] : nullptr;
    std::fill_n(left, numFrames, 0.f);
    if (right) std::fill_n(right, numFrames, 0.f);

    const AudioSource* source = active_ ? active_->source.get() : nullptr;
    const int64_t frames = source ? source->frames() : 0;
    const bool silent = ctx.inEvents.count == 0 && !filterRinging() &&
                        std::none_of(voices_.begin(), voices_.end(), [](const Voice& voice) { return voice.active; });
    if (frames <= 0 || silent) {  // nothing to play: skip the work, and start the next note at the current settings
        for (size_t e = 0; e < ctx.inEvents.count; ++e) {  // (the keys held still count, for legato)
            const ProcessEvent& event = ctx.inEvents.events[e];
            if (event.type == ProcessEvent::Type::NoteOn && event.velocity() > 0) holdKey(event.key());
            else if (event.type == ProcessEvent::Type::NoteOn || event.type == ProcessEvent::Type::NoteOff)
                releaseKey(event.key());
        }
        volume_.snapTo(dbToGain(param(Volume)));
        gain_.snapTo(dbToGain(param(Gain)));
        pan_.snapTo(param(Pan));
        filtering_ = false;
        advance(ctx, 0, numFrames, nullptr, nullptr);
        return;
    }
    readBlock(ctx);
    int position = 0;
    for (size_t e = 0; e < ctx.inEvents.count; ++e) {
        const ProcessEvent& event = ctx.inEvents.events[e];
        const int at = std::clamp(static_cast<int>(event.sampleOffset), position, numFrames);
        advance(ctx, position, at, left, right);
        position = at;
        if (event.type == ProcessEvent::Type::NoteOn && event.velocity() > 0) {
            noteOn(event.key(), event.velocity());
        } else if (event.type == ProcessEvent::Type::NoteOn || event.type == ProcessEvent::Type::NoteOff) {
            noteOff(event.key());
        }
    }
    advance(ctx, position, numFrames, left, right);
}

void SamplerProcessor::advance(const ProcessContext& ctx, int from, int to, float* left, float* right) {
    while (from < to) {
        const int until = std::min({to, from + kChunk, from + (kMeterSamples - meterCount_)});
        if (left) renderChunk(ctx, from, until, left, right);
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
        if (voice.active && !voice.killing && (!newest || voice.started > newest->started)) newest = &voice;
    }
    if (!newest || frames <= 0) return -1.f;
    return static_cast<float>(std::min(1.0, newest->position / static_cast<double>(frames)));
}

SUB_REGISTER_BUILTIN(SamplerProcessor, Instrument);

}  // namespace sub
