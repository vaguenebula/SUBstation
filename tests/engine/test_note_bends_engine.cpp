// Notes' bends (MIDI 2.0's per-note pitch bend) in the engine: the rules of a
// bend's curve and vibratos (NoteBend.h), the renderer sending each sounding
// note's bend along its curve to the note's id, MIDI 2.0 input (Ump.h) bending
// the note it names, recorded bends, and the Synth, the Sampler and a VST3
// plug-in (note expression) playing bent notes at their pitch.
//
// The renderer's events are watched with a device of the test's own, which
// logs what it is given, in snapshots made by hand (no audio device needed).

#include <cmath>
#include <map>
#include <memory>

#include "Engine.h"
#include "Ump.h"
#include "builtin/BuiltinProcessor.h"
#include "harness/Fixtures.h"
#include "harness/Signal.h"

using namespace subtest;

namespace {

// A device that keeps every event it is given, with where on the timeline it
// was (the stretch's position plus its offset).
class EventLog final : public sub::Processor {
public:
    struct Seen {
        int64_t time;
        sub::ProcessEvent event;
    };
    std::vector<Seen> seen;

    std::string typeId() const override { return "test:log"; }
    std::string name() const override { return "Event Log"; }
    void prepare(double, int) override {}
    void process(const sub::ProcessContext& ctx, float* const*, int, int) override {
        for (size_t i = 0; i < ctx.inEvents.count; ++i) {
            seen.push_back({ctx.samplePos + ctx.inEvents.events[i].sampleOffset, ctx.inEvents.events[i]});
        }
    }
    const std::vector<sub::ParamInfo>& params() const override {
        static const std::vector<sub::ParamInfo> kNone;
        return kNone;
    }
    float getParam(int) const override { return 0.f; }
    void setParam(int, float) override {}
    bool acceptsMidi() const override { return true; }

    std::vector<Seen> of(sub::ProcessEvent::Type type) const {
        std::vector<Seen> found;
        for (const Seen& s : seen)
            if (s.event.type == type) found.push_back(s);
        return found;
    }
};

// A snapshot of one track whose only device is `device`, playing `notes`.
std::shared_ptr<sub::RenderSnapshot> snapshotOf(const std::shared_ptr<sub::Processor>& device,
                                                std::vector<sub::NoteRender> notes) {
    auto snap = std::make_shared<sub::RenderSnapshot>();
    snap->sampleRate = kSampleRate;
    snap->tempo = 120.0;
    sub::TrackRender track;
    track.id = 1;
    track.params = std::make_shared<sub::TrackParams>();
    track.inserts = {device};
    track.buffers = std::make_shared<sub::TrackBuffers>(sub::Renderer::kMaxBlock);
    track.notes = std::move(notes);
    snap->tracks.push_back(std::move(track));
    return snap;
}

sub::NoteRender noteOf(int64_t start, int64_t end, int key, std::vector<sub::BendPoint> points,
                       std::vector<sub::VibratoSpan> vibrato = {}) {
    sub::NoteRender note;
    note.start = start;
    note.end = end;
    note.key = static_cast<uint8_t>(key);
    note.velocity = 100;
    if (!points.empty() || !vibrato.empty()) {
        auto bend = std::make_shared<sub::NoteBendRender>();
        bend->points = std::move(points);
        bend->vibrato = std::move(vibrato);
        note.bend = bend;
    }
    return note;
}

// Renders a snapshot offline from `from` for `frames`.
void render(sub::Renderer& renderer, const sub::RenderSnapshot& snap, int64_t frames) {
    std::vector<float> out(static_cast<size_t>(frames) * 2);
    renderer.renderOffline(snap, out.data(), frames);
}

void playFrom(sub::Renderer& renderer, int64_t position) {
    renderer.prepare(kSampleRate);
    renderer.setPosition(position);
    renderer.setPlaying(true);
}

// A Synth playing sines (no filtering, full sustain) on a new track: (track, synth).
std::pair<uint32_t, uint32_t> sineSynth(sub::Engine& engine, const std::vector<sub::NoteDesc>& notes) {
    const uint32_t track = engine.addTrack();
    const uint32_t synth = engine.addBuiltinProcessor(engine.trackChain(track), "synth", -1);
    setParam(engine, synth, "wave", 0.f);
    setParam(engine, synth, "cutoff", 20000.f);
    setParam(engine, synth, "sustain", 100.f);
    setParam(engine, synth, "release", 5.f);
    engine.setTrackNotes(track, notes);
    return {track, synth};
}

sub::NoteDesc bentNote(double start, double length, int key, std::vector<sub::BendPoint> bend,
                       std::vector<sub::VibratoSpan> vibrato = {}) {
    sub::NoteDesc note;
    note.startBeat = start;
    note.lengthBeats = length;
    note.key = key;
    note.bend = std::move(bend);
    note.vibrato = std::move(vibrato);
    return note;
}

}  // namespace

