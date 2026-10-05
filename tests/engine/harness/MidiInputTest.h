#pragma once
// What the MIDI input tests (test_midi_input.cpp) and their live parallel
// repeat (test_parallel_live.cpp) share: SUB Test Synth in its DC mode (each
// held note adds its velocity / 127 to every sample, which shows exactly when it
// starts and stops), messages sent so that they play at a known offset into the
// next buffer, and what the driver played.

#include <cmath>

#include "harness/Recording.h"

namespace subtest {

inline constexpr uint8_t kNoteOn = 0x90, kNoteOff = 0x80;

// SUB Test Synth on a track, in its DC mode (the test skips without the plug-ins).
inline uint32_t dcSynth(sub::Engine& engine, uint32_t track) {
    const uint32_t synth = addTestPlugin(engine, engine.trackChain(track), "SUB Test Synth");
    engine.setProcessorParam(synth, engine.processorParamIndex(synth, "1"), 0.f);  // Wave: DC
    return synth;
}

// Sends `message` so that it plays `offset` samples into the next buffer (it
// plays one buffer after it arrives).
inline void send(sub::Engine& engine, const std::vector<uint8_t>& message, int64_t offset,
                 const std::string& device = "Keys") {
    const sub::AudioClockStatus clock = engine.audioClock();
    REQUIRE(clock.running);
    REQUIRE(clock.midiDelay == kBuffer);
    engine.sendMidiInput(device, message, clock.hostTimeNs + std::llround(static_cast<double>(offset) * 1e9 / kRate));
}

// What the next buffers play on a channel.
inline std::vector<double> heard(AsioDriver& driver, int buffers = 1, int channel = 0) {
    driver.clearOutput();
    driver.process(buffers);
    return output(driver, channel);
}

// What the synth plays for notes (start, end, velocity) within one stretch.
struct HeldNote {
    int start, end, velocity;
};
inline std::vector<double> held(const std::vector<HeldNote>& notes, int length = kBuffer) {
    std::vector<double> expected(static_cast<size_t>(length), 0.0);
    for (const HeldNote& n : notes)
        for (int i = n.start; i < n.end; ++i) expected[static_cast<size_t>(i)] += n.velocity / 127.0;
    return expected;
}

// A running device and a track with the synth, hearing every MIDI input (armed).
inline uint32_t keys(sub::Engine& engine, AsioDriver& driver) {
    openAsio(engine, kRate, kBuffer);
    const uint32_t track = engine.addTrack();
    dcSynth(engine, track);
    engine.setTrackMidiInput(track, true, "", 0);
    engine.setTrackArmed(track, true);
    driver.process(2);
    return track;
}

}  // namespace subtest
