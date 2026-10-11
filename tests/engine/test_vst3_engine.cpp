// VST3 hosting in the engine, with the test plug-ins built alongside it
// (tests/vst3_plugins): instruments and effects in the chain, parameters,
// transport, latency compensation, state, and plug-in editors.
//
// Rendered offline, so no audio device is needed. The editor test opens real
// (briefly visible) Win32 windows and talks to the test plug-in's view with
// window messages, as a user's clicks would reach it: Windows only (elsewhere
// plug-ins show no editor).

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <cmath>

#include "Engine.h"
#include "harness/Fixtures.h"
#include "harness/Signal.h"

using namespace subtest;

namespace {

// SUB Test Synth's parameters (then Macros 2..10).
enum { GAIN, WAVE, TEMPO, PLAYING, BEAT, LOOP, MACRO };

using EventType = sub::ProcessorEvent::Type;

struct PluginEngine {
    sub::Engine engine;
    PluginEngine() { engine.setClipFadeMs(0); }
};

// A track with one of the test plug-ins (and notes): (track, processor).
std::pair<uint32_t, uint32_t> pluginTrack(sub::Engine& engine, const std::string& name,
                                          const std::vector<sub::NoteDesc>& notes = {}) {
    const uint32_t track = engine.addTrack();
    const uint32_t processor = addTestPlugin(engine, engine.trackChain(track), name);
    if (!notes.empty()) engine.setTrackNotes(track, notes);
    return {track, processor};
}

bool hasEvent(const std::vector<sub::ProcessorEventRecord>& events, EventType type) {
    return std::any_of(events.begin(), events.end(), [&](const auto& e) { return e.type == type; });
}

std::vector<std::string> paramNames(const std::vector<sub::ParamInfo>& params) {
    std::vector<std::string> names;
    for (const auto& p : params) names.push_back(p.name);
    return names;
}

#ifdef _WIN32
std::vector<sub::ProcessorEventRecord> eventsOf(sub::Engine& engine, EventType type) {
    std::vector<sub::ProcessorEventRecord> found;
    for (const auto& e : engine.takeProcessorEvents())
        if (e.type == type) found.push_back(e);
    return found;
}

constexpr UINT kEditGain = WM_USER + 1, kResize = WM_USER + 2, kDirty = WM_USER + 3;  // what the effect's editor understands

HWND editorWindow(const wchar_t* title) { return FindWindowW(L"SUBstationPluginEditor", title); }

std::pair<int, int> clientSize(HWND hwnd) {
    RECT rect{};
    GetClientRect(hwnd, &rect);
    return {rect.right - rect.left, rect.bottom - rect.top};
}
#endif

}  // namespace

TEST_CASE("scan lists the classes of a module") {
    requireTestPlugins();
    std::map<std::string, sub::PluginDescription> found;
    for (const auto& d : sub::vst3::Vst3Format::instance().scanFile(testPluginsBundle())) found[d.name] = d;
    std::vector<std::string> names;
    for (const auto& [name, d] : found) names.push_back(name);
    CHECK(names == (std::vector<std::string>{"SUB Test Effect", "SUB Test Mono", "SUB Test Note Effect",
                                             "SUB Test Sidechain", "SUB Test Synth"}));  // not the controller class
    const auto& synth = found["SUB Test Synth"];
    CHECK(synth.isInstrument);
    CHECK_EQ(synth.category, std::string("Instrument|Synth"));
    CHECK_EQ(synth.vendor, std::string("SUBstation"));
    CHECK(!found["SUB Test Effect"].isInstrument);
    CHECK_EQ(found["SUB Test Effect"].category, std::string("Fx|Delay"));
    CHECK_EQ(synth.uid.size(), size_t{32});
    CHECK_EQ(synth.format, std::string("VST3"));
    CHECK_EQ(synth.path, testPluginsBundle());
    const std::string missing = (std::filesystem::path(testPluginsBundle()).parent_path() / "Missing.vst3").string();
    CHECK_THROWS_AS(sub::vst3::Vst3Format::instance().scanFile(missing), std::runtime_error);
}

