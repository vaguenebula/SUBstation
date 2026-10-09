// The built-in Sampler: its sample (its state besides its parameters), playing
// it across the keyboard, and swapping it while it plays; then what it does as
// Ableton's Simpler does: its modes (Classic, 1-Shot, Slice), slicing, the
// loop and its crossfade, reverse, snap, gain and pan, the filter, the LFO,
// voices and glide, warping. Rendered offline.

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <tuple>

#include "Engine.h"
#include "builtin/SampleSlicing.h"
#include "harness/Fixtures.h"
#include "harness/Signal.h"

using namespace subtest;

namespace {

using Bytes = std::vector<uint8_t>;
using Values = std::vector<std::pair<std::string, float>>;

Bytes bytes(const std::string& text) { return {text.begin(), text.end()}; }

// A built-in device's state as the engine stores it (BuiltinProcessor::encodeState):
// "name=value" lines, sorted by name, a backslash escaping a backslash or a newline.
Bytes encodeState(const std::map<std::string, std::string>& values) {
    std::string text;
    for (const auto& [name, value] : values) {
        text += name + "=";
        for (const char c : value) {
            if (c == '\\') text += "\\\\";
            else if (c == '\n') text += "\\n";
            else text += c;
        }
        text += "\n";
    }
    return bytes(text);
}

std::map<std::string, std::string> decodeState(const Bytes& state) {
    std::map<std::string, std::string> values;
    std::string line;
    const std::string text(state.begin(), state.end());
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = std::min(text.find('\n', start), text.size());
        line = text.substr(start, end - start);
        start = end + 1;
        const size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        std::string value;
        for (size_t i = equals + 1; i < line.size(); ++i) {
            if (line[i] == '\\' && i + 1 < line.size()) {
                ++i;
                value += line[i] == 'n' ? '\n' : line[i];
            } else {
                value += line[i];
            }
        }
        values[line.substr(0, equals)] = value;
    }
    return values;
}

Bytes state(const std::string& path) { return encodeState({{"sample", path}}); }

// A track with a Sampler playing `notes`: (track, device).
std::pair<uint32_t, uint32_t> samplerTrack(sub::Engine& engine, const std::vector<sub::NoteDesc>& notes,
                                           const std::string& path = {}, const Values& values = {}) {
    const uint32_t track = engine.addTrack();
    const uint32_t device = engine.addBuiltinProcessor(engine.trackChain(track), "sampler", -1);
    Values all{{"velocity", 0.f}, {"release", 1.f}};
    for (const auto& [name, value] : values) {
        auto it = std::find_if(all.begin(), all.end(), [&](const auto& v) { return v.first == name; });
        if (it != all.end()) it->second = value;
        else all.emplace_back(name, value);
    }
    for (const auto& [name, value] : all) setParam(engine, device, name, value);
    if (!path.empty()) engine.setProcessorState(device, state(path));
    engine.setTrackNotes(track, notes);
    return {track, device};
}

Samples leftFrom(const Samples& out, int64_t from, int64_t to = std::numeric_limits<int64_t>::max()) {
    return channel(frames(out, from, to), 0);
}

}  // namespace

TEST_CASE("the sampler is an instrument, listed with its parameters") {
    const sub::BuiltinInfo info = builtinInfo("sampler");
    CHECK_EQ(info.name, std::string("Sampler"));
    CHECK(info.isInstrument());
    CHECK(paramIds(info.params) ==
          (std::vector<std::string>{"mode",        "root",       "tune",        "fine",        "start",      "end",
                                    "gain",        "reverse",    "snap",        "warp",        "warp_beats", "warp_mode",
                                    "loop",        "loop_start", "loop_fade",   "attack",      "decay",      "sustain",
                                    "release",     "voices",     "glide",       "trigger",     "fade_in",    "fade_out",
                                    "slice_by",    "sensitivity", "slice_beat", "regions",     "playback",   "filter",
                                    "filter_type", "filter_slope", "filter_freq", "filter_res", "lfo",       "lfo_wave",
                                    "lfo_sync",    "lfo_rate",   "lfo_beats",   "lfo_retrig",  "lfo_volume", "lfo_pitch",
                                    "lfo_filter",  "lfo_pan",    "pan",         "velocity",    "volume"}));
    // What an old project left out plays as it did: Classic, nothing else on.
    const auto defaultOf = [&](const std::string& id) {
        for (const sub::ParamInfo& p : info.params)
            if (p.id == id) return p.defaultValue;
        return -1.f;
    };
    for (const char* id : {"mode", "reverse", "snap", "warp", "loop_start", "loop_fade", "filter", "lfo", "pan", "glide"})
        CHECK_EQ(defaultOf(id), 0.f);
    CHECK_EQ(defaultOf("voices"), 32.f);
    CHECK_EQ(defaultOf("gain"), 0.f);
}

TEST_CASE("without a sample it is silent and has no state") {
    sub::Engine engine;
    const auto [track, device] = samplerTrack(engine, {{0.0, 1.0, 60}});
    CHECK(engine.processorState(device).empty());
    CHECK(!anyNonzero(engine.renderOffline(0.0, kBeat)));
}

TEST_CASE("its state round-trips its path") {
    sub::Engine engine;
    const std::string path = makeWav(sine(440, 1.0));  // (on Windows: backslashes, escaped and back)
    const auto [track, device] = samplerTrack(engine, {}, path);
    CHECK(engine.processorState(device) == state(path));
    CHECK(decodeState(engine.processorState(device)) == (std::map<std::string, std::string>{{"sample", path}}));
}

