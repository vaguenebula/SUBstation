// Which preset a plug-in has loaded (plugins/PresetName.h, Vst3Processor):
// names found in plug-ins' saved states as real plug-ins keep them (Serum 2's
// JSON, Valhalla's and Nuro Audio's XML, Spitfire's <META>), in a whole
// .vstpreset (component, then controller), placeholders that say nothing; and a
// plug-in's name followed live: restored with its state, picked in its editor.

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "Engine.h"
#include "harness/Fixtures.h"
#include "plugins/PresetName.h"

using namespace subtest;
using sub::vst3::isPlaceholderPresetName;
using sub::vst3::presetNameFromPreset;
using sub::vst3::presetNameFromState;

namespace {

std::string nameIn(const std::string& state) {
    return presetNameFromState(reinterpret_cast<const uint8_t*>(state.data()), state.size());
}

void putLittle(std::string& out, uint64_t value, int bytes) {
    for (int i = 0; i < bytes; ++i) out += static_cast<char>((value >> (8 * i)) & 0xff);
}

uint64_t getLittle(const std::string& in, size_t at) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value |= static_cast<uint64_t>(static_cast<uint8_t>(in[at + i])) << (8 * i);
    return value;
}

// A .vstpreset of these chunks ("Comp", "Cont"), as the VST3 SDK writes one, for a class id.
std::string vstpreset(const std::vector<std::pair<std::string, std::string>>& chunks,
                      const std::string& classId = std::string(32, 'A')) {
    std::string out = "VST3";
    putLittle(out, 1, 4);                  // version
    out += classId;
    putLittle(out, 0, 8);                  // the list's offset, below
    std::string list = "List";
    putLittle(list, chunks.size(), 4);
    for (const auto& [id, data] : chunks) {
        list += id;
        putLittle(list, out.size(), 8);
        putLittle(list, data.size(), 8);
        out += data;
    }
    const uint64_t listAt = out.size();
    for (int i = 0; i < 8; ++i) out[40 + i] = static_cast<char>((listAt >> (8 * i)) & 0xff);
    return out + list;
}

// A chunk of a .vstpreset ("" if it has none).
std::string chunk(const std::string& preset, const std::string& id) {
    const uint64_t list = getLittle(preset, 40);
    const uint32_t count = static_cast<uint32_t>(getLittle(preset, list + 4) & 0xffffffff);
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t entry = list + 8 + i * 20;
        if (preset.compare(entry, 4, id) == 0) return preset.substr(getLittle(preset, entry + 4), getLittle(preset, entry + 12));
    }
    return {};
}

std::string nameInPreset(const std::string& preset) {
    return presetNameFromPreset(reinterpret_cast<const uint8_t*>(preset.data()), preset.size());
}

using EventType = sub::ProcessorEvent::Type;

bool hasEvent(const std::vector<sub::ProcessorEventRecord>& events, EventType type) {
    return std::any_of(events.begin(), events.end(), [&](const auto& e) { return e.type == type; });
}

}  // namespace

TEST_CASE("preset names in plug-ins' states") {
    const std::string binary("\x01\x00\x7f\xff\x10", 5);
    // Serum 2: JSON in its controller's state, after binary.
    CHECK_EQ(nameIn(binary + R"(XferJson{"presetName":"PLUCK - Dynasty","presetAuthor":"x"})"),
             std::string("PLUCK - Dynasty"));
    // Valhalla (JUCE): an attribute.
    CHECK_EQ(nameIn(R"(<ValhallaVintageVerb mix="0.5" presetName="Default"/>)"), std::string("Default"));
    // Spitfire: <META>'s name, not its family's.
    CHECK_EQ(nameIn(R"(<META family="Brass" name="Core - Trumpet" tags="Brass"/>)"), std::string("Core - Trumpet"));
    // Nuro Audio: <PRESET>'s name.
    CHECK_EQ(nameIn(R"(<XVox><PRESET group="Factory Presets" name="Clean" modified="1"/></XVox>)"),
             std::string("Clean"));
    // Trimmed; single quotes; other keys.
    CHECK_EQ(nameIn("programName = '  Warm Room  '"), std::string("Warm Room"));
    CHECK_EQ(nameIn(R"("patchName":"Lead 3")"), std::string("Lead 3"));
}