TEST_CASE("an instrument plays its notes sample exactly") {
    PluginEngine e;
    auto& engine = e.engine;
    const auto [track, synth] = pluginTrack(engine, "SUB Test Synth", {{1.0, 2.0, 60, 127}});
    engine.setProcessorParam(synth, WAVE, 0);  // DC: velocity / 127 while the note is held
    const Samples out = engine.renderOffline(0.0, 4 * kBeat);
    CHECK(allEqual(frames(out, 0, kBeat), 0.0));
    CHECK_ALLCLOSE(frames(out, kBeat, 3 * kBeat), 1.0, 1e-7, 0.0);
    CHECK(allEqual(frames(out, 3 * kBeat), 0.0));
}

TEST_CASE("pitch, velocity and chords") {
    PluginEngine e;
    auto& engine = e.engine;
    const auto [track, synth] = pluginTrack(engine, "SUB Test Synth", {{0.0, 2.0, 69, 127}});
    const Samples out = channel(engine.renderOffline(0.0, kBeat), 0);
    const std::vector<double> s = spectrum(out, hanning(out.size()));
    CHECK_NEAR(static_cast<double>(argmax(s)) * kSampleRate / static_cast<double>(out.size()), 440.0, 2.0);  // A3
    engine.setProcessorParam(synth, WAVE, 0);
    engine.setTrackNotes(track, {{0.0, 1.0, 60, 64}, {0.0, 1.0, 64, 64}});
    CHECK_NEAR(at(engine.renderOffline(0.0, 100), 50, 0), 2 * 64 / 127.0, 1e-6);
}

TEST_CASE("parameters as the plug-in describes them") {
    PluginEngine e;
    auto& engine = e.engine;
    const auto [track, synth] = pluginTrack(engine, "SUB Test Synth");
    const auto params = engine.processorParams(synth);
    std::vector<std::string> names{"Gain", "Wave", "Tempo", "Playing", "Beat", "Loop"};
    for (int i = 1; i <= 10; ++i) names.push_back("Macro " + std::to_string(i));
    CHECK(paramNames(params) == names);
    std::vector<std::string> ids;
    for (int i = 0; i < 16; ++i) ids.push_back(std::to_string(i));
    CHECK(paramIds(params) == ids);
    REQUIRE(params.size() == 16);
    const auto& gain = params[GAIN];
    const auto& wave = params[WAVE];
    CHECK_EQ(gain.minValue, 0.f);
    CHECK_EQ(gain.maxValue, 1.f);
    CHECK_EQ(gain.defaultValue, 1.f);
    CHECK_EQ(gain.steps, 0);
    CHECK(gain.automatable && !gain.readOnly && !gain.hidden);
    CHECK(wave.valueLabels == (std::vector<std::string>{"DC", "Sine"}));
    CHECK_EQ(wave.steps, 1);
    CHECK_EQ(wave.maxValue, 1.f);
    std::vector<bool> readOnly;
    for (const auto& p : params) readOnly.push_back(p.readOnly);
    std::vector<bool> expected{false, false, true, true, true, true};
    expected.resize(16, false);
    CHECK(readOnly == expected);
    CHECK_EQ(params[TEMPO].unit, std::string("BPM"));
    CHECK_EQ(engine.processorParamIndex(synth, "1"), WAVE);
    CHECK_EQ(engine.processorParamIndex(synth, "99"), -1);
    // Values are plain: 0..1 when continuous, the step when stepped.
    engine.setProcessorParam(synth, GAIN, 0.25f);
    engine.setProcessorParam(synth, WAVE, 0.f);
    CHECK_APPROX(engine.processorParam(synth, GAIN), 0.25);
    CHECK_EQ(engine.processorParam(synth, WAVE), 0.f);
    CHECK_EQ(engine.processorParamText(synth, WAVE, 1.f), std::string("Sine"));  // the plug-in's own words
    CHECK(engine.processorParamText(synth, GAIN, 0.25f).rfind("0.25", 0) == 0);
    const auto info = engine.processorInfo(synth);
    CHECK_EQ(info.name, std::string("SUB Test Synth"));
    CHECK_EQ(info.typeId, "vst3:" + testPluginUids().at("SUB Test Synth"));
    CHECK_EQ(info.latency, 0);
    CHECK(info.hasEditor);  // it has a controller (its editor is asked for on opening)
}

