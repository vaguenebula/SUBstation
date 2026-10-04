// ASIO, with the fake driver built alongside the engine (tests/asio_driver):
// finding and opening drivers, sample rates and buffer sizes, channels, every
// sample format, inputs, the driver's requests (resets, new latencies, another
// clock), its control panel and errors. Skipped without the ASIO SDK.
//
// The driver runs in manual mode unless a test says otherwise: each
// `driver.process(n)` is n buffer switches, made on the test's thread, so what
// the engine plays can be checked sample by sample. Its outputs are read back as
// the bytes the engine wrote, and decoded here.

#include <chrono>
#include <cmath>
#include <thread>

#include "Engine.h"
#include "harness/AsioDriver.h"
#include "harness/Fixtures.h"
#include "harness/Signal.h"

using namespace subtest;

namespace {

// The driver (the test skips without it), then an engine, which must have let
// go of the driver when it is closed.
struct AsioTest {
    AsioDriver driver;
    sub::Engine engine;
    ~AsioTest() {
        engine.closeDevice();
        CHECK_EQ(driver.get("instances"), 0.0);  // (or the driver was not released)
    }
};

bool waitUntil(const std::function<bool()>& predicate, double timeout = 5.0) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

std::vector<double> range(int count, double scale) {
    std::vector<double> values(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) values[static_cast<size_t>(i)] = i / scale;
    return values;
}

}  // namespace

TEST_CASE("lists the driver") {
    AsioTest t;
    CHECK(sub::Engine::driverTypes() == (std::vector<std::string>{"WASAPI", "ASIO"}));
    std::vector<std::string> names;
    for (const auto& d : t.engine.devices("ASIO")) names.push_back(d.name);
    CHECK(names == std::vector<std::string>{kTestAsioName});
}

TEST_CASE("status and capabilities") {
    AsioTest t;
    t.driver.setManual(true);
    sub::DeviceConfig config = asioConfig(48000, 128);
    config.window = 0x1234;
    t.engine.openDevice(config);
    const sub::DeviceStatus status = t.engine.deviceStatus();
    CHECK(status.open);
    CHECK_EQ(status.backend, std::string("ASIO"));
    CHECK_EQ(status.name, std::string(kTestAsioName));
    CHECK_EQ(status.sampleRate, 48000u);
    CHECK_EQ(status.bufferFrames, 128u);
    CHECK_EQ(t.engine.sampleRate(), 48000.0);
    CHECK_APPROX(status.latencyMs, (128 + 64) / 48.0);  // the driver's figures
    CHECK_APPROX(status.inputLatencyMs, (128 + 32) / 48.0);
    CHECK(status.inputChannels.empty());
    CHECK(status.outputChannels == (std::vector<int>{0, 1}));
    const sub::DeviceCaps caps = t.engine.deviceCapabilities();
    CHECK(caps.inputNames == (std::vector<std::string>{"Test In 1", "Test In 2", "Test In 3", "Test In 4"}));
    CHECK(caps.outputNames ==
          (std::vector<std::string>{"Test Out 1", "Test Out 2", "Test Out 3", "Test Out 4", "Test Out 5", "Test Out 6"}));
    CHECK(caps.sampleRates == (std::vector<uint32_t>{44100, 48000, 96000}));
    CHECK(caps.bufferSizes == (std::vector<uint32_t>{32, 64, 128, 256, 512, 1024, 2048}));
    CHECK_EQ(caps.preferredBufferFrames, 256u);
    CHECK(caps.hasControlPanel);
    CHECK_EQ(t.driver.initHandle(), uintptr_t{0x1234});  // the window its dialogs belong to
    CHECK_EQ(t.driver.get("time_info"), 1.0);  // the engine takes bufferSwitchTimeInfo
    CHECK_EQ(t.driver.get("inputs"), 0.0);  // only the open channels
    CHECK_EQ(t.driver.get("outputs"), 2.0);
}

