// MIDI input in the engine, with the fake ASIO driver (tests/asio_driver) in
// manual mode: live notes reaching a track's instrument at the right offset in a
// block, which tracks hear which input, monitoring, notes released when the
// transport stops (or a track stops hearing its input), and recorded notes
// landing on the beats they were played, latency and all. Skipped without the
// ASIO SDK (but for the device list, which needs no driver).
//
// Messages are sent with sendMidiInput(), stamped against the audio clock the
// engine reports after the last buffer, so where each one plays is known to the
// sample. The instrument is SUB Test Synth in its DC mode: each held note adds its
// velocity (/127) to every sample, which shows exactly when it starts and stops.

#include <cmath>
#include <map>
#include <set>

#include "Engine.h"
#include "harness/LiveTests.h"
#include "harness/MidiInputTest.h"

using namespace subtest;

namespace {

bool allAre(const std::vector<double>& values, double value) { return allEqual(values, value); }

// The timeline position of the playhead, in samples.
int64_t playhead(sub::Engine& engine) { return std::llround(engine.positionBeats() * kRecordSpb); }

// Sends `message` as a player would who hears the timeline at `beat` then:
// the output plays a buffer's position `latency` samples after the renderer
// rendered it (the device's output latency, and the delay of the master's
// devices or of delay compensation).
void sendHeard(sub::Engine& engine, double beat, const std::vector<uint8_t>& message, int64_t latency) {
    const sub::AudioClockStatus clock = engine.audioClock();
    REQUIRE(clock.running);
    const int64_t rendered = playhead(engine) - kBuffer;  // the timeline position of the last buffer's first sample
    const int64_t ahead = std::llround(beat * kRecordSpb) - rendered + latency;  // samples after the last buffer began
    REQUIRE(0 <= ahead);
    REQUIRE(ahead < kRate / 2);  // (or send it closer to its time)
    engine.sendMidiInput("Keys", message, clock.hostTimeNs + std::llround(static_cast<double>(ahead) * 1e9 / kRate));
}

void processUntil(AsioDriver& driver, sub::Engine& engine, double beat) {
    while (engine.positionBeats() < beat) driver.process(1);
}

using NoteRow = std::vector<int64_t>;  // start, end (-1: held), key, velocity, channel

std::vector<NoteRow> rows(const std::vector<sub::RecordedNote>& notes) {
    std::vector<NoteRow> out;
    for (const auto& n : notes) out.push_back({n.start, n.end, n.key, n.velocity, n.channel});
    return out;
}

}  // namespace

// --- Playing ------------------------------------------------------------------------

void subtest::live::liveNotesReachTheInstrument(RecordingTest& t) {
    auto& driver = t.driver;
    auto& engine = t.engine;
    keys(engine, driver);
    send(engine, {kNoteOn, 60, 127}, 100);
    send(engine, {kNoteOn, 64, 64}, 150);
    send(engine, {kNoteOff, 60, 0}, 200);
    CHECK_ALLCLOSE(heard(driver), held({{100, 200, 127}, {150, kBuffer, 64}}), 1e-7, 1e-6);
    // Velocity 0 releases too; one sent further ahead waits for its buffer.
    send(engine, {kNoteOn, 64, 0}, kBuffer + 30);
    CHECK_ALLCLOSE(heard(driver, 2), held({{0, kBuffer + 30, 64}}, 2 * kBuffer), 1e-7, 1e-6);
    CHECK(allAre(heard(driver), 0.0));
}

TEST_CASE("live notes reach the instrument at their offsets") {
    RecordingTest t;
    live::liveNotesReachTheInstrument(t);
}

void subtest::live::aNoteReleasedAsItIsPlayedStillSounds(RecordingTest& t) {
    auto& driver = t.driver;
    auto& engine = t.engine;
    keys(engine, driver);
    // Both late (stamped before the last buffer began): the note-off follows the note-on.
    send(engine, {kNoteOn, 60, 127}, -2 * kBuffer);
    send(engine, {kNoteOff, 60, 0}, -2 * kBuffer);
    CHECK_ALLCLOSE(heard(driver), held({{0, 1, 127}}), 1e-7, 1e-6);
    // In the last sample of a buffer: it ends in the next one.
    send(engine, {kNoteOff, 62, 0}, kBuffer - 1);  // (a stray note-off first: ignored)
    send(engine, {kNoteOn, 62, 127}, kBuffer - 1);
    send(engine, {kNoteOff, 62, 0}, kBuffer - 1);
    CHECK_ALLCLOSE(heard(driver, 2), held({{kBuffer - 1, kBuffer, 127}}, 2 * kBuffer), 1e-7, 1e-6);
}