TEST_CASE("parameter changes reach the processor") {
    PluginEngine e;
    auto& engine = e.engine;
    const auto [track, synth] = pluginTrack(engine, "SUB Test Synth", {{0.0, 4.0, 60, 127}});
    engine.setProcessorParam(synth, WAVE, 0);
    engine.setProcessorParam(synth, GAIN, 0.5f);
    CHECK_APPROX(at(engine.renderOffline(0.0, 100), 50, 0), 0.5);
}

TEST_CASE("transport reaches the plug-in") {
    PluginEngine e;
    auto& engine = e.engine;
    const auto [track, synth] = pluginTrack(engine, "SUB Test Synth");
    engine.setTempo(90.0);
    engine.renderOffline(2.0, 4096);
    engine.idle();  // the plug-in's output parameters reach its controller
    CHECK_APPROX(engine.processorParam(synth, TEMPO), 0.09);
    CHECK(engine.processorParamText(synth, TEMPO, engine.processorParam(synth, TEMPO)).rfind("90", 0) == 0);
    CHECK_EQ(engine.processorParam(synth, PLAYING), 1.f);
    const double beatOfLastBlock = 2.0 + 3 * 1024 / (kSampleRate * 60 / 90.0);
    CHECK_NEAR(engine.processorParam(synth, BEAT) * 1000.0, beatOfLastBlock, 1e-3);
    CHECK_EQ(engine.processorParam(synth, LOOP), 0.f);
    CHECK(hasEvent(engine.takeProcessorEvents(), EventType::ParamsChanged));
}

TEST_CASE("blocks are split where the loop wraps") {
    PluginEngine e;
    auto& engine = e.engine;
    const auto [track, synth] = pluginTrack(engine, "SUB Test Synth");
    engine.setLoop(true, 1.0, 2.0);
    // The 24th block runs from 47552 to 48576 on the timeline, across the loop end
    // (48000): the plug-in sees it as two blocks, the second starting at beat 1.
    engine.renderOffline(1.0, 24 * 1024, true);
    engine.idle();
    CHECK_APPROX(engine.processorParam(synth, BEAT) * 1000.0, 1.0);
    CHECK_EQ(engine.processorParam(synth, LOOP), 1.f);
}

TEST_CASE("offline renders are repeatable (VST3)") {
    PluginEngine e;
    auto& engine = e.engine;
    pluginTrack(engine, "SUB Test Synth", {{0.0, 16.0, 60, 100}});
    const Samples first = engine.renderOffline(0.0, 2 * kBeat);
    engine.renderOffline(0.0, kBeat / 2);  // stops in the middle of the note
    CHECK_EQ(maxAbs(engine.renderOffline(4.0, kBeat)), 0.0);  // no hanging note
    CHECK_ARRAY_EQUAL(engine.renderOffline(0.0, 2 * kBeat), first);
}

TEST_CASE("an effect processes the track") {
    PluginEngine e;
    auto& engine = e.engine;
    const uint32_t track = clipTrack(engine, dcWav());
    const uint32_t effect = addTestPlugin(engine, engine.trackChain(track), "SUB Test Effect");
    const auto params = engine.processorParams(effect);
    CHECK(paramNames(params) == (std::vector<std::string>{"Gain", "Latency", "Bypass"}));
    REQUIRE(params.size() == 3);
    CHECK(params[FX_BYPASS].hidden);  // the device's own on/off switch stands for it
    CHECK_EQ(params[FX_LATENCY].steps, 4096);
    CHECK(params[FX_LATENCY].valueLabels.empty());
    CHECK_ALLCLOSE(engine.renderOffline(0.0, 1000), 0.5, 1e-7, 0.0);  // Gain 0.5 is unity
    engine.setProcessorParam(effect, FX_GAIN, 0.25f);
    CHECK_ALLCLOSE(engine.renderOffline(0.0, 1000), 0.25, 1e-7, 0.0);
    engine.setProcessorEnabled(effect, false);  // switched off: the audio passes by
    CHECK_ALLCLOSE(engine.renderOffline(0.0, 1000), 0.5, 1e-7, 0.0);
}