TEST_CASE("it plays the sample at its root key") {
    sub::Engine engine;
    const std::string path = makeWav(sine(440, 1.0));
    samplerTrack(engine, {{0.0, 1.0, 60, 127}}, path);
    const Samples out = engine.renderOffline(0.0, kBeat);
    CHECK_APPROX_REL(dominantFreq(leftFrom(out, 1000)), 440, 1e-3);
    CHECK_APPROX_REL(rms(leftFrom(out, 1000)), 0.5 / std::sqrt(2.0), 0.02);
    CHECK(allclose(channel(out, 0), channel(out, 1)));  // a mono sample on both channels
}

TEST_CASE("pitch follows the key, the root and the tuning") {
    const std::vector<std::tuple<int, Values, double>> cases{
        {72, {}, 880},                            // an octave up
        {60, {{"tune", -12.f}}, 220},             // transposed down
        {60, {{"root", 48.f}}, 880},              // played an octave above its root
        {60, {{"fine", 100.f}}, 440 * std::pow(2.0, 1 / 12.0)},
    };
    for (const auto& [key, values, freq] : cases) {
        INFO("key " + std::to_string(key) + ", " + std::to_string(freq) + " Hz");
        sub::Engine engine;
        const std::string path = makeWav(sine(440, 1.0));
        samplerTrack(engine, {{0.0, 1.0, key, 127}}, path, values);
        const Samples out = engine.renderOffline(0.0, kBeat);
        CHECK_APPROX_REL(dominantFreq(leftFrom(out, 1000)), freq, 2e-3);
    }
}

TEST_CASE("a file at another rate plays at its pitch") {
    sub::Engine engine;
    const std::string path = makeWav(sine(440, 1.0, 0.5, 22050), 1, 22050);
    samplerTrack(engine, {{0.0, 1.0, 60, 127}}, path);
    const Samples out = engine.renderOffline(0.0, kBeat);
    CHECK_APPROX_REL(dominantFreq(leftFrom(out, 1000)), 440, 2e-3);
}

TEST_CASE("a note ends with the sample unless it loops") {
    sub::Engine engine;
    const std::string path = makeWav(sine(440, 0.1));  // shorter than the note
    const auto [track, device] = samplerTrack(engine, {{0.0, 2.0, 60, 127}}, path);
    Samples out = engine.renderOffline(0.0, kBeat);
    CHECK(rms(leftFrom(out, 2000, 4000)) > 0.3);
    CHECK(!anyNonzero(leftFrom(out, static_cast<int64_t>(0.11 * kSampleRate))));

    setParam(engine, device, "loop", 1.f);
    out = engine.renderOffline(0.0, kBeat);
    CHECK(rms(leftFrom(out, kBeat - 4800)) > 0.3);  // still going, half a second in
}

TEST_CASE("start and end choose the part played") {
    // Half a second of 440 Hz, then half a second of 880 Hz: start in the second half.
    sub::Engine engine;
    Samples samples = sine(440, 0.5);
    const Samples high = sine(880, 0.5);
    samples.insert(samples.end(), high.begin(), high.end());
    const std::string path = makeWav(samples);
    samplerTrack(engine, {{0.0, 1.0, 60, 127}}, path, {{"start", 50.f}});
    const Samples out = engine.renderOffline(0.0, kBeat);
    CHECK_APPROX_REL(dominantFreq(leftFrom(out, 1000, 20000)), 880, 2e-3);
    CHECK(!anyNonzero(leftFrom(out, static_cast<int64_t>(0.51 * kSampleRate))));
}

TEST_CASE("velocity sensitivity") {
    sub::Engine engine;
    const std::string path = makeWav(sine(440, 1.0));
    samplerTrack(engine, {{0.0, 1.0, 60, 64}}, path, {{"velocity", 100.f}});
    const Samples out = engine.renderOffline(0.0, kBeat);
    CHECK_APPROX_REL(rms(leftFrom(out, 1000)), 0.5 / std::sqrt(2.0) * 64 / 127, 0.02);
}

TEST_CASE("swapping the sample while it plays") {
    sub::Engine engine;
    const std::string low = makeWav(sine(440, 1.0)), high = makeWav(sine(880, 1.0));
    const auto [track, device] = samplerTrack(engine, {{0.0, 8.0, 60, 127}}, low, {{"loop", 1.f}});
    const Samples first = engine.renderOffline(0.0, kBeat);
    CHECK_APPROX_REL(dominantFreq(leftFrom(first, 1000)), 440, 2e-3);

    // The playing note stops with its sample; the next one plays the new sample.
    engine.setProcessorState(device, state(high));
    CHECK(!anyNonzero(engine.renderOffline(0.5, kBeat)));
    Samples out = engine.renderOffline(0.0, kBeat);
    CHECK_APPROX_REL(dominantFreq(leftFrom(out, 1000)), 880, 2e-3);

    // Many swaps, faster than blocks render: the last one wins.
    for (int n = 0; n < 40; ++n) {
        engine.setProcessorState(device, state(n % 2 ? low : high));
        if (n % 3 == 0) engine.renderOffline(0.0, 256);
    }
    engine.idle();
    out = engine.renderOffline(0.0, kBeat);
    CHECK_APPROX_REL(dominantFreq(leftFrom(out, 1000)), 440, 2e-3);

    engine.setProcessorState(device, {});  // no sample
    CHECK(engine.processorState(device).empty());
    CHECK(!anyNonzero(engine.renderOffline(0.0, kBeat)));
}