TEST_CASE("what isn't a preset name") {
    CHECK_EQ(nameIn(""), std::string());
    CHECK_EQ(presetNameFromState(nullptr, 0), std::string());
    CHECK_EQ(nameIn(std::string("\x00\x01\x02 binary only \xff", 18)), std::string());
    CHECK_EQ(nameIn(R"(xpresetName="nope")"), std::string());     // another key ending in one
    CHECK_EQ(nameIn(R"(presetName="")"), std::string());          // empty
    CHECK_EQ(nameIn("presetName=\"bin\x01\x02\""), std::string());  // binary in it
    CHECK_EQ(nameIn(R"(presetName="unclosed)"), std::string());
    // Only the start of a state is read (names sit in its header).
    CHECK_EQ(nameIn(std::string(70 * 1024, ' ') + R"(presetName="Far")"), std::string());
}

TEST_CASE("preset names in a .vstpreset") {
    // Serum 2's name is in its controller's state, after its component's.
    CHECK_EQ(nameInPreset(vstpreset({{"Comp", "binary component state"}, {"Cont", R"({"presetName":"BS - Bass Line"})"}})),
             std::string("BS - Bass Line"));
    // The component's first.
    CHECK_EQ(nameInPreset(vstpreset({{"Cont", R"(presetName="Second")"}, {"Comp", R"(presetName="First")"}})),
             std::string("First"));
    CHECK_EQ(nameInPreset(vstpreset({{"Comp", "nothing"}})), std::string());
    // Bytes that aren't a .vstpreset are read as they are.
    CHECK_EQ(nameInPreset(R"(presetName="Loose")"), std::string("Loose"));
    // A damaged one: with no list where it says, read as bytes; chunks past its end, nothing.
    std::string damaged = vstpreset({{"Comp", R"(presetName="X")"}});
    damaged[40] = 0x7f;
    CHECK_EQ(nameInPreset(damaged), std::string("X"));
    std::string truncated = vstpreset({{"Comp", R"(presetName="Gone")"}});
    truncated.resize(truncated.size() - 6);
    CHECK_EQ(nameInPreset(truncated), std::string());
}

TEST_CASE("placeholder program names") {
    for (const char* name : {"", "  ", "Prog 12", "prog 3", "Program 7", "preset", "Patch_2", "slot #4", "PROG12"}) {
        INFO(name);
        CHECK(isPlaceholderPresetName(name));
    }
    for (const char* name : {"Init", "Grand Piano", "Program Change Lead", "Preset 1 Bass", "Prog Rock Lead"}) {
        INFO(name);
        CHECK(!isPlaceholderPresetName(name));
    }
}

TEST_CASE("a plug-in's preset name follows its state") {
    sub::Engine engine;
    const uint32_t track = engine.addTrack();
    const uint32_t effect = addTestPlugin(engine, engine.trackChain(track), "SUB Test Effect");
    engine.idle();
    CHECK_EQ(engine.processorPresetName(effect), std::string());  // it has none yet
    CHECK(!hasEvent(engine.takeProcessorEvents(), EventType::PresetChanged));

    // A state with a name in it (as a project saved one): the name comes with it.
    const std::vector<uint8_t> plain = engine.processorState(effect);
    const std::string preset(plain.begin(), plain.end());
    const std::string named = vstpreset({{"Comp", chunk(preset, "Comp") + R"(<preset presetName="Warm Room"/>)"}},
                                        preset.substr(8, 32));
    const std::vector<uint8_t> state(named.begin(), named.end());
    engine.setProcessorState(effect, state);
    engine.idle();  // (looked at there, at once)
    CHECK_EQ(engine.processorPresetName(effect), std::string("Warm Room"));
    CHECK(hasEvent(engine.takeProcessorEvents(), EventType::PresetChanged));
    // It keeps it: its own state says so.
    const std::vector<uint8_t> saved = engine.processorState(effect);
    CHECK_EQ(presetNameFromPreset(saved.data(), saved.size()), std::string("Warm Room"));

    // Back to a state without one.
    engine.setProcessorState(effect, plain);
    engine.idle();
    CHECK_EQ(engine.processorPresetName(effect), std::string());
    CHECK(hasEvent(engine.takeProcessorEvents(), EventType::PresetChanged));
}

TEST_CASE("a preset picked in a plug-in's editor") {
#ifndef _WIN32
    SKIP("plug-in editors are Windows only");
#else
    constexpr UINT kPickPreset = WM_USER + 4;  // what the test effect's editor understands
    sub::Engine engine;
    const uint32_t track = engine.addTrack();
    const uint32_t effect = addTestPlugin(engine, engine.trackChain(track), "SUB Test Effect");
    REQUIRE(engine.openEditor(effect, 0, "SUB Test Effect - Presets"));
    const HWND frame = FindWindowW(L"SUBstationPluginEditor", L"SUB Test Effect - Presets");
    const HWND view = frame ? GetWindow(frame, GW_CHILD) : nullptr;
    REQUIRE(frame && view);
    engine.idle();
    engine.takeProcessorEvents();

    // The plug-in says its state changed (setDirty): looked at again, once the
    // last look is a moment ago (not at every knob turned).
    CHECK(SendMessageW(view, kPickPreset, 0, 0) != 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    engine.idle();
    CHECK_EQ(engine.processorPresetName(effect), std::string("Hall"));
    CHECK(hasEvent(engine.takeProcessorEvents(), EventType::PresetChanged));
    engine.idle();
    CHECK(!hasEvent(engine.takeProcessorEvents(), EventType::PresetChanged));  // once
    engine.closeEditor(effect);
#endif
}
