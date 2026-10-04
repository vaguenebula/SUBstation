// The built-in Sampler: its sample (its state besides its parameters), playing
// it across the keyboard, and swapping it while it plays. Rendered offline.

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <tuple>

#include "Engine.h"
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
    const sub::BuiltinInfo& info = builtinInfo("sampler");
    CHECK_EQ(info.name, std::string("Sampler"));
    CHECK(info.isInstrument());
    CHECK(paramIds(info.params) == (std::vector<std::string>{"root", "tune", "fine", "start", "end", "loop", "attack",
                                                             "decay", "sustain", "release", "velocity", "volume"}));
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