TEST_CASE("a missing file is an error but stays in its state") {
    sub::Engine engine;
    const std::string missing = (tempDir() / "gone.wav").string();
    const auto [track, device] = samplerTrack(engine, {{0.0, 1.0, 60}});
    CHECK_THROWS_AS(engine.setProcessorState(device, state(missing)), std::runtime_error);
    CHECK(engine.processorState(device) == state(missing));
    CHECK(!anyNonzero(engine.renderOffline(0.0, kBeat)));
}

TEST_CASE("unknown state values are ignored") {
    sub::Engine engine;
    const std::string path = makeWav(sine(440, 1.0));
    const auto [track, device] = samplerTrack(engine, {{0.0, 1.0, 60, 127}});
    Bytes mixed = bytes("from_the_future=1\n");
    const Bytes known = state(path);
    mixed.insert(mixed.end(), known.begin(), known.end());
    const Bytes junk = bytes("no equals sign\n");
    mixed.insert(mixed.end(), junk.begin(), junk.end());
    engine.setProcessorState(device, mixed);
    CHECK(rms(leftFrom(engine.renderOffline(0.0, kBeat), 1000)) > 0.3);
}

TEST_CASE("the position display follows the note") {
    sub::Engine engine;
    const std::string path = makeWav(sine(440, 1.0));  // one second
    const auto [track, device] = samplerTrack(engine, {{0.0, 1.0, 60, 127}}, path);
    const auto displays = engine.processorDisplays(device);
    REQUIRE(displays.size() == 1);
    CHECK_EQ(displays[0].id, std::string("position"));
    CHECK_EQ(displays[0].samplesPerValue, 256);
    engine.renderOffline(0.0, kBeat + 4800);  // the note plays half of it, then releases (1 ms)
    std::vector<float> values;
    engine.readProcessorDisplay(device, 0, 0, values);
    std::vector<float> playing;
    for (const float v : values)
        if (v >= 0.f) playing.push_back(v);
    CHECK(playing.size() > 80);
    bool rising = true;
    for (size_t i = 1; i < playing.size(); ++i) rising = rising && playing[i] - playing[i - 1] > 0.f;
    CHECK(rising);
    REQUIRE(!playing.empty());
    CHECK_NEAR(playing.back(), 0.5, 0.02);
    REQUIRE(!values.empty());
    CHECK_EQ(values.back(), -1.f);  // none plays at the end
}

// --- As Simpler ---------------------------------------------------------------------

namespace {

constexpr double kPiD = 3.14159265358979323846;

// Tones one after another, each `seconds` long.
Samples tones(const std::vector<double>& freqs, double seconds, double amplitude = 0.5) {
    Samples out;
    for (const double freq : freqs) {
        const Samples tone = sine(freq, seconds, amplitude);
        out.insert(out.end(), tone.begin(), tone.end());
    }
    return out;
}

// Hits: a tone at `freqs[i]` starting at `at[i]` seconds, `amplitudes[i]` loud,
// decaying (50 ms), in `seconds` of silence.
Samples hits(const std::vector<double>& at, const std::vector<double>& amplitudes, const std::vector<double>& freqs,
             double seconds) {
    Samples out(static_cast<size_t>(seconds * kSampleRate), 0.f);
    for (size_t h = 0; h < at.size(); ++h) {
        const auto from = static_cast<size_t>(at[h] * kSampleRate);
        for (size_t i = from; i < out.size(); ++i) {
            const double t = static_cast<double>(i - from) / kSampleRate;
            out[i] += static_cast<float>(amplitudes[h] * std::exp(-t / 0.05) * std::sin(2 * kPiD * freqs[h] * t));
        }
    }
    return out;
}

// How loud a tone at `freq` is in `samples` (its amplitude, by projection: a whole number of its cycles is best).
double toneLevel(const Samples& samples, double freq) {
    double re = 0.0, im = 0.0;
    for (size_t i = 0; i < samples.size(); ++i) {
        const double phase = 2 * kPiD * freq * static_cast<double>(i) / kSampleRate;
        re += samples[i] * std::cos(phase);
        im += samples[i] * std::sin(phase);
    }
    return 2.0 * std::hypot(re, im) / static_cast<double>(samples.size());
}

// The frequency by rising zero crossings over `samples`.
double crossingFreq(const Samples& samples) {
    int64_t first = -1, last = -1, count = 0;
    for (size_t i = 1; i < samples.size(); ++i) {
        if (samples[i - 1] < 0.f && samples[i] >= 0.f) {
            if (first < 0) first = static_cast<int64_t>(i);
            else ++count;
            last = static_cast<int64_t>(i);
        }
    }
    return count > 0 ? count * kSampleRate / static_cast<double>(last - first) : 0.0;
}

// The biggest jump from one sample to the next: a click.
double largestStep(const Samples& samples) {
    double most = 0.0;
    for (size_t i = 1; i < samples.size(); ++i) most = std::max(most, std::abs(double(samples[i]) - samples[i - 1]));
    return most;
}

double dB(double ratio) { return 20.0 * std::log10(std::max(ratio, 1e-12)); }

}  // namespace