TEST_CASE("automation reaches the plug-in and its controller") {
    PluginEngine e;
    auto& engine = e.engine;
    const uint32_t track = clipTrack(engine, dcWav());
    const uint32_t effect = addTestPlugin(engine, engine.trackChain(track), "SUB Test Effect");
    engine.takeProcessorEvents();
    engine.setTrackAutomation(track, {{effect, std::to_string(FX_GAIN), {{0.0, 0.25f, 0.f}}}});
    CHECK_ALLCLOSE(engine.renderOffline(0.0, 1000), 0.25, 1e-7, 0.0);  // automated, as if set to 0.25
    engine.idle();  // the automated value reaches the controller (the plug-in's editor) and the UI
    CHECK_APPROX(engine.processorParam(effect, FX_GAIN), 0.25);
    CHECK(hasEvent(engine.takeProcessorEvents(), EventType::ParamsChanged));
    engine.setTrackAutomation(track, {});
    engine.setProcessorParam(effect, FX_GAIN, 0.5f);
    CHECK_ALLCLOSE(engine.renderOffline(0.0, 1000), 0.5, 1e-7, 0.0);
}

TEST_CASE("a mono plug-in on a stereo track") {
    PluginEngine e;
    auto& engine = e.engine;
    const std::string wav = makeWav(interleave({full(kSampleRate, 0.5f), full(kSampleRate, 0.25f)}), 2);
    const uint32_t track = clipTrack(engine, wav);
    const uint32_t mono = addTestPlugin(engine, engine.trackChain(track), "SUB Test Mono");
    CHECK(engine.processorParams(mono).empty());
    CHECK(!engine.processorInfo(mono).hasEditor);  // no controller
    const Samples out = engine.renderOffline(0.0, 1000);
    CHECK_ALLCLOSE(out, 0.5 * (0.5 + 0.25) / 2, 1e-7, 1e-4);  // mixed to mono, halved, on both sides
}

TEST_CASE("latency is compensated") {
    PluginEngine e;
    auto& engine = e.engine;
    const std::string wav = clickWav(0.5);
    const uint32_t late = clipTrack(engine, wav, 1.0);
    const uint32_t direct = clipTrack(engine, wav, 1.0);
    const uint32_t effect = addTestPlugin(engine, engine.trackChain(late), "SUB Test Effect");
    engine.setProcessorParam(effect, FX_LATENCY, 100);
    engine.idle();  // the plug-in asked for a restart to change its latency
    const auto events = engine.takeProcessorEvents();
    CHECK_EQ(std::count_if(events.begin(), events.end(), [](const auto& ev) { return ev.type == EventType::LatencyChanged; }),
             1);
    CHECK_EQ(engine.processorInfo(effect).latency, 100);
    const Samples out = channel(engine.renderOffline(0.0, 2 * kBeat), 0);
    // Both clicks land on beat 1: the other track waits for the late one, and the
    // render starts that much earlier.
    CHECK(nonzero(out) == std::vector<int64_t>{kBeat});
    CHECK_NEAR(out[kBeat], 1.0, 1e-4);

    // So does the metronome.
    engine.setTrackClips(late, {});
    engine.setTrackClips(direct, {});
    const Samples withLatency = engine.renderOffline(0.0, kBeat, false, true);
    engine.setProcessorEnabled(effect, false);
    CHECK_ALLCLOSE(engine.renderOffline(0.0, kBeat, false, true), withLatency, 1e-7, 1e-6);
}

TEST_CASE("state restores a plug-in") {
    PluginEngine e;
    auto& engine = e.engine;
    const auto [track, synth] = pluginTrack(engine, "SUB Test Synth", {{0.0, 1.0, 60, 127}});
    engine.setProcessorParam(synth, GAIN, 0.25f);
    engine.setProcessorParam(synth, WAVE, 0);
    engine.setProcessorParam(synth, MACRO, 0.7f);  // kept by the controller alone
    const std::vector<uint8_t> state = engine.processorState(synth);
    REQUIRE(state.size() >= 4);
    CHECK(std::string(state.begin(), state.begin() + 4) == "VST3");  // a .vstpreset
    const auto [copyTrack, copy] = pluginTrack(engine, "SUB Test Synth", {{0.0, 1.0, 60, 127}});
    engine.setProcessorState(copy, state);
    CHECK_APPROX(engine.processorParam(copy, GAIN), 0.25);  // the controller knows...
    CHECK_EQ(engine.processorParam(copy, WAVE), 0.f);
    CHECK_APPROX(engine.processorParam(copy, MACRO), 0.7);
    CHECK(hasEvent(engine.takeProcessorEvents(), EventType::ParamsChanged));
    CHECK_ALLCLOSE(frames(engine.renderOffline(0.0, 100), 50, 51), 0.25 + 0.25, 1e-7, 0.0);  // both play DC at gain 0.25

    // ...and so does the processor: a single-component plug-in's state too.
    const auto [effectTrack, effect] = pluginTrack(engine, "SUB Test Effect");
    engine.setProcessorParam(effect, FX_GAIN, 0.75f);
    const std::vector<uint8_t> effectState = engine.processorState(effect);
    engine.setProcessorParam(effect, FX_GAIN, 0.1f);
    engine.setProcessorState(effect, effectState);
    CHECK_APPROX(engine.processorParam(effect, FX_GAIN), 0.75);
    CHECK_THROWS_MATCHING(engine.setProcessorState(synth, effectState), std::runtime_error,
                          "not for SUB Test Synth");  // another plug-in's
    const std::string garbage = "garbage";
    CHECK_THROWS_AS(engine.setProcessorState(synth, std::vector<uint8_t>(garbage.begin(), garbage.end())),
                    std::runtime_error);
}

