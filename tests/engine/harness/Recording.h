#pragma once
// What the recording, MIDI input, resampling and live parallel tests share
// (the Python test_recording.py's fixtures): the fake ASIO driver in manual
// mode with float samples (the cable carries floats exactly), an engine
// without clip fades, and reading back what the engine played and recorded.

#include <cmath>

#include "Engine.h"
#include "harness/AsioDriver.h"
#include "harness/Fixtures.h"
#include "harness/Signal.h"

namespace subtest {

inline constexpr int kRate = 48000;
inline constexpr int64_t kRecordSpb = kRate / 2;  // samples per beat at 120 BPM
inline constexpr int kBuffer = 256;
inline constexpr int kRecordFxLatency = 1;  // SUB Test Effect's latency parameter

// The driver (in manual mode, float samples; the test skips without it), then
// an engine, which must have let go of the driver when it is closed.
//
// `onWorkers`: the engine renders on four threads, with silent tracks beside the
// test's (each with a device), so that every buffer's tracks are shared out
// among threads (test_parallel_live.cpp runs tests again so).
struct RecordingTest {
    static constexpr int kThreads = 4;
    static constexpr int kBeside = 7;  // silent tracks with a device: enough work to share at this buffer size

    AsioDriver driver;
    sub::Engine engine;
    explicit RecordingTest(bool onWorkers = false) {
        driver.setManual(true);
        driver.setSampleType(kFloat32Lsb);  // the cable carries floats exactly
        engine.setClipFadeMs(0);
        if (!onWorkers) return;
        engine.setAudioThreads(kThreads);
        for (int i = 0; i < kBeside; ++i) engine.addBuiltinProcessor(engine.trackChain(engine.addTrack()), "utility", -1);
    }
    RecordingTest(const RecordingTest&) = delete;
    RecordingTest& operator=(const RecordingTest&) = delete;
    ~RecordingTest() {
        engine.stopRecording();
        engine.closeDevice();
        CHECK_EQ(driver.get("instances"), 0.0);  // (or the driver was not released)
    }
};

inline std::vector<double> output(AsioDriver& driver, int channel = 0) {
    return decode(driver.output(channel), kFloat32Lsb);
}

// A take's samples, channel by channel, decoded by the engine.
inline std::vector<std::vector<float>> readTake(sub::Engine& engine, const sub::RecordedTake& take) {
    const auto source = engine.loadSource(take.path);
    CHECK_EQ(source->frames(), take.frames);
    CHECK_EQ(static_cast<int>(source->channels()), take.channels);
    CHECK_EQ(source->sampleRate(), static_cast<uint32_t>(kRate));
    std::vector<std::vector<float>> channels;
    for (uint32_t c = 0; c < source->channels(); ++c)
        channels.emplace_back(source->channelData(c), source->channelData(c) + source->frames());
    return channels;
}

// SUB Test Effect on a track (or the master), `latency` samples late (the test skips without the plug-ins).
inline uint32_t latentEffect(sub::Engine& engine, uint32_t track, int latency) {
    const uint32_t effect = addTestPlugin(engine, engine.trackChain(track), "SUB Test Effect");
    engine.setProcessorParam(effect, kRecordFxLatency, static_cast<float>(latency));
    engine.idle();  // the plug-in asked for a restart to change its latency
    CHECK_EQ(engine.processorInfo(effect).latency, latency);
    return effect;
}

}  // namespace subtest