TEST_CASE("output in every sample format") {
    requireTestAsio();
    for (const long type : asioSampleTypes()) {
        INFO("sample type " + std::to_string(type));
        AsioTest t;
        auto& engine = t.engine;
        t.driver.setManual(true);
        t.driver.setSampleType(type);
        const uint32_t track = engine.addTrack();
        const std::string ramp = rampWav();
        engine.loadSource(ramp);
        engine.setClipFadeMs(0);
        engine.setTrackClips(track, {clip(ramp, 0.0, 1.0)});
        openAsio(engine, kSampleRate, 256, {}, {2, 3});
        engine.play();
        CHECK_EQ(t.driver.process(8), 8);
        const std::vector<double> expected = range(8 * 256, 32768.0);  // both halves of the double buffer, in order
        const int bits = asioFormat(type).bits;
        const double tolerance = bits ? 1.5 / static_cast<double>(int64_t{1} << (bits - 1)) : 1e-7;
        for (const int c : {2, 3}) CHECK_ALLCLOSE(decode(t.driver.output(c), type), expected, 1e-7, tolerance);
        for (const int c : {0, 1, 4, 5}) CHECK(t.driver.output(c).empty());  // never opened
        CHECK_APPROX(engine.positionBeats() * kBeat, 8 * 256);
        CHECK_EQ(t.driver.get("output_ready"), 1.0 + 8);  // asked once whether it takes it, then after each buffer
    }
}

TEST_CASE("full scale is clipped") {
    AsioTest t;
    auto& engine = t.engine;
    t.driver.setManual(true);
    const std::string loud = makeWav(full(kSampleRate, 0.9f));
    const uint32_t track = engine.addTrack();
    engine.loadSource(loud);
    engine.setClipFadeMs(0);
    engine.setTrackClips(track, {clip(loud, 0.0, 1.0)});
    engine.setTrackGain(track, 4.f);  // 3.6: over full scale
    openAsio(engine, kSampleRate, 64);
    engine.play();
    t.driver.process(4);
    const std::vector<double> values = decode(t.driver.output(0), kInt32Lsb);
    CHECK_EQ(maxOf(values) * 2147483648.0, 2147483647.0);  // not wrapped round to negative
}

TEST_CASE("a mono output mixes the master") {
    AsioTest t;
    auto& engine = t.engine;
    t.driver.setManual(true);
    const std::string left = makeWav(interleave({full(kSampleRate, 0.5f), full(kSampleRate, 0.f)}), 2);
    const uint32_t track = engine.addTrack();
    engine.loadSource(left);
    engine.setClipFadeMs(0);
    engine.setTrackClips(track, {clip(left, 0.0, 1.0)});
    openAsio(engine, kSampleRate, 64, {}, {5});
    engine.play();
    t.driver.process(2);
    CHECK(engine.deviceStatus().outputChannels == std::vector<int>{5});
    CHECK_ALLCLOSE(decode(t.driver.output(5), kInt32Lsb), 0.25, 1e-7, 1e-6);
}

TEST_CASE("open inputs are metered") {
    requireTestAsio();
    for (const long type : {kInt16Lsb, kInt24Lsb, kInt32Lsb, kFloat32Lsb, kFloat64Lsb, kInt32Lsb24, kInt32Msb}) {
        INFO("sample type " + std::to_string(type));
        AsioTest t;
        auto& engine = t.engine;
        t.driver.setManual(true);
        t.driver.setSampleType(type);
        t.driver.setInputLevel(1, -0.25);
        t.driver.setInputLevel(3, 0.75);
        openAsio(engine, 0, 0, {3, 1});
        CHECK(engine.deviceStatus().inputChannels == (std::vector<int>{3, 1}));
        CHECK_EQ(t.driver.get("inputs"), 2.0);
        t.driver.process(2);
        CHECK_ALLCLOSE(engine.takeInputMeters(), (std::vector<double>{0.75, 0.25}), 1e-7, 1e-4);
        CHECK(engine.takeInputMeters() == (std::vector<float>{0.f, 0.f}));  // since the last call
    }
}

TEST_CASE("sample rates") {
    AsioTest t;
    auto& engine = t.engine;
    t.driver.setManual(true);
    openAsio(engine);  // the rate the driver runs at
    CHECK_EQ(engine.deviceStatus().sampleRate, 44100u);
    CHECK_EQ(engine.sampleRate(), 44100.0);
    openAsio(engine, 96000);
    CHECK_EQ(engine.deviceStatus().sampleRate, 96000u);
    CHECK_EQ(t.driver.get("rate"), 96000.0);
    CHECK_THROWS_MATCHING(openAsio(engine, 22050), std::runtime_error, "SUB Test ASIO can't run at 22050 Hz");
    CHECK(!engine.deviceStatus().open);
}