TEST_CASE("load errors") {
    PluginEngine e;
    auto& engine = e.engine;
    const auto& uids = testPluginUids();
    const uint32_t track = engine.addTrack();
    const uint32_t chain = engine.trackChain(track);
    const std::string missing = (std::filesystem::path(testPluginsBundle()).parent_path() / "Missing.vst3").string();
    CHECK_THROWS_AS(engine.addPluginProcessor(chain, "VST3", missing, uids.at("SUB Test Synth"), -1), std::runtime_error);
    CHECK_THROWS_MATCHING(engine.addPluginProcessor(chain, "VST3", testPluginsBundle(), std::string(32, '0'), -1),
                          std::runtime_error, "does not contain");
    CHECK_THROWS_AS(engine.addPluginProcessor(chain, "VST3", testPluginsBundle(), "not a uid", -1), std::invalid_argument);
    CHECK_THROWS_AS(engine.addPluginProcessor(chain, "CLAP", testPluginsBundle(), uids.at("SUB Test Synth"), -1),
                    std::invalid_argument);
    CHECK_THROWS_AS(engine.addPluginProcessor(chain + 99, "VST3", testPluginsBundle(), uids.at("SUB Test Synth"), -1),
                    std::invalid_argument);
}

TEST_CASE("chain order") {
    PluginEngine e;
    auto& engine = e.engine;
    const auto [track, synth] = pluginTrack(engine, "SUB Test Synth", {{0.0, 1.0, 60, 127}});
    engine.setProcessorParam(synth, WAVE, 0);
    const uint32_t effect = addTestPlugin(engine, engine.trackChain(track), "SUB Test Effect");
    engine.setProcessorParam(effect, FX_GAIN, 1.f);  // doubles
    CHECK_ALLCLOSE(engine.renderOffline(0.0, 100), 2.0, 1e-7, 0.0);
    // An instrument writes its output over what comes in: first the effect, then the synth.
    engine.setChainOrder(engine.trackChain(track), {effect, synth});
    CHECK_ALLCLOSE(engine.renderOffline(0.0, 100), 1.0, 1e-7, 0.0);
    for (const std::vector<uint32_t>& bad :
         {std::vector<uint32_t>{effect}, std::vector<uint32_t>{effect, effect}, std::vector<uint32_t>{effect, synth, 999}})
        CHECK_THROWS_AS(engine.setChainOrder(engine.trackChain(track), bad), std::invalid_argument);
    engine.removeProcessor(effect);
    engine.removeTrack(track);
    engine.idle();  // releases the plug-ins, on this thread
}