TEST_CASE("a note released as it is played still sounds") {
    RecordingTest t;
    live::aNoteReleasedAsItIsPlayedStillSounds(t);
}

void subtest::live::whichTracksHearWhichInput(RecordingTest& t) {
    auto& driver = t.driver;
    auto& engine = t.engine;
    openAsio(engine, kRate, kBuffer);
    const uint32_t left = engine.addTrack(), right = engine.addTrack();
    for (const auto& [track, pan] : std::vector<std::pair<uint32_t, float>>{{left, -1.f}, {right, 1.f}}) {
        dcSynth(engine, track);
        engine.setTrackPan(track, pan);
        engine.setTrackMonitor(track, sub::MonitorMode::In);
    }
    engine.setTrackMidiInput(left, true, "Keys", 1);  // one input, channel 1
    engine.setTrackMidiInput(right, true, "", 2);  // every input, channel 2
    driver.process(1);

    const auto play = [&](const std::string& device, uint8_t status) {
        send(engine, {status, 60, 127}, 0, device);
        send(engine, {static_cast<uint8_t>(kNoteOff | (status & 0x0F)), 60, 0}, 10, device);
        driver.clearOutput();
        driver.process(1);
        return std::vector<bool>{maxAbs(output(driver, 0)) > 0.5, maxAbs(output(driver, 1)) > 0.5};
    };

    CHECK(play("Keys", 0x90) == (std::vector<bool>{true, false}));
    CHECK(play("Keys", 0x91) == (std::vector<bool>{false, true}));
    CHECK(play("Pads", 0x91) == (std::vector<bool>{false, true}));
    CHECK(play("Pads", 0x90) == (std::vector<bool>{false, false}));
    CHECK(play("Keys", 0x92) == (std::vector<bool>{false, false}));
    engine.setTrackMidiInput(left, true, "", 0);  // every input, every channel
    CHECK(play("Pads", 0x92) == (std::vector<bool>{true, false}));
    engine.setTrackMidiInput(left, false, "", 0);
    CHECK(play("Keys", 0x90) == (std::vector<bool>{false, false}));
    CHECK_THROWS_AS(engine.setTrackMidiInput(left, true, "Keys", 17), std::invalid_argument);
    CHECK_THROWS_AS(engine.setTrackMidiInput(sub::Engine::kMaster, true, "", 0), std::invalid_argument);
    CHECK_THROWS_AS(engine.sendMidiInput("Keys", {60, 100}), std::invalid_argument);  // no status byte
}

TEST_CASE("which tracks hear which input") {
    RecordingTest t;
    live::whichTracksHearWhichInput(t);
}

void subtest::live::midiMonitoring(RecordingTest& t) {
    auto& driver = t.driver;
    auto& engine = t.engine;
    const uint32_t track = keys(engine, driver);
    engine.setTrackNotes(track, {{0.0, 64.0, 72, 127}});  // a clip's note, all along

    const auto levels = [&] {
        send(engine, {kNoteOn, 60, 64}, 0);
        const std::vector<double> out = heard(driver);
        send(engine, {kNoteOff, 60, 0}, 0);
        driver.process(1);
        return std::llround(out.back() * 127);
    };

    engine.setTrackMonitor(track, sub::MonitorMode::Auto);
    CHECK_EQ(levels(), 64);  // armed, stopped
    engine.play();
    driver.process(1);
    CHECK_EQ(levels(), 127 + 64);  // armed: the clip and the input
    engine.setTrackArmed(track, false);
    CHECK_EQ(levels(), 127);  // only the clip
    engine.setTrackMonitor(track, sub::MonitorMode::In);
    CHECK_EQ(levels(), 64);  // only the input
    engine.setTrackMonitor(track, sub::MonitorMode::Off);
    engine.setTrackArmed(track, true);
    engine.stop();
    engine.play();  // the clip's note starts again (it stopped when In took over)
    driver.process(1);
    CHECK_EQ(levels(), 127);  // only the clip
}

TEST_CASE("MIDI monitoring") {
    RecordingTest t;
    live::midiMonitoring(t);
}

