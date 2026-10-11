// A device's MIDI input from another track (Engine::setProcessorMidiInput()):
// an instrument, or an effect with a MIDI input (SUB Test Note Effect, as a
// vocoder or a pitch corrector), hearing another track's notes instead of its
// own track's; in racks, after moves, from frozen tracks, on any number of
// threads; and what is refused. Rendered offline.

#include <cmath>
#include <stdexcept>

#include "Engine.h"
#include "harness/Fixtures.h"
#include "harness/Signal.h"

using namespace subtest;

namespace {

sub::NoteDesc note(double start, double length, int key, int velocity = 100) {
    sub::NoteDesc n;
    n.startBeat = start;
    n.lengthBeats = length;
    n.key = key;
    n.velocity = velocity;
    return n;
}

// A Synth playing sines (no filtering, full sustain) in a chain.
uint32_t sineSynthOn(sub::Engine& engine, uint32_t chain) {
    const uint32_t synth = engine.addBuiltinProcessor(chain, "synth", -1);
    setParam(engine, synth, "wave", 0.f);
    setParam(engine, synth, "cutoff", 20000.f);
    setParam(engine, synth, "sustain", 100.f);
    setParam(engine, synth, "release", 5.f);
    return synth;
}

// A MIDI track playing `notes`, with no instrument of its own.
uint32_t notesTrack(sub::Engine& engine, const std::vector<sub::NoteDesc>& notes) {
    const uint32_t track = engine.addTrack();
    engine.setTrackNotes(track, notes);
    return track;
}

double pitchAt(const Samples& left, int64_t from) { return dominantFreq(slice(left, from, from + 16384)); }

}  // namespace

TEST_CASE("an instrument hears another track's notes instead of its own track's") {
    sub::Engine engine;
    const uint32_t keys = notesTrack(engine, {note(0.0, 2.0, 69)});  // A3, 440 Hz
    const uint32_t lead = engine.addTrack();
    const uint32_t synth = sineSynthOn(engine, engine.trackChain(lead));
    engine.setTrackNotes(lead, {note(0.0, 2.0, 81)});  // its own: A4, 880 Hz
    CHECK_APPROX_REL(pitchAt(channel(engine.renderOffline(0.0, 2 * kBeat), 0), 4800), 880.0, 2e-3);
    engine.setProcessorMidiInput(synth, keys);
    CHECK_EQ(engine.processorMidiInput(synth), keys);
    CHECK_APPROX_REL(pitchAt(channel(engine.renderOffline(0.0, 2 * kBeat), 0), 4800), 440.0, 2e-3);
    // Bends and all: the source's notes as it plays them.
    sub::NoteDesc bent = note(0.0, 2.0, 69);
    bent.bend = {{0.0, 12.0, 0.0}};
    engine.setTrackNotes(keys, {bent});
    CHECK_APPROX_REL(pitchAt(channel(engine.renderOffline(0.0, 2 * kBeat), 0), 4800), 880.0, 2e-3);
    engine.setProcessorMidiInput(synth, 0);  // its own track's again
    engine.setTrackNotes(lead, {note(0.0, 2.0, 57)});
    CHECK_APPROX_REL(pitchAt(channel(engine.renderOffline(0.0, 2 * kBeat), 0), 4800), 220.0, 2e-3);
}

TEST_CASE("an effect with a MIDI input plays along with a MIDI track's notes") {
    requireTestPlugins();
    sub::Engine engine;
    const uint32_t keys = notesTrack(engine, {note(1.0, 1.0, 60, 127), note(3.0, 0.5, 64, 127)});
    const uint32_t audio = engine.addTrack();  // no clips: the effect's notes are all it puts out
    const uint32_t effect = addTestPlugin(engine, engine.trackChain(audio), "SUB Test Note Effect");
    CHECK(maxAbs(engine.renderOffline(0.0, 4 * kBeat)) == 0.0);  // its own track has no notes
    engine.setProcessorMidiInput(effect, keys);
    const Samples left = channel(engine.renderOffline(0.0, 4 * kBeat), 0);
    CHECK(allEqual(slice(left, 0, kBeat), 0.0));
    CHECK(allEqual(slice(left, kBeat, 2 * kBeat), 1.0));  // velocity/127 while the note is held, to the sample
    CHECK(allEqual(slice(left, 2 * kBeat, 3 * kBeat), 0.0));
    CHECK(allEqual(slice(left, 3 * kBeat, 3 * kBeat + kBeat / 2), 1.0));
    CHECK(allEqual(slice(left, 3 * kBeat + kBeat / 2, 4 * kBeat), 0.0));
}