TEST_CASE("a plug-in moves to another track as it is") {
    PluginEngine e;
    auto& engine = e.engine;
    const std::string wav = clickWav(0.5);
    const uint32_t a = clipTrack(engine, wav, 1.0);
    const uint32_t b = clipTrack(engine, wav, 1.0);
    const uint32_t effect = addTestPlugin(engine, engine.trackChain(a), "SUB Test Effect");
    engine.setProcessorParam(effect, FX_GAIN, 1.f);  // doubles
    engine.setProcessorParam(effect, FX_LATENCY, 100);
    engine.idle();
    const std::vector<uint8_t> state = engine.processorState(effect);

    engine.moveProcessor(effect, engine.trackChain(b), -1);
    CHECK_EQ(engine.processorChain(effect), engine.trackChain(b));
    // The same processor, with its parameters and state: nothing loaded again.
    CHECK_EQ(engine.processorParam(effect, FX_GAIN), 1.f);
    CHECK(engine.processorState(effect) == state);
    engine.setTrackClips(a, {});
    const Samples out = channel(engine.renderOffline(0.0, 2 * kBeat), 0);
    CHECK(nonzero(out) == std::vector<int64_t>{kBeat});  // compensated on its new track
    CHECK_NEAR(out[kBeat], 1.0, 1e-4);

    // Its old track can go without it.
    engine.removeTrack(a);
    engine.idle();
    CHECK_EQ(engine.processorParam(effect, FX_GAIN), 1.f);
    engine.removeTrack(b);
    engine.idle();
    CHECK_THROWS_AS(engine.processorInfo(effect), std::invalid_argument);
}

TEST_CASE("editor edits, resizes and closes") {
#ifndef _WIN32
    SKIP("plug-in editors are Windows only");
#else
    PluginEngine e;
    auto& engine = e.engine;
    const uint32_t track = clipTrack(engine, dcWav());
    const uint32_t effect = addTestPlugin(engine, engine.trackChain(track), "SUB Test Effect");
    REQUIRE(engine.openEditor(effect, 0, "SUB Test Effect - Audio"));
    CHECK(engine.isEditorOpen(effect));
    const HWND frame = editorWindow(L"SUB Test Effect - Audio");
    const HWND view = frame ? GetWindow(frame, GW_CHILD) : nullptr;  // the plug-in's own window
    REQUIRE(frame && view);
    CHECK(clientSize(frame) == std::make_pair(400, 300));
    CHECK(engine.openEditor(effect, 0, "Renamed"));  // already open: raised, retitled
    CHECK(editorWindow(L"Renamed") == frame);

    // A knob drag in the plug-in's editor: one gesture, heard at once, reported for undo.
    CHECK(SendMessageW(view, kEditGain, 0, 0) != 0);
    const auto edits = eventsOf(engine, EventType::ParamEdited);
    REQUIRE(edits.size() == 2);
    CHECK_EQ(edits[0].paramIndex, FX_GAIN);
    CHECK_EQ(std::round(edits[0].value * 1e6) / 1e6, 0.25);
    CHECK_EQ(edits[0].oldValue, 0.5f);
    CHECK_EQ(edits[1].paramIndex, FX_GAIN);
    CHECK_EQ(std::round(edits[1].value * 1e6) / 1e6, 0.3);
    CHECK_EQ(edits[1].oldValue, 0.5f);
    CHECK(edits[0].gesture == edits[1].gesture && edits[1].gesture != 0);
    CHECK_EQ(edits[0].processorId, effect);
    CHECK_APPROX(engine.processorParam(effect, FX_GAIN), 0.3);
    CHECK_ALLCLOSE(engine.renderOffline(0.0, 100), 0.3, 1e-7, 1e-6);  // 0.5 * 2 * 0.3

    // The plug-in resizes its editor; the window follows.
    CHECK(SendMessageW(view, kResize, 500, 350) != 0);
    CHECK(clientSize(frame) == std::make_pair(500, 350));
    CHECK(clientSize(view) == std::make_pair(500, 350));

    // The user drags the window smaller than the plug-in allows (200 x 150): it stops there,
    // and the view follows the window.
    RECT outer{};
    GetWindowRect(frame, &outer);
    const int borderX = outer.right - outer.left - 500, borderY = outer.bottom - outer.top - 350;
    RECT wanted{outer.left, outer.top, outer.left + 50, outer.top + 50};
    SendMessageW(frame, WM_SIZING, WMSZ_BOTTOMRIGHT, reinterpret_cast<LPARAM>(&wanted));  // dragging the bottom right corner
    CHECK_EQ(wanted.right - wanted.left, 200 + borderX);
    CHECK_EQ(wanted.bottom - wanted.top, 150 + borderY);
    SetWindowPos(frame, nullptr, 0, 0, 260 + borderX, 180 + borderY, SWP_NOMOVE | SWP_NOZORDER);
    CHECK(clientSize(view) == std::make_pair(260, 180));

    CHECK(SendMessageW(view, kDirty, 0, 0) != 0);
    engine.idle();
    CHECK(!eventsOf(engine, EventType::StateDirty).empty());

    // Closing the window closes the editor.
    SendMessageW(frame, WM_CLOSE, 0, 0);
    engine.idle();
    CHECK(!engine.isEditorOpen(effect));
    CHECK(!eventsOf(engine, EventType::EditorClosed).empty());
    CHECK(editorWindow(L"Renamed") == nullptr);

    // Removing the device closes its editor too.
    CHECK(engine.openEditor(effect, 0, "Again"));
    engine.removeProcessor(effect);
    CHECK(editorWindow(L"Again") == nullptr);
#endif
}