TEST_CASE("slicing: transients where hits start, stronger ones first to count") {
    const std::vector<double> times{0.1, 0.35, 0.6, 0.85};
    const Samples sample = hits(times, {0.9, 0.3, 0.9, 0.1}, {200, 300, 400, 500}, 1.0);
    const float* channels[] = {sample.data()};
    const auto frames = static_cast<int64_t>(sample.size());
    const std::vector<sub::slicing::Onset> onsets = sub::slicing::detectOnsets(channels, 1, frames, kSampleRate, false);
    for (const double seconds : times) {
        const auto hit = static_cast<int64_t>(seconds * kSampleRate);
        const bool found = std::any_of(onsets.begin(), onsets.end(), [&](const sub::slicing::Onset& onset) {
            return onset.frame <= hit + 48 && onset.frame >= hit - 96;  // at the hit (just before at most 2 ms)
        });
        INFO("hit at " + std::to_string(hit));
        CHECK(found);
    }
    for (const sub::slicing::Onset& onset : onsets) CHECK(onset.strength > 0.f && onset.strength <= 1.f);

    // More sensitive, more slices; every slice starts at a hit (or Start).
    std::array<int64_t, sub::slicing::kMaxSlices> starts{};
    sub::slicing::SliceSettings settings;
    std::vector<int> counts;
    for (const float sensitivity : {0.f, 0.5f, 1.f}) {
        settings.sensitivity = sensitivity;
        counts.push_back(sub::slicing::sliceStarts(settings, onsets.data(), onsets.size(), 0, frames, kSampleRate,
                                                   starts.data()));
        CHECK_EQ(starts[0], int64_t{0});
    }
    CHECK(counts[0] < counts[2]);
    CHECK(counts[0] <= counts[1] && counts[1] <= counts[2]);
    CHECK_EQ(counts[2], 5);  // Start, and the four hits

    // Played backwards, its transients are where the cut-off ends begin.
    const auto reversed = sub::slicing::detectOnsets(channels, 1, frames, kSampleRate, true);
    REQUIRE(!reversed.empty());
    for (size_t i = 1; i < reversed.size(); ++i) CHECK(reversed[i].frame > reversed[i - 1].frame);

    // Beats and regions: equal slices.
    settings.by = sub::slicing::SliceBy::Region;
    settings.regions = 4;
    CHECK_EQ(sub::slicing::sliceStarts(settings, nullptr, 0, 1000, 5000, kSampleRate, starts.data()), 4);
    CHECK((std::vector<int64_t>(starts.begin(), starts.begin() + 4) == std::vector<int64_t>{1000, 2000, 3000, 4000}));
    settings.by = sub::slicing::SliceBy::Beat;
    settings.regionBeats = 2.0;
    settings.divisionBeats = 0.25;
    CHECK_EQ(sub::slicing::sliceStarts(settings, nullptr, 0, 0, 8000, kSampleRate, starts.data()), 8);
    CHECK_EQ(starts[7], int64_t{7000});

    // Snap: the nearest zero crossing.
    const Samples wave = sine(440, 0.5);
    const float* waveChannels[] = {wave.data()};
    const int64_t snapped = sub::slicing::nearestZeroCrossing(waveChannels, 1, 24000, false, 4944, 480);
    CHECK_EQ(snapped, int64_t{4964});
    CHECK_EQ(sub::slicing::nearestZeroCrossing(waveChannels, 1, 24000, false, 0, 480), int64_t{0});
}

TEST_CASE("slice mode: a slice per key from C1, cut by region, beat or transient") {
    const std::vector<double> freqs{220, 330, 440, 550};
    const std::string path = makeWav(tones(freqs, 0.25));  // half a beat each
    // Each key a beat apart: C1, C#1, D1, D#1, then E1 (no fifth slice) and B0 (below C1).
    const std::vector<sub::NoteDesc> notes{{0.0, 0.25, 36, 127}, {1.0, 0.25, 37, 127}, {2.0, 0.25, 38, 127},
                                           {3.0, 0.25, 39, 127}, {4.0, 0.25, 40, 127}, {5.0, 0.25, 35, 127}};
    const std::vector<Values> ways{
        {{"slice_by", 2.f}, {"regions", 4.f}},                       // Region: four
        {{"slice_by", 1.f}, {"warp_beats", 2.f}, {"slice_beat", 1.f}},  // Beat: 1/8s of a 2-beat sample
    };
    for (const Values& way : ways) {
        sub::Engine engine;
        Values values{{"mode", 2.f}};
        values.insert(values.end(), way.begin(), way.end());
        samplerTrack(engine, notes, path, values);
        const Samples out = engine.renderOffline(0.0, 6 * kBeat);
        for (size_t k = 0; k < freqs.size(); ++k) {
            const auto from = static_cast<int64_t>(k) * kBeat;
            INFO("slice " + std::to_string(k));
            CHECK_APPROX_REL(dominantFreq(leftFrom(out, from + 500, from + 11000)), freqs[k], 5e-3);
            CHECK(!anyNonzero(leftFrom(out, from + 12100, from + kBeat)));  // (Trigger: the slice to its end)
        }
        CHECK(!anyNonzero(leftFrom(out, 4 * kBeat)));
    }

    // At its transients: each hit on a key of its own.
    sub::Engine engine;
    const std::string drums = makeWav(hits({0.0, 0.25, 0.5, 0.75}, {0.9, 0.5, 0.8, 0.6}, {200, 300, 400, 500}, 1.0));
    samplerTrack(engine, notes, drums, {{"mode", 2.f}, {"slice_by", 0.f}, {"sensitivity", 100.f}});
    const Samples out = engine.renderOffline(0.0, 4 * kBeat);
    for (size_t k = 0; k < 4; ++k) {
        const auto from = static_cast<int64_t>(k) * kBeat;
        INFO("hit " + std::to_string(k));
        CHECK_APPROX_REL(dominantFreq(leftFrom(out, from, from + 9600)), 200.0 + 100.0 * static_cast<double>(k), 0.03);
    }
}