void subtest::live::heldKeysAreReleased(RecordingTest& t) {
    auto& driver = t.driver;
    auto& engine = t.engine;
    const uint32_t track = keys(engine, driver);
    send(engine, {kNoteOn, 60, 127}, 0);
    engine.play();
    CHECK(allAre(heard(driver), 1.0));
    engine.stop();  // stopping releases the held keys
    CHECK(allAre(heard(driver), 0.0));
    send(engine, {kNoteOff, 60, 0}, 0);  // its note-off now changes nothing
    send(engine, {kNoteOn, 62, 127}, 10);
    CHECK_ALLCLOSE(heard(driver), held({{10, kBuffer, 127}}), 1e-7, 1e-6);

    engine.setTrackArmed(track, false);  // the track stops hearing its input (Auto)
    CHECK(allAre(heard(driver), 0.0));
    engine.setTrackArmed(track, true);
    send(engine, {kNoteOff, 62, 0}, 0);
    send(engine, {kNoteOn, 64, 127}, 0);
    CHECK(allAre(heard(driver), 1.0));
    engine.setTrackMidiInput(track, true, "Pads", 0);  // nor from this input
    CHECK(allAre(heard(driver), 0.0));

    engine.setTrackMidiInput(track, true, "", 0);
    send(engine, {kNoteOn, 65, 127}, 0);
    driver.process(1);
    engine.removeTrack(track);  // its notes go with it
    const uint32_t other = engine.addTrack();
    dcSynth(engine, other);
    engine.setTrackMidiInput(other, true, "", 0);
    engine.setTrackMonitor(other, sub::MonitorMode::In);
    CHECK(allAre(heard(driver), 0.0));
    send(engine, {kNoteOff, 65, 0}, 0);  // a note-off for a note it never had
    CHECK(allAre(heard(driver), 0.0));
}

TEST_CASE("held keys are released") {
    RecordingTest t;
    live::heldKeysAreReleased(t);
}

TEST_CASE("without a running device MIDI input is dropped") {
    RecordingTest t;
    auto& driver = t.driver;
    auto& engine = t.engine;
    const uint32_t track = engine.addTrack();
    dcSynth(engine, track);
    engine.setTrackMidiInput(track, true, "", 0);
    engine.setTrackMonitor(track, sub::MonitorMode::In);
    CHECK(!engine.audioClock().running);
    engine.sendMidiInput("Keys", {kNoteOn, 60, 127});
    openAsio(engine, kRate, kBuffer);
    CHECK(allAre(heard(driver, 4), 0.0));
    CHECK(engine.audioClock().running);
    engine.closeDevice();
    CHECK(!engine.audioClock().running);
}

TEST_CASE("the MIDI device list") {
    // Whatever is connected to this computer: names, and none open until asked.
    // (No driver needed: this runs everywhere.)
    sub::Engine engine;
    const std::vector<std::string> names = engine.midiInputDevices();
    for (const auto& name : names) CHECK(!name.empty());
    CHECK_EQ(std::set<std::string>(names.begin(), names.end()).size(), names.size());
    CHECK(engine.openMidiInputs().empty());
    CHECK_THROWS_MATCHING(engine.openMidiInput("No Such MIDI Device"), std::runtime_error, "not connected");
    engine.closeMidiInput("No Such MIDI Device");  // not open: nothing to do
}

// --- Recording ----------------------------------------------------------------------

void subtest::live::recordedNotesLandOnTheirBeats(RecordingTest& t, const std::string& latency) {
    auto& driver = t.driver;
    auto& engine = t.engine;
    driver.setLatencies(40, 90);
    openAsio(engine, kRate, kBuffer);
    const uint32_t track = engine.addTrack();
    dcSynth(engine, track);
    engine.setTrackMidiInput(track, true, "", 0);
    engine.setTrackArmed(track, true);
    if (latency == "track") latentEffect(engine, track, 333);  // every track is delayed to line up with it...
    else if (latency == "master") latentEffect(engine, sub::Engine::kMaster, 333);  // ...or comes out of the master this late
    const int64_t lag = std::llround(engine.deviceStatus().latencyMs * kRate / 1000) + (latency != "none" ? 333 : 0);
    CHECK(lag >= kBuffer + 90);

    engine.setPositionBeats(4.0);
    engine.startRecording({{track, ""}});
    CHECK(engine.isRecording());
    CHECK(engine.isPlaying());
    driver.process(1);
    struct Played {
        double start, end;
        uint8_t key, velocity;
    };
    const std::vector<Played> played{{4.5, 5.0, 60, 100}, {5.25, 5.5, 64, 80}, {5.5, 6.0, 67, 127}};
    for (const Played& p : played) {
        processUntil(driver, engine, p.start - 0.5);
        sendHeard(engine, p.start, {kNoteOn, p.key, p.velocity}, lag);
        processUntil(driver, engine, p.end - 0.5);
        sendHeard(engine, p.end, {kNoteOff, p.key, 0}, lag);
    }
    processUntil(driver, engine, 6.25);
    sendHeard(engine, 6.5, {kNoteOn | 3, 48, 90}, lag);  // held when the take ends (channel 4)
    processUntil(driver, engine, 6.75);

    const auto progress = engine.recordingProgress();
    REQUIRE(progress.size() == 1);
    CHECK_EQ(progress[0].trackId, track);
    CHECK(progress[0].midi);
    CHECK(progress[0].started);
    CHECK_EQ(progress[0].startSample, 4 * kRecordSpb);
    REQUIRE(!progress[0].notes.empty());
    CHECK(rows(progress[0].notes).back() == (NoteRow{std::llround(6.5 * kRecordSpb), -1, 48, 90, 3}));

    const auto takes = engine.stopRecording();
    REQUIRE(takes.size() == 1);
    const sub::RecordedTake& take = takes[0];
    CHECK_EQ(take.trackId, track);
    CHECK(take.midi);
    CHECK_EQ(take.path, std::string());
    CHECK_EQ(take.startSample, 4 * kRecordSpb);
    CHECK_EQ(take.error, std::string());
    const int64_t end = playhead(engine);
    CHECK_EQ(take.frames, end - 4 * kRecordSpb);
    std::vector<NoteRow> expected;
    for (const Played& p : played)
        expected.push_back({std::llround(p.start * kRecordSpb), std::llround(p.end * kRecordSpb), p.key, p.velocity, 0});
    expected.push_back({std::llround(6.5 * kRecordSpb), end, 48, 90, 3});
    CHECK(rows(take.notes) == expected);
}