// --- The rules (NoteBend.h) -------------------------------------------------------------

TEST_CASE("a bend's curve starts at the note's pitch, goes through its points and holds the last") {
    const std::vector<sub::BendPoint> none;
    CHECK_EQ(sub::bend::curveAt(none, 5.0), 0.0);
    const std::vector<sub::BendPoint> points{{2.0, 4.0, 0.0}, {4.0, -2.0, 0.0}};
    CHECK_EQ(sub::bend::curveAt(points, 0.0), 0.0);  // from its own pitch...
    CHECK_APPROX(sub::bend::curveAt(points, 1.0), 2.0);  // ...to the first point
    CHECK_APPROX(sub::bend::curveAt(points, 2.0), 4.0);
    CHECK_APPROX(sub::bend::curveAt(points, 3.0), 1.0);
    CHECK_APPROX(sub::bend::curveAt(points, 9.0), -2.0);  // held after the last
    // A point at the start sets where it starts; two at one time make a step (the later counts).
    const std::vector<sub::BendPoint> step{{0.0, -1.0, 0.0}, {1.0, -1.0, 0.0}, {1.0, 3.0, 0.0}};
    CHECK_APPROX(sub::bend::curveAt(step, 0.0), -1.0);
    CHECK_APPROX(sub::bend::curveAt(step, 0.999), -1.0);
    CHECK_APPROX(sub::bend::curveAt(step, 1.0), 3.0);
    // A curve bends a segment as an automation breakpoint's does (bulging up when positive).
    const std::vector<sub::BendPoint> curved{{0.0, 0.0, 0.5}, {1.0, 12.0, 0.0}};
    CHECK(sub::bend::curveAt(curved, 0.5) > 6.0);
    CHECK_NEAR(sub::bend::curveAt(curved, 0.5), 12.0 * sub::automationShape(0.5f, 0.5f), 1e-5);
    // Held to the range.
    const std::vector<sub::BendPoint> far{{0.0, 100.0, 0.0}};
    CHECK_EQ(sub::bend::curveAt(far, 0.0), sub::kMaxBendSemitones);
}

TEST_CASE("a vibrato begins and ends on the curve and swings at its rate") {
    const sub::VibratoSpan vibrato{1.0, 2.0, 0.5, 4.0, 0.25};  // from 1 to 3, 4 cycles a unit
    CHECK_EQ(sub::bend::vibratoAt(vibrato, 0.5), 0.0);
    CHECK_EQ(sub::bend::vibratoAt(vibrato, 1.0), 0.0);
    CHECK_EQ(sub::bend::vibratoAt(vibrato, 3.0), 0.0);
    CHECK_EQ(sub::bend::vibratoAt(vibrato, 3.5), 0.0);
    CHECK_NEAR(sub::bend::vibratoAt(vibrato, 2.0 + 1.0 / 16), 0.5, 1e-9);  // a quarter cycle into a swing, swelled in
    CHECK_NEAR(sub::bend::vibratoAt(vibrato, 2.0 + 3.0 / 16), -0.5, 1e-9);
    CHECK_NEAR(sub::bend::vibratoAt(vibrato, 1.0 + 1.0 / 16), 0.5 * sub::bend::smooth(1.0 / 32 / 0.25), 1e-9);  // swelling
    CHECK(std::abs(sub::bend::vibratoAt(vibrato, 2.99)) < 0.01);  // dying away
    // On top of the curve: it swings around it.
    const std::vector<sub::BendPoint> points{{0.0, 2.0, 0.0}};
    const std::vector<sub::VibratoSpan> vibratos{vibrato};
    CHECK_NEAR(sub::bend::at(points, vibratos, 2.0 + 1.0 / 16), 2.5, 1e-9);
    CHECK_NEAR(sub::bend::at(points, vibratos, 0.5), 2.0, 1e-9);
    // A rate in other units: cycles a second, times seconds a unit.
    CHECK_NEAR(sub::bend::vibratoAt(sub::VibratoSpan{1.0, 2.0, 0.5, 2.0, 0.25}, 2.0 + 1.0 / 16, 2.0), 0.5, 1e-9);
}