TEST_CASE("slice playback: mono cuts the slice before, poly lets it ring, thru plays on to the end") {
    const std::string path = makeWav(tones({220, 330, 440, 550}, 0.25));
    const std::vector<sub::NoteDesc> notes{{0.0, 0.1, 36, 127}, {0.25, 0.1, 38, 127}};  // C1, then D1 6000 samples on
    const auto render = [&](float playback) {
        sub::Engine engine;
        samplerTrack(engine, notes, path, {{"mode", 2.f}, {"slice_by", 2.f}, {"regions", 4.f}, {"playback", playback}});
        return engine.renderOffline(0.0, kBeat);
    };
    const Samples mono = render(0.f), poly = render(1.f);
    const Samples late = leftFrom(mono, 7200, 9600);  // (a whole number of cycles of both)
    CHECK(toneLevel(late, 220) < 0.01);
    CHECK_APPROX_REL(toneLevel(late, 440), 0.5, 0.05);
    const Samples both = leftFrom(poly, 7200, 9600);
    CHECK_APPROX_REL(toneLevel(both, 220), 0.5, 0.05);
    CHECK_APPROX_REL(toneLevel(both, 440), 0.5, 0.05);
    // The slice that was cut short faded out, without a click.
    CHECK(largestStep(leftFrom(mono, 5900, 6400)) < 0.08);

    sub::Engine engine;
    samplerTrack(engine, {{0.0, 0.1, 36, 127}}, path, {{"mode", 2.f}, {"slice_by", 2.f}, {"regions", 4.f}, {"playback", 2.f}});
    const Samples on = engine.renderOffline(0.0, 2 * kBeat);
    CHECK_APPROX_REL(dominantFreq(leftFrom(on, 13000, 23000)), 330, 5e-3);  // the next slice, played through
    CHECK_APPROX_REL(dominantFreq(leftFrom(on, 37000, 47000)), 550, 5e-3);
}

TEST_CASE("1-shot: one note at a time, Trigger plays on, Gate fades out after the note, fades in and before the end") {
    const std::string path = makeWav(sine(440, 1.0));
    const std::vector<sub::NoteDesc> shortNote{{0.0, 0.25, 60, 127}};  // 6000 samples
    {
        sub::Engine engine;
        samplerTrack(engine, shortNote, path, {{"mode", 1.f}});
        const Samples out = engine.renderOffline(0.0, 2 * kBeat);
        CHECK(rms(leftFrom(out, 20000, 30000)) > 0.3);  // long after the note-off
        CHECK(!anyNonzero(leftFrom(out, kSampleRate + 10)));  // and it ends with the sample
    }
    {
        sub::Engine engine;
        samplerTrack(engine, shortNote, path, {{"mode", 1.f}, {"trigger", 1.f}, {"fade_out", 10.f}});
        const Samples out = engine.renderOffline(0.0, kBeat);
        CHECK(rms(leftFrom(out, 3000, 5000)) > 0.3);
        CHECK(rms(leftFrom(out, 6000, 6240)) < rms(leftFrom(out, 3000, 5000)));  // fading
        CHECK(!anyNonzero(leftFrom(out, 6000 + 480 + 1)));                     // gone 10 ms after the note
    }
    {
        sub::Engine engine;
        samplerTrack(engine, {{0.0, 4.0, 60, 127}}, path, {{"mode", 1.f}, {"fade_in", 100.f}, {"fade_out", 100.f}});
        const Samples out = engine.renderOffline(0.0, 2 * kBeat);
        const double steady = rms(leftFrom(out, 12000, 24000));
        CHECK_APPROX_REL(rms(leftFrom(out, 2160, 2640)), steady * 0.5, 0.1);         // half way in at 50 ms
        CHECK_APPROX_REL(rms(leftFrom(out, 45360, 45840)), steady * 0.5, 0.1);       // half way out, 50 ms before the end
        CHECK(rms(leftFrom(out, 47760, 48000)) < 0.03 * steady * std::sqrt(2.0) * 2);  // all but out at the end
    }
    {
        // A second note cuts the first: one note at a time, each pitched.
        sub::Engine engine;
        samplerTrack(engine, {{0.0, 1.0, 60, 127}, {0.5, 1.0, 72, 127}}, path, {{"mode", 1.f}});
        const Samples out = engine.renderOffline(0.0, kBeat);
        const Samples late = leftFrom(out, 14400, 19200);
        CHECK(toneLevel(late, 440) < 0.01);
        CHECK_APPROX_REL(toneLevel(late, 880), 0.5, 0.05);
    }
}