TEST_CASE("a device in a rack takes a track's notes, and keeps them as it moves") {
    sub::Engine engine;
    const uint32_t keys = notesTrack(engine, {note(0.0, 2.0, 69)});
    const uint32_t host = engine.addTrack();
    const uint32_t rack = engine.addRack(engine.trackChain(host), -1);
    const uint32_t chain = engine.addRackChain(rack, -1);
    const uint32_t synth = sineSynthOn(engine, chain);
    engine.setProcessorMidiInput(synth, keys);
    CHECK_APPROX_REL(pitchAt(channel(engine.renderOffline(0.0, 2 * kBeat), 0), 4800), 440.0, 2e-3);
    const uint32_t other = engine.addTrack();
    engine.moveProcessor(synth, engine.trackChain(other), -1);
    CHECK_EQ(engine.processorMidiInput(synth), keys);
    CHECK_APPROX_REL(pitchAt(channel(engine.renderOffline(0.0, 2 * kBeat), 0), 4800), 440.0, 2e-3);
    // Onto its source: its own track's notes are the same notes.
    engine.moveProcessor(synth, engine.trackChain(keys), -1);
    CHECK_APPROX_REL(pitchAt(channel(engine.renderOffline(0.0, 2 * kBeat), 0), 4800), 440.0, 2e-3);
}

TEST_CASE("a frozen track's notes still reach a device elsewhere") {
    sub::Engine engine;
    const uint32_t keys = notesTrack(engine, {note(0.0, 2.0, 69)});
    const uint32_t lead = engine.addTrack();
    const uint32_t synth = sineSynthOn(engine, engine.trackChain(lead));
    engine.setProcessorMidiInput(synth, keys);
    engine.setTrackFrozen(keys, true);
    CHECK_APPROX_REL(pitchAt(channel(engine.renderOffline(0.0, 2 * kBeat), 0), 4800), 440.0, 2e-3);
}

TEST_CASE("a device's MIDI input goes with its track") {
    sub::Engine engine;
    const uint32_t keys = notesTrack(engine, {note(0.0, 4.0, 69)});
    const uint32_t lead = engine.addTrack();
    const uint32_t synth = sineSynthOn(engine, engine.trackChain(lead));
    engine.setProcessorMidiInput(synth, keys);
    CHECK(maxAbs(engine.renderOffline(0.0, kBeat)) > 0.05);
    engine.removeTrack(keys);
    CHECK_EQ(engine.processorMidiInput(synth), 0u);
    CHECK(maxAbs(engine.renderOffline(0.0, kBeat)) == 0.0);  // its own track has none (and nothing hangs)
}

TEST_CASE("what can't take a track's MIDI is refused") {
    sub::Engine engine;
    const uint32_t keys = notesTrack(engine, {});
    const uint32_t track = engine.addTrack();
    const uint32_t utility = engine.addBuiltinProcessor(engine.trackChain(track), "utility", -1);
    const uint32_t synth = sineSynthOn(engine, engine.trackChain(track));
    CHECK_THROWS_MATCHING(engine.setProcessorMidiInput(utility, keys), std::invalid_argument, "takes no MIDI");
    CHECK_THROWS_AS(engine.setProcessorMidiInput(synth, 999), std::invalid_argument);
    CHECK_THROWS_AS(engine.setProcessorMidiInput(999, keys), std::invalid_argument);
    CHECK_EQ(engine.processorMidiInput(synth), 0u);
    engine.setProcessorMidiInput(utility, 0);  // (none: always fine)
    // MIDI carries no audio: two tracks may take each other's notes.
    const uint32_t other = engine.addTrack();
    const uint32_t second = sineSynthOn(engine, engine.trackChain(other));
    engine.setProcessorMidiInput(synth, other);
    engine.setProcessorMidiInput(second, track);
}

TEST_CASE("devices taking tracks' notes render alike on any number of threads") {
    sub::Engine engine;
    std::vector<uint32_t> sources;
    for (int k = 0; k < 3; ++k) sources.push_back(notesTrack(engine, {note(0.25 * k, 2.0, 57 + 5 * k)}));
    for (int k = 0; k < 6; ++k) {
        const uint32_t track = engine.addTrack();
        const uint32_t synth = sineSynthOn(engine, engine.trackChain(track));
        engine.setTrackNotes(track, {note(0.5, 1.0, 72 + k)});
        if (k % 2 == 0) engine.setProcessorMidiInput(synth, sources[static_cast<size_t>(k / 2)]);
    }
    engine.setAudioThreads(1);
    const Samples serial = engine.renderOffline(0.0, 3 * kBeat);
    engine.setAudioThreads(4);
    const Samples parallel = engine.renderOffline(0.0, 3 * kBeat);
    CHECK(maxAbs(serial) > 0.05);
    CHECK_ARRAY_EQUAL(parallel, serial);
}