TEST_CASE("plug-ins without an editor") {
    PluginEngine e;
    auto& engine = e.engine;
    const auto [track, synth] = pluginTrack(engine, "SUB Test Synth");
    CHECK(!engine.openEditor(synth, 0, "x"));
    CHECK(!engine.isEditorOpen(synth));
    const auto [monoTrack, mono] = pluginTrack(engine, "SUB Test Mono");
    CHECK(!engine.openEditor(mono, 0, "x"));
}

TEST_CASE("a latent plug-in on the master") {
    PluginEngine e;
    auto& engine = e.engine;
    const uint32_t track = clipTrack(engine, clickWav(0.5), 1.0);
    const uint32_t effect = addTestPlugin(engine, engine.trackChain(sub::Engine::kMaster), "SUB Test Effect");
    engine.setProcessorParam(effect, FX_LATENCY, 100);
    engine.idle();  // the plug-in asked for a restart to change its latency
    // The click lands on beat 1 still: the render starts that much earlier.
    const Samples out = channel(engine.renderOffline(0.0, 2 * kBeat), 0);
    CHECK(nonzero(out) == std::vector<int64_t>{kBeat});
    CHECK_NEAR(out[kBeat], 0.5, 1e-4);
    const auto target = tempDir() / "mix.wav";
    engine.exportWav(target.string(), 0.0, 2.0, 32);
    const Wav wav = readWav(target);
    CHECK_EQ(wav.format, 3);  // 32-bit float
    CHECK(nonzero(channel(wav.samples(), 0)) == std::vector<int64_t>{kBeat});

    // The metronome is mixed after the master's devices: it waits for them too.
    engine.setTrackClips(track, {});
    const Samples withLatency = engine.renderOffline(0.0, kBeat, false, true);
    CHECK(maxAbs(withLatency) > 0.1);
    engine.setProcessorEnabled(effect, false);
    CHECK_ALLCLOSE(engine.renderOffline(0.0, kBeat, false, true), withLatency, 1e-7, 1e-6);
}

TEST_CASE("master device automation plays in time") {
    // A master device hears the timeline as late as the slowest track, plus the
    // master's devices before it: its automation is as late.
    PluginEngine e;
    auto& engine = e.engine;
    const uint32_t track = clipTrack(engine, makeWav(full(2 * kSampleRate * 2, 0.5f), 2), 0.0, 2.0);
    const uint32_t late = addTestPlugin(engine, engine.trackChain(track), "SUB Test Effect");
    engine.setProcessorParam(late, FX_LATENCY, 100);
    const uint32_t first = addTestPlugin(engine, engine.trackChain(sub::Engine::kMaster), "SUB Test Effect");
    engine.setProcessorParam(first, FX_LATENCY, 50);
    engine.idle();
    const uint32_t utility = engine.addBuiltinProcessor(engine.trackChain(sub::Engine::kMaster), "utility", -1);
    const sub::ParamInfo gain = paramInfo(engine, utility, "gain");
    const float quiet = gain.toNormalized(-60.f), loud = gain.toNormalized(0.f);
    const int64_t step = kBeat + 400;  // not on a block boundary of the render (which starts 150 samples early)
    const double stepBeat = static_cast<double>(step) / kBeat;
    engine.setTrackAutomation(sub::Engine::kMaster,
                              {{utility, "gain", {{0.0, quiet, 0.f}, {stepBeat, quiet, 0.f}, {stepBeat, loud, 0.f}}}});
    const Samples out = channel(engine.renderOffline(0.0, step + 2000), 0);
    CHECK(maxAbs(slice(out, step - 300, step)) < 0.001);  // still at -60 dB right up to the step
    CHECK(out[step + 40] > 0.01);  // and rising from it at once (smoothed)
}