TEST_CASE("classic: the loop runs from Loop Start, its end crossfaded") {
    Samples halves = sine(440, 0.5);
    const Samples high = sine(880, 0.5);
    halves.insert(halves.end(), high.begin(), high.end());
    sub::Engine engine;
    samplerTrack(engine, {{0.0, 8.0, 60, 127}}, makeWav(halves), {{"loop", 1.f}, {"loop_start", 50.f}});
    const Samples out = engine.renderOffline(0.0, 4 * kBeat);
    CHECK_APPROX_REL(dominantFreq(leftFrom(out, 1000, 20000)), 440, 2e-3);
    CHECK_APPROX_REL(dominantFreq(leftFrom(out, 30000, 47000)), 880, 2e-3);
    CHECK_APPROX_REL(dominantFreq(leftFrom(out, 50000, 95000)), 880, 2e-3);  // looping the second half

    // A loop that doesn't hold a whole number of cycles jumps where it wraps; its fade smooths it.
    const std::string uneven = makeWav(sine(440.3, 1.0));
    const auto seam = [&](float fade) {
        sub::Engine e;
        samplerTrack(e, {{0.0, 8.0, 60, 127}}, uneven, {{"loop", 1.f}, {"loop_start", 30.f}, {"loop_fade", fade}});
        return largestStep(leftFrom(e.renderOffline(0.0, 4 * kBeat), 1000));
    };
    const double normal = 2 * kPiD * 440.3 / kSampleRate * 0.5;  // the steepest a 440 Hz sine steps
    CHECK(seam(0.f) > 3 * normal);
    CHECK(seam(20.f) < 1.3 * normal);
}

TEST_CASE("reverse plays the sample backwards, its markers places in it played so") {
    Samples sample = sine(440, 0.5);
    sample.resize(static_cast<size_t>(kSampleRate), 0.f);  // half a second of tone, then silence
    const std::string path = makeWav(sample);
    {
        sub::Engine engine;
        samplerTrack(engine, {{0.0, 2.0, 60, 127}}, path, {{"reverse", 1.f}});
        const Samples out = engine.renderOffline(0.0, 2 * kBeat);
        CHECK(rms(leftFrom(out, 1000, 23000)) < 1e-3);  // the silence first
        CHECK(rms(leftFrom(out, 25000, 47000)) > 0.3);
    }
    {
        sub::Engine engine;
        samplerTrack(engine, {{0.0, 2.0, 60, 127}}, path, {{"reverse", 1.f}, {"start", 50.f}});
        const Samples out = engine.renderOffline(0.0, 2 * kBeat);
        CHECK(rms(leftFrom(out, 1000, 23000)) > 0.3);  // from the middle (backwards): the tone at once
        CHECK(!anyNonzero(leftFrom(out, 24100)));
    }
}

TEST_CASE("snap starts the sample at the zero crossing nearest Start") {
    const Samples wave = sine(440, 1.0);
    const std::string path = makeWav(wave);
    const Samples& file = wave;  // (the WAV is 16-bit: compared loosely)
    for (const bool snap : {false, true}) {
        sub::Engine engine;
        samplerTrack(engine, {{0.0, 1.0, 60, 127}}, path,
                     {{"mode", 1.f}, {"start", 10.3f}, {"snap", snap ? 1.f : 0.f}});
        const Samples out = engine.renderOffline(0.0, kBeat / 2);
        const int64_t start = snap ? 4964 : 4944;  // 10.3 % of the second: frame 4944; the nearest crossing 4964
        INFO(snap ? "snapped" : "as set");
        for (int64_t i = 10; i < 1000; i += 37) CHECK_NEAR(at(out, i, 0), file[static_cast<size_t>(start + i)], 2e-4);
    }
}

TEST_CASE("gain and pan") {
    const std::string path = makeWav(sine(440, 1.0, 0.25));
    const auto render = [&](const Values& values) {
        sub::Engine engine;
        samplerTrack(engine, {{0.0, 1.0, 60, 127}}, path, values);
        return engine.renderOffline(0.0, kBeat);
    };
    const Samples plain = render({}), louder = render({{"gain", 6.0206f}});
    CHECK_APPROX_REL(rms(leftFrom(louder, 1000)), 2 * rms(leftFrom(plain, 1000)), 1e-3);
    const Samples left = render({{"pan", -1.f}}), right = render({{"pan", 1.f}});
    CHECK_APPROX_REL(rms(leftFrom(left, 1000)), rms(leftFrom(plain, 1000)), 1e-3);
    CHECK(rms(channel(frames(left, 1000), 1)) < 1e-6);
    CHECK(rms(leftFrom(right, 1000)) < 1e-6);
}

TEST_CASE("the filter: low-, high- and band-pass and notch, 12 or 24 dB an octave") {
    // 200 Hz and 5 kHz together; 0.2 s holds a whole number of cycles of each.
    Samples sample = sine(200, 1.0, 0.3);
    const Samples high = sine(5000, 1.0, 0.3);
    for (size_t i = 0; i < sample.size(); ++i) sample[i] += high[i];
    const std::string path = makeWav(sample);
    const auto levels = [&](const Values& values) {
        sub::Engine engine;
        Values all{{"filter", 1.f}};
        all.insert(all.end(), values.begin(), values.end());
        samplerTrack(engine, {{0.0, 1.0, 60, 127}}, path, all);
        const Samples part = leftFrom(engine.renderOffline(0.0, kBeat), 9600, 19200);
        return std::make_pair(dB(toneLevel(part, 200) / 0.3), dB(toneLevel(part, 5000) / 0.3));
    };
    const auto [openLow, openHigh] = levels({});  // 22 kHz: open
    CHECK(std::abs(openLow) < 0.1);
    CHECK(std::abs(openHigh) < 0.5);
    const auto [lowPass12Low, lowPass12High] = levels({{"filter_freq", 1000.f}, {"filter_slope", 0.f}});
    const auto [lowPass24Low, lowPass24High] = levels({{"filter_freq", 1000.f}});
    CHECK(std::abs(lowPass12Low) < 0.5);
    CHECK(std::abs(lowPass24Low) < 0.5);
    CHECK(lowPass12High < -24.0);
    CHECK(lowPass24High < -48.0);
    const auto [highPassLow, highPassHigh] = levels({{"filter_type", 1.f}, {"filter_freq", 1000.f}});
    CHECK(highPassLow < -48.0);
    CHECK(std::abs(highPassHigh) < 0.5);
    const auto [bandLow, bandHigh] = levels({{"filter_type", 2.f}, {"filter_freq", 5000.f}});
    CHECK(bandLow < -30.0);
    CHECK(std::abs(bandHigh) < 0.5);
    const auto [notchLow, notchHigh] = levels({{"filter_type", 3.f}, {"filter_freq", 5000.f}});
    CHECK(std::abs(notchLow) < 0.5);
    CHECK(notchHigh < -30.0);
    // Resonance: a peak at the cutoff.
    const auto [resonantLow, resonantHigh] = levels({{"filter_freq", 5000.f}, {"filter_res", 80.f}});
    CHECK(resonantHigh > 6.0);
    CHECK(std::abs(resonantLow) < 0.5);
}