TEST_CASE("buffer sizes") {
    AsioTest t;
    auto& engine = t.engine;
    t.driver.setManual(true);
    for (const auto& [asked, used] : std::vector<std::pair<uint32_t, uint32_t>>{{0, 256}, {100, 128}, {128, 128}, {3000, 2048}, {10, 32}}) {
        INFO("asked for " + std::to_string(asked));
        openAsio(engine, 0, asked);
        CHECK_EQ(engine.deviceStatus().bufferFrames, used);
        CHECK_EQ(t.driver.get("buffer_size"), static_cast<double>(used));
    }
    // A driver whose size is set in its control panel only.
    t.driver.setBufferSizes(512, 512, 512, 0);
    openAsio(engine, 0, 128);
    CHECK_EQ(engine.deviceStatus().bufferFrames, 512u);
    CHECK(engine.deviceCapabilities().bufferSizes == std::vector<uint32_t>{512});
    // Sizes in steps of 48.
    t.driver.setBufferSizes(48, 480, 96, 48);
    openAsio(engine, 0, 200);
    CHECK_EQ(engine.deviceStatus().bufferFrames, 192u);  // the nearest it takes
    CHECK(engine.deviceCapabilities().bufferSizes == (std::vector<uint32_t>{48, 96, 192, 384, 480}));  // the usual ones among them
}

TEST_CASE("a reset request reopens with the same settings") {
    AsioTest t;
    auto& engine = t.engine;
    t.driver.setManual(true);
    openAsio(engine, 48000, 128, {}, {4, 5});
    CHECK_EQ(engine.takeDeviceEvent(), std::string());
    CHECK_EQ(t.driver.sendMessage(kResetRequest, 0), 1);
    CHECK_EQ(engine.takeDeviceEvent(), std::string("reset"));
    CHECK_EQ(engine.takeDeviceEvent(), std::string());
    engine.reopenDevice();
    const sub::DeviceStatus status = engine.deviceStatus();
    CHECK_EQ(status.sampleRate, 48000u);
    CHECK_EQ(status.bufferFrames, 128u);
    CHECK(status.outputChannels == (std::vector<int>{4, 5}));
    CHECK_EQ(t.driver.get("inits"), 2.0);
}

TEST_CASE("a buffer size change request") {
    AsioTest t;
    auto& engine = t.engine;
    t.driver.setManual(true);
    openAsio(engine, 0, 128);
    CHECK_EQ(t.driver.sendMessage(kBufferSizeChange, 512), 1);
    CHECK_EQ(engine.takeDeviceEvent(), std::string("reset"));
    engine.reopenDevice();
    CHECK_EQ(engine.deviceStatus().bufferFrames, 512u);
}

TEST_CASE("the driver changes its clock") {
    AsioTest t;
    auto& engine = t.engine;
    t.driver.setManual(true);
    openAsio(engine, 44100);
    t.driver.changeSampleRate(96000);  // the hardware follows an external clock
    CHECK_EQ(engine.takeDeviceEvent(), std::string("reset"));
    engine.reopenDevice();
    CHECK_EQ(engine.deviceStatus().sampleRate, 96000u);
    CHECK_EQ(engine.sampleRate(), 96000.0);
}

TEST_CASE("new latencies") {
    AsioTest t;
    auto& engine = t.engine;
    t.driver.setManual(true);
    openAsio(engine, 48000, 128);
    t.driver.setLatencies(16, 480);
    CHECK_EQ(t.driver.sendMessage(kLatenciesChanged, 0), 1);
    CHECK_EQ(engine.takeDeviceEvent(), std::string("latency"));
    CHECK_APPROX(engine.deviceStatus().latencyMs, (128 + 480) / 48.0);
    CHECK_APPROX(engine.deviceStatus().inputLatencyMs, (128 + 16) / 48.0);
}

TEST_CASE("questions drivers ask") {
    AsioTest t;
    auto& engine = t.engine;
    openAsio(engine);
    CHECK_EQ(t.driver.sendMessage(kEngineVersion, 0), 2);
    CHECK_EQ(t.driver.sendMessage(kSupportsTimeInfo, 0), 1);
    for (const long selector : {kResetRequest, kBufferSizeChange, kResyncRequest, kLatenciesChanged, kSupportsTimeInfo}) {
        INFO("selector " + std::to_string(selector));
        CHECK_EQ(t.driver.sendMessage(kSelectorSupported, selector), 1);
    }
    CHECK_EQ(t.driver.sendMessage(kSelectorSupported, kOverload), 0);
    CHECK_EQ(t.driver.sendMessage(kResyncRequest, 0), 1);  // nothing to redo
    CHECK_EQ(engine.takeDeviceEvent(), std::string());
}