// --- MIDI 2.0's packets (Ump.h) -----------------------------------------------------------

TEST_CASE("UMP channel voice messages decode as the engine's input") {
    sub::MidiInputEvent event;
    const auto decode = [&event](std::array<uint32_t, 2> words) { return sub::ump::decode(words.data(), 2, event); };
    REQUIRE(decode(sub::ump::noteOn(3, 60, 0xFFFF)));
    CHECK_EQ(event.status, 0x93);
    CHECK_EQ(event.data1, 60);
    CHECK_EQ(event.data2, 127);
    REQUIRE(decode(sub::ump::noteOn(0, 61, 0)));  // velocity 0 is still a note in MIDI 2.0
    CHECK_EQ(event.data2, 1);
    REQUIRE(decode(sub::ump::noteOff(0, 61)));
    CHECK_EQ(event.status, 0x80);
    REQUIRE(decode(sub::ump::controlChange(2, 74, 0x80000000u)));
    CHECK_EQ(event.status, 0xB2);
    CHECK_EQ(event.data1, 74);
    CHECK_EQ(event.data2, 64);
    REQUIRE(decode(sub::ump::perNoteBend(5, 64, 2.0)));
    CHECK(event.kind == sub::MidiInputEvent::Kind::NoteBend);
    CHECK_EQ(event.status & 0x0F, 5);
    CHECK_EQ(event.data1, 64);
    CHECK_NEAR(sub::ump::bendSemitones(event.value), 2.0, 1e-6);
    REQUIRE(decode(sub::ump::perNoteManagement(5, 64, sub::ump::kResetControllers)));  // its bend goes back
    CHECK(event.kind == sub::MidiInputEvent::Kind::NoteBend);
    CHECK_EQ(event.value, sub::ump::kBendCenter);
    CHECK(!decode(sub::ump::perNoteManagement(5, 64, sub::ump::kDetachControllers)));
    // A MIDI 2.0 pitch bend, 32 bits into 14.
    REQUIRE(decode(sub::ump::message(0xE, 0, 0, 0, 0x80000000u)));
    CHECK_EQ(event.status, 0xE0);
    CHECK_EQ(event.data1 | (event.data2 << 7), 8192);
    // MIDI 1.0 in a packet, as its bytes.
    const uint32_t midi1 = 0x20903C40;  // group 0, note on, key 60, velocity 64
    REQUIRE(sub::ump::decode(&midi1, 1, event));
    CHECK(event.kind == sub::MidiInputEvent::Kind::Message);
    CHECK_EQ(event.status, 0x90);
    CHECK_EQ(event.data2, 64);
    // Packets' lengths, by type.
    CHECK_EQ(sub::ump::packetWords(0x20000000u), 1);
    CHECK_EQ(sub::ump::packetWords(0x40000000u), 2);
    CHECK_EQ(sub::ump::packetWords(0xF0000000u), 4);
    // The bend's range, either way.
    CHECK_EQ(sub::ump::bendValue(0.0), sub::ump::kBendCenter);
    CHECK_EQ(sub::ump::bendValue(-48.0), 0u);
    CHECK_EQ(sub::ump::bendValue(100.0), 0xFFFFFFFFu);
}

// --- The renderer sending bends -----------------------------------------------------------