TEST_CASE("recorded notes land on the beats they were played") {
    requireTestAsio();
    for (const std::string latency : {"none", "track", "master"}) {
        INFO("latency: " + latency);
        RecordingTest t;
        live::recordedNotesLandOnTheirBeats(t, latency);
    }
}

void subtest::live::midiAndAudioRecordTogether(RecordingTest& t) {
    auto& driver = t.driver;
    auto& engine = t.engine;
    driver.setInputLevel(0, 0.5);
    openAsio(engine, kRate, kBuffer, {0});
    const uint32_t audio = engine.addTrack(), midi = engine.addTrack();
    engine.setTrackInput(audio, {0});
    engine.setTrackMidiInput(midi, true, "Keys", 1);
    engine.setTrackMonitor(midi, sub::MonitorMode::Off);  // recorded, though not heard
    engine.startRecording({{audio, (tempDir() / "a.wav").string()}, {midi, ""}}, 1.0);
    driver.process(1);
    send(engine, {kNoteOn, 50, 100}, 0);  // during the count-in: not recorded
    driver.process(static_cast<int>(kRecordSpb / kBuffer + 4));
    send(engine, {kNoteOn | 1, 51, 100}, 0);  // another channel: not this track's
    send(engine, {kNoteOn, 52, 100}, 0);
    send(engine, {kNoteOff, 52, 0}, 10);
    driver.process(2);
    std::map<uint32_t, sub::RecordedTake> takes;
    for (const auto& take : engine.stopRecording()) takes[take.trackId] = take;
    REQUIRE(takes.count(audio) == 1 && takes.count(midi) == 1);
    CHECK(takes[audio].frames > 0);
    CHECK(!takes[audio].midi);
    CHECK(takes[audio].notes.empty());
    const sub::RecordedTake& take = takes[midi];
    CHECK(take.midi);
    CHECK_EQ(take.frames, takes[audio].frames);
    REQUIRE(take.notes.size() == 1);
    CHECK_EQ(int{take.notes[0].key}, 52);
    CHECK_EQ(take.notes[0].end - take.notes[0].start, int64_t{10});
}

TEST_CASE("MIDI and audio record together") {
    RecordingTest t;
    live::midiAndAudioRecordTogether(t);
}

TEST_CASE("recording MIDI needs a MIDI input") {
    RecordingTest t;
    auto& engine = t.engine;
    openAsio(engine, kRate, kBuffer);
    const uint32_t track = engine.addTrack();
    CHECK_THROWS_MATCHING(engine.startRecording({{track, ""}}), std::invalid_argument, "no MIDI input");
    engine.setTrackMidiInput(track, true, "", 0);
    CHECK_THROWS_MATCHING(engine.startRecording({{track, ""}, {track, ""}}), std::invalid_argument, "twice");
    engine.startRecording({{track, ""}});
    engine.stop();  // stopped before the playhead moved: an empty take
    const sub::RecordedTake take = engine.stopRecording().at(0);
    CHECK(take.midi);
    CHECK_EQ(take.frames, int64_t{0});
    CHECK(take.notes.empty());
}