TEST_CASE("the LFO: tremolo, vibrato, the filter and the pan; free, synced, restarted by notes") {
    const std::string path = makeWav(sine(440, 4.0));
    const auto render = [&](const Values& values, const std::vector<sub::NoteDesc>& notes = {{0.0, 8.0, 60, 127}}) {
        sub::Engine engine;
        Values all{{"lfo", 1.f}, {"loop", 1.f}};
        all.insert(all.end(), values.begin(), values.end());
        samplerTrack(engine, notes, path, all);
        return engine.renderOffline(0.0, 4 * kBeat);
    };
    // Tremolo: 4 Hz, all the way down and up.
    const Samples tremolo = render({{"lfo_rate", 4.f}, {"lfo_volume", 100.f}});
    const std::vector<double> shape = envelope(leftFrom(tremolo, 0), 480);
    const double loudest = *std::max_element(shape.begin() + 6000, shape.end());
    const double quietest = *std::min_element(shape.begin() + 6000, shape.end());
    CHECK(quietest < 0.05 * loudest);
    std::vector<double> sampled;  // the envelope every 10 ms, for its rate
    for (size_t i = 0; i < shape.size(); i += 480) sampled.push_back(shape[i] - mean(shape));
    CHECK_APPROX_REL(dominantFreq(sampled, 100.0), 4.0, 0.05);
    // Off, the same settings do nothing.
    const Samples off = render({{"lfo", 0.f}, {"lfo_rate", 4.f}, {"lfo_volume", 100.f}});
    CHECK_APPROX_REL(rms(leftFrom(off, 9600, 19200)), rms(leftFrom(off, 28800, 38400)), 1e-3);

    // Vibrato: a square wave of an octave either way, synced to a beat: up for half a beat, down for the other.
    const Values vibrato{{"lfo_wave", 4.f}, {"lfo_sync", 1.f}, {"lfo_beats", 3.f}, {"lfo_pitch", 1200.f}};
    const Samples pitched = render(vibrato);
    CHECK_APPROX_REL(crossingFreq(leftFrom(pitched, 1000, 11000)), 880, 0.01);
    CHECK_APPROX_REL(crossingFreq(leftFrom(pitched, 13000, 23000)), 220, 0.01);
    CHECK_APPROX_REL(crossingFreq(leftFrom(pitched, kBeat + 1000, kBeat + 11000)), 880, 0.01);
    // Synced to the song: a note starting off the beat finds the LFO where the song is...
    const Samples late = render(vibrato, {{0.25, 8.0, 60, 127}});
    CHECK_APPROX_REL(crossingFreq(leftFrom(late, 6000 + 1000, 6000 + 5000)), 880, 0.01);
    CHECK_APPROX_REL(crossingFreq(leftFrom(late, 13000, 23000)), 220, 0.01);
    // ...restarted by each note, from its start.
    Values restarted = vibrato;
    restarted.emplace_back("lfo_retrig", 1.f);
    const Samples again = render(restarted, {{0.25, 8.0, 60, 127}});
    CHECK_APPROX_REL(crossingFreq(leftFrom(again, 6000 + 1000, 6000 + 11000)), 880, 0.01);
    CHECK_APPROX_REL(crossingFreq(leftFrom(again, 6000 + 13000, 6000 + 23000)), 220, 0.01);

    // The pan: left and right take turns, each all but silent while the other is full.
    const Samples panned = render({{"lfo_rate", 2.f}, {"lfo_pan", 100.f}});
    const std::vector<double> l = envelope(leftFrom(panned, 0), 480), r = envelope(channel(panned, 1), 480);
    const auto quietestOf = [](const std::vector<double>& side) {
        return static_cast<size_t>(std::min_element(side.begin() + 4800, side.end() - 4800) - side.begin());
    };
    const double full = *std::max_element(l.begin() + 4800, l.end());
    CHECK(l[quietestOf(l)] < 0.05 * full);
    CHECK(r[quietestOf(l)] > 0.95 * full);
    CHECK(r[quietestOf(r)] < 0.05 * full);
    CHECK(l[quietestOf(r)] > 0.95 * full);

    // The filter: its cutoff swept across the tone.
    const Samples swept = render({{"filter", 1.f}, {"filter_freq", 440.f}, {"lfo_rate", 2.f}, {"lfo_filter", 100.f}});
    const std::vector<double> sweep = envelope(leftFrom(swept, 0), 480);
    CHECK(*std::max_element(sweep.begin() + 6000, sweep.end()) > 4 * *std::min_element(sweep.begin() + 6000, sweep.end()));
}