TEST_CASE("a note's events carry its id and its bends follow its curve") {
    auto log = std::make_shared<EventLog>();
    // From sample 1000 to 9000: a glide to +2 over its first 4000 samples, then held.
    auto snap = snapshotOf(log, {noteOf(1000, 9000, 60, {{4000.0, 2.0, 0.0}}), noteOf(2000, 3000, 64, {})});
    sub::Renderer renderer;
    playFrom(renderer, 0);
    render(renderer, *snap, 10000);
    const auto ons = log->of(sub::ProcessEvent::Type::NoteOn);
    const auto offs = log->of(sub::ProcessEvent::Type::NoteOff);
    const auto bends = log->of(sub::ProcessEvent::Type::NoteBend);
    REQUIRE(ons.size() == 2);
    REQUIRE(offs.size() == 2);
    const int32_t id = ons[0].event.noteId;
    CHECK(id >= 0);
    CHECK(ons[1].event.noteId != id);  // every note has its own
    CHECK_EQ(offs[1].event.noteId, id);
    CHECK_EQ(offs[1].time, 9000);
    // Every kBendStep samples along its own grid, where it moves; none after it ends.
    REQUIRE(!bends.empty());
    for (const auto& bend : bends) {
        CHECK_EQ(bend.event.noteId, id);
        CHECK_EQ(bend.event.key(), 60);
        CHECK_EQ((bend.time - 1000) % 32, 0);
        CHECK(bend.time > 1000);
        CHECK(bend.time <= 5000);  // held from there: nothing more to send
        CHECK_NEAR(bend.event.bend, 2.0 * static_cast<double>(bend.time - 1000) / 4000.0, 1e-5);
    }
    CHECK_EQ(bends.back().time, 5000);
    CHECK_APPROX(bends.back().event.bend, 2.0);
    CHECK_EQ(bends.size(), size_t{125});  // 4000 / 32
    // In time order, the bends after the note-on they bend.
    for (size_t i = 1; i < log->seen.size(); ++i) CHECK(log->seen[i - 1].time <= log->seen[i].time);
}

TEST_CASE("a bend that ends on the note's end lands on its last value before the note-off") {
    auto log = std::make_shared<EventLog>();
    const auto beat = static_cast<int64_t>(kSpb);
    // A slide down three semitones into the next note: its last point on its end, the drop curved.
    auto snap = snapshotOf(log, {noteOf(0, 2 * beat + 7, 71, {{1.5 * kSpb, 0.0, 0.6}, {2.0 * kSpb + 7, -3.0, 0.0}}),
                                 noteOf(2 * beat + 7, 4 * beat, 68, {})});
    sub::Renderer renderer;
    playFrom(renderer, 0);
    render(renderer, *snap, 4 * beat);
    const auto bends = log->of(sub::ProcessEvent::Type::NoteBend);
    const auto offs = log->of(sub::ProcessEvent::Type::NoteOff);
    REQUIRE(!bends.empty());
    REQUIRE(!offs.empty());
    CHECK_EQ(bends.back().time, 2 * beat + 7 - 1);  // its last sample: the release holds what it reached
    CHECK_NEAR(bends.back().event.bend, -3.0, 1e-3);
    CHECK(bends.back().time < offs.front().time);
    for (const auto& bend : bends) CHECK_EQ(bend.event.key(), 71);  // (none for the next note)
}

TEST_CASE("a note that starts bent is bent from its note-on") {
    auto log = std::make_shared<EventLog>();
    auto snap = snapshotOf(log, {noteOf(500, 2000, 60, {{0.0, -3.0, 0.0}})});
    sub::Renderer renderer;
    playFrom(renderer, 0);
    render(renderer, *snap, 3000);
    REQUIRE(log->seen.size() == 3);
    CHECK(log->seen[0].event.type == sub::ProcessEvent::Type::NoteOn);
    CHECK(log->seen[1].event.type == sub::ProcessEvent::Type::NoteBend);
    CHECK_EQ(log->seen[1].time, 500);  // with it
    CHECK_APPROX(log->seen[1].event.bend, -3.0);
    CHECK(log->seen[2].event.type == sub::ProcessEvent::Type::NoteOff);
}

TEST_CASE("a note joined as playback starts is bent as far as it got") {
    auto log = std::make_shared<EventLog>();
    auto snap = snapshotOf(log, {noteOf(0, 20000, 60, {{10000.0, 10.0, 0.0}})});
    sub::Renderer renderer;
    renderer.prepare(kSampleRate);
    renderer.setPosition(5000);
    renderer.applyCommand({sub::TransportCommand::Type::Play, 0.0, 0.0});  // chases the notes under way
    render(renderer, *snap, 64);
    REQUIRE(log->seen.size() >= 2);
    CHECK(log->seen[0].event.type == sub::ProcessEvent::Type::NoteOn);
    CHECK_EQ(log->seen[0].time, 5000);
    CHECK(log->seen[1].event.type == sub::ProcessEvent::Type::NoteBend);
    CHECK_APPROX(log->seen[1].event.bend, 5.0);
}