TEST_CASE("the control panel") {
    AsioTest t;
    auto& engine = t.engine;
    t.driver.setManual(true);
    CHECK(!engine.showDeviceControlPanel());  // no device
    openAsio(engine, 0, 128);
    CHECK(engine.showDeviceControlPanel());
    CHECK_EQ(t.driver.get("control_panels"), 1.0);
    CHECK_EQ(engine.takeDeviceEvent(), std::string());  // nothing changed
    // The user sets another buffer size in it; the driver doesn't ask for a reset.
    t.driver.setControlPanelChange(1024, 0);
    CHECK(engine.showDeviceControlPanel());
    CHECK_EQ(engine.takeDeviceEvent(), std::string("reset"));
    engine.reopenDevice();
    CHECK_EQ(engine.deviceStatus().bufferFrames, 1024u);
}

TEST_CASE("errors leave no driver open") {
    AsioTest t;
    auto& engine = t.engine;
    t.driver.failInit("the interface is unplugged");
    CHECK_THROWS_MATCHING(openAsio(engine), std::runtime_error,
                          "^SUB Test ASIO could not be started: the interface is unplugged$");
    t.driver.failInit(nullptr);
    CHECK(!engine.deviceStatus().open);
    CHECK_EQ(t.driver.get("instances"), 0.0);
    sub::DeviceConfig nope;
    nope.driver = "ASIO";
    nope.name = "Nope";
    CHECK_THROWS_MATCHING(engine.openDevice(nope), std::runtime_error, "^ASIO driver not found: Nope$");
    CHECK_THROWS_MATCHING(openAsio(engine, 0, 0, {}, {6}), std::runtime_error, "^SUB Test ASIO has no output 7$");
    CHECK_THROWS_MATCHING(openAsio(engine, 0, 0, {4}), std::runtime_error, "^SUB Test ASIO has no input 5$");
    CHECK_THROWS_MATCHING(openAsio(engine, 0, 0, {}, {1, 1}), std::runtime_error, "listed twice");
    sub::DeviceConfig coreAudio;
    coreAudio.driver = "CoreAudio";
    CHECK_THROWS_MATCHING(engine.openDevice(coreAudio), std::runtime_error, "Unknown driver type: CoreAudio");
    CHECK_EQ(t.driver.get("instances"), 0.0);
    CHECK_THROWS_MATCHING(engine.reopenDevice(), std::runtime_error, "No audio device has been opened");
}

TEST_CASE("one ASIO device per program") {
    AsioTest t;
    t.driver.setManual(true);
    openAsio(t.engine);
    sub::Engine other;
    CHECK_THROWS_MATCHING(openAsio(other), std::runtime_error, "Another ASIO device is open");
    t.engine.closeDevice();
    openAsio(other);  // now it can
    other.closeDevice();
}

TEST_CASE("it plays on the driver's thread") {
    AsioTest t;
    auto& engine = t.engine;
    const uint32_t track = engine.addTrack();
    const std::string dc = dcWav();
    engine.loadSource(dc);
    engine.setTrackClips(track, {clip(dc, 0.0, 1.0)});
    for (int i = 0; i < 3; ++i) {  // open and close while the driver's thread runs
        INFO("time " + std::to_string(i + 1));
        openAsio(engine, kSampleRate, 64);
        engine.play();
        CHECK(waitUntil([&] { return engine.positionBeats() * kBeat > 20 * 64; }));
        engine.stop();
        engine.setPositionBeats(0.0);
        engine.closeDevice();
        CHECK_EQ(t.driver.get("running"), 0.0);
    }
    CHECK_EQ(t.driver.get("starts"), 3.0);
    const std::vector<double> samples = decode(t.driver.output(0), kInt32Lsb);
    CHECK(std::any_of(samples.begin(), samples.end(), [](double x) { return std::fabs(x - 0.5) <= 1e-6 + 1e-5 * 0.5; }));
}