TEST_CASE("voices: more notes than voices, the oldest fades out; one voice with glide plays legato") {
    const std::string path = makeWav(sine(440, 4.0));
    {
        sub::Engine engine;
        samplerTrack(engine, {{0.0, 4.0, 60, 127}, {0.25, 4.0, 67, 127}, {0.5, 4.0, 72, 127}}, path,
                     {{"voices", 2.f}, {"loop", 1.f}});
        const Samples out = engine.renderOffline(0.0, kBeat);
        const Samples late = leftFrom(out, 14400, 19200);
        CHECK(toneLevel(late, 440) < 0.01);  // the first note went for the third
        CHECK_APPROX_REL(toneLevel(late, 880), 0.5, 0.05);
        CHECK_APPROX_REL(toneLevel(late, 440 * std::pow(2.0, 7 / 12.0)), 0.5, 0.05);
        CHECK(largestStep(leftFrom(out, 11800, 12400)) < 0.12);  // without a click
    }
    {
        // Legato: the second note glides (100 ms) from the first's pitch; letting go of it glides back.
        sub::Engine engine;
        samplerTrack(engine, {{0.0, 3.0, 60, 127}, {1.0, 1.0, 72, 127}}, path,
                     {{"voices", 1.f}, {"glide", 100.f}, {"loop", 1.f}});
        const Samples out = engine.renderOffline(0.0, 3 * kBeat);
        CHECK_APPROX_REL(crossingFreq(leftFrom(out, 10000, 20000)), 440, 0.01);
        CHECK_APPROX_REL(crossingFreq(leftFrom(out, kBeat + 2200, kBeat + 2600)), 440 * std::sqrt(2.0), 0.06);  // half way
        CHECK_APPROX_REL(crossingFreq(leftFrom(out, kBeat + 6000, 2 * kBeat)), 880, 0.01);
        CHECK_APPROX_REL(crossingFreq(leftFrom(out, 2 * kBeat + 6000, 3 * kBeat)), 440, 0.01);
        // Not played again: no attack, the envelope never dips.
        const std::vector<double> shape = envelope(leftFrom(out, 2000, 3 * kBeat), 480);
        CHECK(*std::min_element(shape.begin() + 480, shape.end() - 480) > 0.3);
    }
}

TEST_CASE("warp: the whole sample in its beats at the song's tempo, resampled or stretched") {
    const std::string path = makeWav(sine(440, 0.5));  // a beat at 120
    const auto render = [&](const Values& values, int key = 60, double tempo = 120.0) {
        sub::Engine engine;
        engine.setTempo(tempo);
        Values all{{"mode", 1.f}, {"warp", 1.f}, {"warp_beats", 2.f}};
        all.insert(all.end(), values.begin(), values.end());
        samplerTrack(engine, {{0.0, 0.25, key, 127}}, path, all);
        return engine.renderOffline(0.0, static_cast<int64_t>(3 * kSampleRate * 60.0 / tempo));
    };
    // Re-Pitch: twice as long, an octave down.
    const Samples repitched = render({{"warp_mode", 4.f}});
    CHECK_APPROX_REL(dominantFreq(leftFrom(repitched, 2000, 46000)), 220, 2e-3);
    CHECK(!anyNonzero(leftFrom(repitched, 48010)));
    // Stretched: twice as long, at its pitch; the key transposes it, not its length.
    for (const float mode : {0.f, 1.f, 2.f, 3.f}) {
        INFO("warp mode " + std::to_string(mode));
        const Samples stretched = render({{"warp_mode", mode}});
        CHECK_APPROX_REL(dominantFreq(leftFrom(stretched, 6000, 42000)), 440, 5e-3);
        CHECK(rms(leftFrom(stretched, 40000, 46000)) > 0.2);
        CHECK(rms(leftFrom(stretched, 52000)) < 0.01);
    }
    const Samples higher = render({{"warp_mode", 1.f}}, 72);
    CHECK_APPROX_REL(dominantFreq(leftFrom(higher, 6000, 42000)), 880, 5e-3);
    CHECK(rms(leftFrom(higher, 40000, 46000)) > 0.2);
    // At half the tempo, twice as long again.
    const Samples slower = render({{"warp_mode", 1.f}}, 60, 60.0);
    CHECK_APPROX_REL(dominantFreq(leftFrom(slower, 6000, 90000)), 440, 0.01);  // (stretched 4 times: less exact)
    CHECK(rms(leftFrom(slower, 88000, 94000)) > 0.2);
    // Offline renders come out the same every time.
    CHECK(render({{"warp_mode", 1.f}}) == render({{"warp_mode", 1.f}}));
}

TEST_CASE("the position display follows a reversed note from the sample's end") {
    sub::Engine engine;
    const std::string path = makeWav(sine(440, 1.0));
    const auto [track, device] = samplerTrack(engine, {{0.0, 1.0, 60, 127}}, path, {{"reverse", 1.f}, {"start", 25.f}});
    engine.renderOffline(0.0, kBeat / 2);  // a quarter of a second from 25 % (as it plays backwards)
    std::vector<float> values;
    engine.readProcessorDisplay(device, 0, 0, values);
    REQUIRE(!values.empty());
    CHECK_NEAR(values.back(), 0.5, 0.02);
}