TEST_CASE("an edited bend applies to the note while it sounds") {
    auto log = std::make_shared<EventLog>();
    auto first = snapshotOf(log, {noteOf(0, 20000, 60, {})});
    sub::Renderer renderer;
    playFrom(renderer, 0);
    render(renderer, *first, 2048);
    CHECK(log->of(sub::ProcessEvent::Type::NoteBend).empty());
    // The same note, now bent: the renderer finds it again in the new snapshot.
    auto second = snapshotOf(log, {noteOf(0, 20000, 60, {{0.0, 7.0, 0.0}})});
    render(renderer, *second, 1024);
    const auto bends = log->of(sub::ProcessEvent::Type::NoteBend);
    REQUIRE(bends.size() == 1);
    CHECK_EQ(bends[0].event.noteId, log->of(sub::ProcessEvent::Type::NoteOn)[0].event.noteId);
    CHECK_APPROX(bends[0].event.bend, 7.0);
    CHECK(bends[0].time >= 2048);
}

TEST_CASE("a vibrato on a note is sent as it swings") {
    auto log = std::make_shared<EventLog>();
    // 5 cycles a second over a second of a two-second note, half a semitone either way.
    const double rate = 5.0 / kSampleRate;
    auto snap = snapshotOf(log, {noteOf(0, 2 * kSampleRate, 60, {}, {{0.5 * kSampleRate, 1.0 * kSampleRate, 0.5, rate, 0.0}})});
    sub::Renderer renderer;
    playFrom(renderer, 0);
    render(renderer, *snap, 2 * kSampleRate);
    const auto bends = log->of(sub::ProcessEvent::Type::NoteBend);
    REQUIRE(!bends.empty());
    double low = 0.0, high = 0.0;
    int crossings = 0;
    for (size_t i = 0; i < bends.size(); ++i) {
        CHECK(bends[i].time > kSampleRate / 2);
        CHECK(bends[i].time <= 3 * kSampleRate / 2);
        low = std::min(low, static_cast<double>(bends[i].event.bend));
        high = std::max(high, static_cast<double>(bends[i].event.bend));
        if (i > 0 && (bends[i - 1].event.bend < 0.f) != (bends[i].event.bend < 0.f)) ++crossings;
    }
    CHECK_NEAR(high, 0.5, 0.01);
    CHECK_NEAR(low, -0.5, 0.01);
    CHECK(crossings >= 9);  // twice a cycle
    CHECK(crossings <= 10);
    CHECK_NEAR(bends.back().event.bend, 0.0, 0.01);  // back on the curve
}

// --- MIDI 2.0 input -----------------------------------------------------------------------

TEST_CASE("a MIDI 2.0 per-note bend bends the live note it names") {
    auto log = std::make_shared<EventLog>();
    auto snap = snapshotOf(log, {});
    sub::TrackRender& track = snap->tracks[0];
    track.midiInput.enabled = true;
    track.monitor = sub::MonitorMode::In;
    sub::SharedState shared;
    sub::Renderer renderer;
    renderer.prepare(kSampleRate);
    const auto queue = [&shared](std::array<uint32_t, 2> words, int64_t time) {
        sub::MidiInputEvent event;
        REQUIRE(sub::ump::decode(words.data(), 2, event));
        event.time = time;
        shared.midiInput.push(event);
    };
    queue(sub::ump::noteOn(0, 60, 0x8000), 100);
    queue(sub::ump::noteOn(0, 64, 0x8000), 100);
    queue(sub::ump::perNoteBend(0, 64, -1.5), 300);
    queue(sub::ump::perNoteBend(0, 67, 3.0), 310);  // no such note held: nothing bends
    queue(sub::ump::perNoteManagement(0, 64, sub::ump::kResetControllers), 400);
    queue(sub::ump::noteOff(0, 64), 500);
    std::vector<float> left(512), right(512);
    float* outputs[2] = {left.data(), right.data()};
    sub::AudioIO io;
    io.outputs = outputs;
    io.numOutputs = 2;
    io.frames = 512;
    renderer.processLive(*snap, shared, io);
    const auto ons = log->of(sub::ProcessEvent::Type::NoteOn);
    const auto bends = log->of(sub::ProcessEvent::Type::NoteBend);
    const auto offs = log->of(sub::ProcessEvent::Type::NoteOff);
    REQUIRE(ons.size() == 2);
    REQUIRE(bends.size() == 2);
    REQUIRE(offs.size() == 1);
    CHECK(ons[0].event.noteId != ons[1].event.noteId);
    CHECK_EQ(bends[0].event.noteId, ons[1].event.noteId);
    CHECK_EQ(bends[0].event.key(), 64);
    CHECK_EQ(bends[0].time, 300);
    CHECK_NEAR(bends[0].event.bend, -1.5, 1e-5);
    CHECK_EQ(bends[1].time, 400);
    CHECK_NEAR(bends[1].event.bend, 0.0, 1e-5);
    CHECK_EQ(offs[0].event.noteId, ons[1].event.noteId);
}

TEST_CASE("notes played by hand have ids, and their note-offs the same") {
    auto log = std::make_shared<EventLog>();
    auto snap = snapshotOf(log, {});
    sub::SharedState shared;
    sub::Renderer renderer;
    renderer.prepare(kSampleRate);
    // C3 auditioned, then D3 while it still sounds, then both let go (C3 first).
    for (const auto& [key, velocity] : std::vector<std::pair<int, int>>{{60, 100}, {62, 100}, {60, 0}, {62, 0}}) {
        shared.previewNotes.push({1, static_cast<uint8_t>(key), static_cast<uint8_t>(velocity)});
    }
    std::vector<float> left(512), right(512);
    float* outputs[2] = {left.data(), right.data()};
    sub::AudioIO io;
    io.outputs = outputs;
    io.numOutputs = 2;
    io.frames = 512;
    renderer.processLive(*snap, shared, io);
    const auto ons = log->of(sub::ProcessEvent::Type::NoteOn);
    const auto offs = log->of(sub::ProcessEvent::Type::NoteOff);
    REQUIRE(ons.size() == 2);
    REQUIRE(offs.size() == 2);
    CHECK(ons[0].event.noteId >= 0);
    CHECK(ons[1].event.noteId >= 0);
    CHECK(ons[0].event.noteId != ons[1].event.noteId);
    CHECK_EQ(offs[0].event.noteId, ons[0].event.noteId);
    CHECK_EQ(offs[1].event.noteId, ons[1].event.noteId);
}

TEST_CASE("a recorded note keeps the per-note bends played on it") {
    sub::MidiRecordingTake take(1);
    take.push({1000, 0, 60, 100, false, 0.f});
    take.push({1000, 0, 62, 100, false, 0.f});
    take.push({1200, 0, 60, 0, true, 1.f});
    take.push({1500, 0, 60, 0, true, 2.f});
    take.push({1600, 0, 61, 0, true, 5.f});  // nothing held there: lost
    take.push({2000, 0, 60, 0, false, 0.f});
    take.push({2100, 0, 60, 0, true, 3.f});  // after its note-off: none
    take.collect();
    REQUIRE(take.notes.size() == 2);
    CHECK_EQ(take.notes[0].end, 2000);
    REQUIRE(take.notes[0].bend.size() == 2);
    CHECK_EQ(take.notes[0].bend[0].time, 200);
    CHECK_EQ(take.notes[0].bend[0].semitones, 1.f);
    CHECK_EQ(take.notes[0].bend[1].time, 500);
    CHECK(take.notes[1].bend.empty());
}

// --- Devices playing bent notes ------------------------------------------------------------

TEST_CASE("the synth plays a bent note at its bent pitch") {
    sub::Engine engine;
    // A3 (440 Hz) bent up an octave from its start, and another gliding there over a beat.
    sineSynth(engine, {bentNote(0.0, 2.0, 69, {{0.0, 12.0, 0.0}})});
    const Samples out = channel(engine.renderOffline(0.0, 2 * kBeat), 0);
    CHECK_APPROX_REL(dominantFreq(slice(out, 4800, 4800 + 16384)), 880.0, 2e-3);

    sub::Engine gliding;
    sineSynth(gliding, {bentNote(0.0, 4.0, 69, {{1.0, 0.0, 0.0}, {2.0, -12.0, 0.0}})});
    const Samples glided = channel(gliding.renderOffline(0.0, 4 * kBeat), 0);
    CHECK_APPROX_REL(dominantFreq(slice(glided, 2400, 2400 + 16384)), 440.0, 2e-3);       // flat to beat 1
    CHECK_APPROX_REL(dominantFreq(slice(glided, 2 * kBeat + 2400, 2 * kBeat + 2400 + 16384)), 220.0, 2e-3);  // down an octave by 2
}

TEST_CASE("the synth bends only the note a bend names") {
    sub::Engine engine;
    // A chord of two A3s an octave apart; only the low one bends, up a fifth.
    sineSynth(engine, {bentNote(0.0, 2.0, 57, {{0.0, 7.0, 0.0}}), bentNote(0.0, 2.0, 69, {})});
    const Samples out = channel(engine.renderOffline(0.0, 2 * kBeat), 0);
    const auto bins = spectrum(slice(out, 4800, 4800 + 16384), hanning(16384));
    const auto level = [&bins](double hz) { return bins[static_cast<size_t>(std::lround(hz * 16384 / kSampleRate))]; };
    const double fifth = 220.0 * std::pow(2.0, 7.0 / 12.0);
    CHECK(level(fifth) > 10.0 * level(220.0));
    CHECK(level(440.0) > 10.0 * level(440.0 * std::pow(2.0, 7.0 / 12.0)));
}

TEST_CASE("the sampler plays a bent note at its bent pitch") {
    sub::Engine engine;
    const std::string path = makeWav(sine(440.0, 2.0, 0.5));  // at its root key, C3
    const uint32_t track = engine.addTrack();
    const uint32_t sampler = engine.addBuiltinProcessor(engine.trackChain(track), "sampler", -1);
    // (Its state escapes backslashes: a Windows path goes through the engine's own encoding.)
    engine.setProcessorState(sampler, sub::BuiltinProcessor::encodeState({{"sample", path}}));
    engine.setTrackNotes(track, {bentNote(0.0, 2.0, 60, {{0.0, 12.0, 0.0}}), bentNote(2.0, 2.0, 60, {{0.0, -12.0, 0.0}})});
    const Samples out = channel(engine.renderOffline(0.0, 4 * kBeat), 0);
    CHECK_APPROX_REL(dominantFreq(slice(out, 2400, 2400 + 8192)), 880.0, 3e-3);
    CHECK_APPROX_REL(dominantFreq(slice(out, 2 * kBeat + 2400, 2 * kBeat + 2400 + 8192)), 220.0, 3e-3);
}

TEST_CASE("a plug-in hears a note's bend as its note expression") {
    requireTestPlugins();
    sub::Engine engine;
    const uint32_t track = engine.addTrack();
    addTestPlugin(engine, engine.trackChain(track), "SUB Test Synth");  // a sine per note
    engine.setTrackNotes(track, {bentNote(0.0, 2.0, 69, {{0.0, 12.0, 0.0}})});
    const Samples out = channel(engine.renderOffline(0.0, 2 * kBeat), 0);
    CHECK_APPROX_REL(dominantFreq(slice(out, 4800, 4800 + 16384)), 880.0, 2e-3);
}

TEST_CASE("a note after a bent one plays at its own pitch on a plug-in that reuses voices") {
    requireTestPlugins();
    sub::Engine engine;
    const uint32_t track = engine.addTrack();
    // SUB Test Synth starts a note with the tuning the last note to end left
    // (as a synth reusing that note's voice can): A3 bent up an octave, then
    // E3 straight after it, unbent, must still be E3.
    addTestPlugin(engine, engine.trackChain(track), "SUB Test Synth");
    engine.setTrackNotes(track, {bentNote(0.0, 1.0, 69, {{0.0, 12.0, 0.0}}), bentNote(1.0, 2.0, 64, {})});
    const Samples out = channel(engine.renderOffline(0.0, 3 * kBeat), 0);
    CHECK_APPROX_REL(dominantFreq(slice(out, 2400, 2400 + 8192)), 880.0, 3e-3);
    CHECK_APPROX_REL(dominantFreq(slice(out, kBeat + 4800, kBeat + 4800 + 16384)), 440.0 * std::pow(2.0, -5.0 / 12.0), 2e-3);
}

TEST_CASE("devices that play notes say so") {
    sub::Engine engine;
    const uint32_t track = engine.addTrack();
    const uint32_t chain = engine.trackChain(track);
    CHECK(engine.processorInfo(engine.addBuiltinProcessor(chain, "synth", -1)).acceptsMidi);
    CHECK(engine.processorInfo(engine.addBuiltinProcessor(chain, "sampler", -1)).acceptsMidi);
    CHECK(!engine.processorInfo(engine.addBuiltinProcessor(chain, "utility", -1)).acceptsMidi);
    if (!haveTestPlugins()) return;
    CHECK(engine.processorInfo(addTestPlugin(engine, chain, "SUB Test Note Effect")).acceptsMidi);
    CHECK(!engine.processorInfo(addTestPlugin(engine, chain, "SUB Test Effect")).acceptsMidi);
}

