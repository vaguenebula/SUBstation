// Parallel track processing: the tracks of a chunk render on several threads
// (the rendering thread and the scheduler's workers), each after what goes into
// it. Whatever the threads, the result is the same bit for bit: random routing
// graphs with nested groups, latent plug-ins, solo, mute and automation render
// identically on one thread and on several. Rendered offline, so no audio device
// is needed (test_parallel_live.cpp plays live, with the fake ASIO driver).

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
#include <thread>

#include "Engine.h"
#include "harness/Fixtures.h"
#include "harness/Signal.h"
#include "harness/TaskGraphOrder.h"

using namespace subtest;

namespace {

// Workers run whatever the computer's cores (oversubscribed on fewer).
constexpr int kThreads = 4;

Samples render(sub::Engine& engine, int threads, int64_t frameCount, double startBeat = 0.0, bool loop = false,
               bool metronome = false) {
    engine.setAudioThreads(threads);
    REQUIRE(engine.audioThreads() == threads);
    return engine.renderOffline(startBeat, frameCount, loop, metronome);
}

// A render to let the parameters set since the last one arrive: a Utility
// ramps to a new gain in its first block (in offline renders too).
void settle(sub::Engine& engine) { engine.renderOffline(0.0, 4096); }

// Renders on one thread and on several: the same samples, and the workers rendered some tracks.
Samples sameOnAnyThreads(sub::Engine& engine, int64_t frameCount) {
    settle(engine);
    const Samples serial = render(engine, 1, frameCount);
    const Samples parallel = render(engine, kThreads, frameCount);
    CHECK(engine.nodesOnWorkers() > 0);  // (or the workers rendered nothing)
    CHECK(maxAbs(serial) > 0.0);
    CHECK_ARRAY_EQUAL(parallel, serial);
    return serial;
}

uint32_t synth(sub::Engine& engine, uint32_t track, const std::vector<sub::NoteDesc>& notes, int wave = 0) {
    const uint32_t processor = engine.addBuiltinProcessor(engine.trackChain(track), "synth", -1);
    engine.setProcessorParam(processor, 0, static_cast<float>(wave));
    engine.setTrackNotes(track, notes);
    return processor;
}

// SUB Test Effect `latency` samples late, at `gain` (FX_GAIN: 0..1 is 0..2 times).
uint32_t latentEffectAt(sub::Engine& engine, uint32_t track, int64_t latency, double gain = 1.0) {
    const uint32_t effect = latentEffect(engine, track, static_cast<int>(latency));
    engine.setProcessorParam(effect, FX_GAIN, static_cast<float>(gain));
    return effect;
}

double round3(double x) { return std::round(x * 1000.0) / 1000.0; }

std::vector<sub::NoteDesc> randomNotes(Rng& rng, double beats = 8.0, int count = 12) {
    std::vector<sub::NoteDesc> notes;
    for (int i = 0; i < count; ++i) {
        const double start = round3(rng.uniform(0.0, beats - 0.25));
        const double length = round3(rng.uniform(0.05, 2.0));
        const auto key = static_cast<int>(rng.integers(36, 96));
        notes.push_back({start, length, key, static_cast<int>(rng.integers(20, 128))});
    }
    return notes;
}

std::vector<sub::AutomationPoint> randomPoints(Rng& rng, int count, double beats, double low, double high) {
    std::vector<double> at;
    for (int i = 0; i < count; ++i) at.push_back(rng.uniform(0.0, beats));
    std::sort(at.begin(), at.end());
    std::vector<sub::AutomationPoint> points;
    for (const double beat : at) points.push_back({beat, static_cast<float>(rng.uniform(low, high)), 0.f});
    return points;
}

struct Project {
    std::vector<uint32_t> groups, leaves;
};

// Clip, synth and group tracks routed at random (groups in groups, tracks
// going into groups made after them), with devices (latent ones too, given the
// plug-ins), gain, pan, solo, mute and automation.
Project randomProject(sub::Engine& engine, Rng& rng, bool plugins, int tracks = 12, double beats = 8.0) {
    std::vector<std::string> noise;
    for (int i = 0; i < 3; ++i) noise.push_back(makeWav(rng.uniformSamples(kSampleRate * 2, -0.5, 0.5), 2));
    for (const auto& path : noise) engine.loadSource(path);
    Project project;
    for (int i = 0; i < tracks; ++i) {
        const uint32_t track = engine.addTrack();
        const size_t kind = rng.weighted({0.4, 0.35, 0.25});  // clip, synth, group
        (kind == 2 ? project.groups : project.leaves).push_back(track);
        if (kind == 0) {
            std::vector<sub::ClipDesc> clips;
            const int64_t count = rng.integers(1, 4);
            for (int64_t c = 0; c < count; ++c) {
                const std::string& path = rng.choice(noise);
                clips.push_back(clip(path, round3(rng.uniform(0.0, beats - 1.0)), 1.0));
            }
            engine.setTrackClips(track, clips);
        } else if (kind == 1) {
            const auto notes = randomNotes(rng, beats);
            synth(engine, track, notes, static_cast<int>(rng.integers(0, 4)));
        }
        if (rng.random() < 0.5) utility(engine, track, rng.uniform(-12.0, 6.0));
        if (plugins && rng.random() < 0.4) {
            const int64_t latency = rng.integers(0, 600);
            latentEffectAt(engine, track, latency, rng.uniform(0.25, 1.0));
        }
        engine.setTrackGain(track, static_cast<float>(rng.uniform(0.3, 1.0)));
        engine.setTrackPan(track, static_cast<float>(rng.uniform(-1.0, 1.0)));
        engine.setTrackMute(track, rng.random() < 0.1);
        engine.setTrackSolo(track, rng.random() < 0.1);
        if (rng.random() < 0.25) engine.setTrackAutomation(track, {{0, "volume", randomPoints(rng, 4, beats, 0.2, 1.0)}});
        if (!project.groups.empty() && rng.random() < 0.7) {  // into a group made before it (groups nest)
            const uint32_t parent = rng.choice(project.groups);
            if (parent != track) engine.setTrackOutput(track, parent);
        }
    }
    for (const uint32_t track : project.leaves) {  // some into groups made after them: the snapshot reorders them
        if (!project.groups.empty() && rng.random() < 0.3) engine.setTrackOutput(track, rng.choice(project.groups));
    }
    if (plugins && rng.random() < 0.5) latentEffectAt(engine, sub::Engine::kMaster, rng.integers(0, 300));
    return project;
}

// Return tracks (with devices, latent ones too), and sends into them at
// random: from tracks and groups, from returns into later ones, after the
// fader or before it, at random levels, some automated, a few soloed or muted.
std::vector<uint32_t> addReturns(sub::Engine& engine, Rng& rng, bool plugins, const std::vector<uint32_t>& tracks,
                                 int64_t count = 3, double beats = 8.0) {
    std::vector<uint32_t> returns;
    for (int64_t i = 0; i < count; ++i) returns.push_back(engine.addTrack());
    for (const uint32_t ret : returns) {
        utility(engine, ret, rng.uniform(-6.0, 0.0));
        if (plugins && rng.random() < 0.6) {
            const int64_t latency = rng.integers(0, 500);
            latentEffectAt(engine, ret, latency, rng.uniform(0.25, 1.0));
        }
        engine.setTrackGain(ret, static_cast<float>(rng.uniform(0.3, 1.0)));
        engine.setTrackMute(ret, rng.random() < 0.1);
        engine.setTrackSolo(ret, rng.random() < 0.15);
    }
    for (size_t i = 0; i < returns.size(); ++i) {  // into later returns only: no cycles
        for (size_t later = i + 1; later < returns.size(); ++later) {
            if (rng.random() < 0.4) {
                const auto gain = static_cast<float>(rng.uniform(0.1, 1.0));
                engine.setTrackSend(returns[i], returns[later], gain, rng.random() < 0.5);
            }
        }
    }
    for (const uint32_t track : tracks) {
        for (const uint32_t ret : returns) {
            if (rng.random() < 0.5) {
                const auto gain = static_cast<float>(rng.uniform(0.1, 1.0));
                engine.setTrackSend(track, ret, gain, rng.random() < 0.4);
                if (rng.random() < 0.2)
                    engine.setTrackAutomation(track, {{0, "send:" + std::to_string(ret), randomPoints(rng, 3, beats, 0.0, 1.0)}});
            }
        }
    }
    return returns;
}

// SUB Test Sidechain on some tracks (and the master), keyed at random by
// others: after their fader, before it or after one of their devices (latent
// ones too). Those that would close a cycle are refused.
int addSidechains(sub::Engine& engine, Rng& rng, const std::vector<uint32_t>& tracks) {
    int keyed = 0;
    std::vector<uint32_t> consumers{sub::Engine::kMaster};
    consumers.insert(consumers.end(), tracks.begin(), tracks.end());
    for (const uint32_t consumer : consumers) {
        if (rng.random() < (consumer == sub::Engine::kMaster ? 0.7 : 0.35)) {
            const int index = static_cast<int>(rng.integers(0, 3)) - 1;
            const uint32_t pid = addTestPlugin(engine, engine.trackChain(consumer), "SUB Test Sidechain", index);
            if (consumer != sub::Engine::kMaster && rng.random() < 0.3)
                latentEffectAt(engine, consumer, rng.integers(1, 400));  // after it
            const uint32_t source = rng.choice(tracks);
            std::vector<sub::SidechainTap> taps{sub::SidechainTap::PostFader, sub::SidechainTap::PreFader,
                                                sub::SidechainTap::PreFx};
            uint32_t device = 0;
            if (rng.random() < 0.4) {  // a device to tap after, before the source's others
                device = latentEffectAt(engine, source, rng.integers(0, 500));
                engine.moveProcessor(device, engine.trackChain(source), 0);
                taps.push_back(sub::SidechainTap::AfterDevice);
            }
            const sub::SidechainTap tap = taps[static_cast<size_t>(rng.integers(0, static_cast<int64_t>(taps.size())))];
            try {
                engine.setProcessorSidechain(pid, source, tap, tap == sub::SidechainTap::AfterDevice ? device : 0);
                ++keyed;
            } catch (const std::invalid_argument&) {
                // its own track, or one it feeds
            }
        }
    }
    return keyed;
}

// The graph renders the same on one thread and on several, and so it does
// looping with the metronome from another position.
void checkRandomGraph(sub::Engine& engine, bool alsoLooping, bool sound = false) {
    settle(engine);
    Samples serial = render(engine, 1, 8 * kBeat);
    Samples parallel = render(engine, kThreads, 8 * kBeat);
    if (sound) CHECK(maxAbs(serial) > 0.0);
    CHECK_ARRAY_EQUAL(parallel, serial);
    if (!alsoLooping) return;
    engine.setLoop(true, 2.0, 5.0);
    serial = render(engine, 1, 6 * kBeat, 1.5, true, true);
    parallel = render(engine, kThreads, 6 * kBeat, 1.5, true, true);
    CHECK_ARRAY_EQUAL(parallel, serial);
}

std::vector<uint32_t> joined(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    std::vector<uint32_t> all = a;
    all.insert(all.end(), b.begin(), b.end());
    return all;
}

}  // namespace

TEST_CASE("the number of audio threads") {
    sub::Engine engine;
    const int expected = std::max(1, std::min(64, static_cast<int>(std::thread::hardware_concurrency()) - 1));
    CHECK_EQ(sub::Engine::defaultAudioThreads(), expected);
    CHECK_EQ(engine.audioThreads(), expected);
    engine.setAudioThreads(3);
    CHECK_EQ(engine.audioThreads(), 3);
    engine.setAudioThreads(0);  // at least the audio thread
    CHECK_EQ(engine.audioThreads(), 1);
    engine.setAudioThreads(1000);
    CHECK_EQ(engine.audioThreads(), 64);
}

TEST_CASE("a track renders the same on any thread") {
    // Built-in synths (with state: their voices go on across blocks) on many
    // tracks, each moving between threads from block to block.
    sub::Engine engine;
    Rng rng(1);
    for (int i = 0; i < 16; ++i) {
        const uint32_t track = engine.addTrack();
        const auto notes = randomNotes(rng, 16.0, 24);
        synth(engine, track, notes, static_cast<int>(rng.integers(0, 4)));
    }
    sameOnAnyThreads(engine, 16 * kBeat);
}

TEST_CASE("a plug-in keeps its state on any thread") {
    // A plug-in's delay line (its latency) carries audio from block to block, on
    // whichever worker it ran: the render is the same as on one thread.
    requireTestPlugins();
    sub::Engine engine;
    Rng rng(2);
    for (int i = 0; i < 8; ++i) {
        const uint32_t track = engine.addTrack();
        const std::string path = makeWav(rng.uniformSamples(kSampleRate * 2, -0.5, 0.5), 2);
        engine.loadSource(path);
        engine.setTrackClips(track, {clip(path, 0.0, 1.0)});
        latentEffectAt(engine, track, rng.integers(1, 2000));
    }
    sameOnAnyThreads(engine, 4 * kBeat);
}

TEST_CASE("random graphs render the same on any threads") {
    for (int seed = 0; seed < 12; ++seed) {
        INFO("seed " + std::to_string(seed));
        sub::Engine engine;
        Rng rng(static_cast<uint64_t>(seed));
        const int tracks = static_cast<int>(rng.integers(4, 20));
        randomProject(engine, rng, haveTestPlugins(), tracks);
        checkRandomGraph(engine, true);
    }
}

TEST_CASE("random graphs with sends render the same on any threads") {
    // The graph fans out: tracks going into a group and several returns, pre-
    // and post-fader, returns into returns, each edge delayed on its own.
    for (int seed = 0; seed < 10; ++seed) {
        INFO("seed " + std::to_string(100 + seed));
        sub::Engine engine;
        Rng rng(static_cast<uint64_t>(100 + seed));
        const int tracks = static_cast<int>(rng.integers(4, 16));
        const Project project = randomProject(engine, rng, haveTestPlugins(), tracks);
        const int64_t returns = rng.integers(1, 5);
        addReturns(engine, rng, haveTestPlugins(), joined(project.groups, project.leaves), returns);
        checkRandomGraph(engine, true, true);
    }
}

TEST_CASE("random graphs with inputs render the same on any threads") {
    // Tracks taking their input from others (resampling): an input edge isn't
    // heard offline, but orders the graph like any edge (the source first).
    for (int seed = 0; seed < 6; ++seed) {
        INFO("seed " + std::to_string(200 + seed));
        sub::Engine engine;
        Rng rng(static_cast<uint64_t>(200 + seed));
        const int trackCount = static_cast<int>(rng.integers(4, 16));
        const Project project = randomProject(engine, rng, haveTestPlugins(), trackCount);
        const int64_t returnCount = rng.integers(0, 3);
        const auto returns = addReturns(engine, rng, haveTestPlugins(), joined(project.groups, project.leaves), returnCount);
        std::vector<uint32_t> sources{sub::Engine::kMaster};
        for (const uint32_t t : joined(joined(project.groups, project.leaves), returns)) sources.push_back(t);
        int inputs = 0;
        for (const uint32_t track : project.leaves) {
            if (rng.random() < 0.6) {
                try {
                    engine.setTrackInputTrack(track, rng.choice(sources));
                    ++inputs;
                } catch (const std::invalid_argument&) {
                    // itself, or a track it feeds
                }
            }
        }
        CHECK((inputs > 0 || project.leaves.empty()));
        checkRandomGraph(engine, false);
    }
}

TEST_CASE("random graphs with sidechains render the same on any threads") {
    // Sidechains serialise their source before the device's track, and line up
    // at the device (the sidechain delayed, or the track's signal before it).
    // (Ten graphs, as in Python, from seeds 301-310: this isn't numpy's generator,
    // and its seed 300 draws a project of four empty groups, which is silent.)
    requireTestPlugins();
    for (int seed = 1; seed <= 10; ++seed) {
        INFO("seed " + std::to_string(300 + seed));
        sub::Engine engine;
        Rng rng(static_cast<uint64_t>(300 + seed));
        const int trackCount = static_cast<int>(rng.integers(4, 16));
        const Project project = randomProject(engine, rng, true, trackCount);
        const int64_t returnCount = rng.integers(0, 3);
        const auto returns = addReturns(engine, rng, true, joined(project.groups, project.leaves), returnCount);
        CHECK(addSidechains(engine, rng, joined(joined(project.groups, project.leaves), returns)) > 0);
        checkRandomGraph(engine, true, true);
    }
}

TEST_CASE("random graphs with Ableton's outputs and inputs render the same on any threads") {
    // Tracks routed as Ableton routes them: some sending only, some into
    // tracks that take what comes in as their input (Track In, monitoring In:
    // heard offline), some into keyed devices' sidechains (summed there with the
    // devices' own), some taking another track's output tapped before its
    // devices, before its fader or after it. Routes that would close a cycle
    // are refused.
    requireTestPlugins();
    for (int seed = 1; seed <= 8; ++seed) {
        INFO("seed " + std::to_string(500 + seed));
        sub::Engine engine;
        Rng rng(static_cast<uint64_t>(500 + seed));
        const int trackCount = static_cast<int>(rng.integers(5, 16));
        const Project project = randomProject(engine, rng, true, trackCount);
        const auto all = joined(project.groups, project.leaves);
        addSidechains(engine, rng, all);
        std::vector<uint32_t> keyedDevices;
        for (const uint32_t track : all) {
            for (const uint32_t pid : engine.chainProcessors(engine.trackChain(track))) {
                if (engine.processorInfo(pid).hasSidechain) keyedDevices.push_back(pid);
            }
        }
        for (const uint32_t track : project.leaves) {
            if (rng.random() < 0.4) {
                engine.setTrackInMonitored(track, true);
                engine.setTrackMonitor(track, sub::MonitorMode::In);
            }
        }
        int routed = 0;
        for (const uint32_t track : all) {
            const double what = rng.random();
            try {
                if (what < 0.15) {
                    engine.setTrackOutput(track, sub::Engine::kNoOutput);
                } else if (what < 0.4 && !keyedDevices.empty()) {
                    engine.setTrackOutputSidechain(track, rng.choice(keyedDevices));
                } else if (what < 0.6) {
                    engine.setTrackOutput(track, rng.choice(project.leaves));
                } else if (what < 0.8) {
                    const std::vector<sub::SidechainTap> taps{sub::SidechainTap::PostFader, sub::SidechainTap::PreFader,
                                                              sub::SidechainTap::PreFx};
                    engine.setTrackInputTrack(track, rng.choice(all), taps[static_cast<size_t>(rng.integers(0, 3))]);
                }
                ++routed;
            } catch (const std::invalid_argument&) {
                // itself, or a track it feeds
            }
        }
        CHECK(routed > 0);
        checkRandomGraph(engine, true);
    }
}

TEST_CASE("nested groups on any threads") {
    // Groups three deep, with tracks at every level: each bus sums what goes into it in a fixed order.
    sub::Engine engine;
    Rng rng(7);
    const std::string noise = makeWav(rng.uniformSamples(kSampleRate * 2, -0.5, 0.5), 2);
    engine.loadSource(noise);
    const uint32_t outer = engine.addTrack();
    const uint32_t middle = engine.addTrack();
    const uint32_t inner = engine.addTrack();
    engine.setTrackOutput(middle, outer);
    engine.setTrackOutput(inner, middle);
    for (const uint32_t bus : {outer, middle, inner}) {
        utility(engine, bus, -3.0);
        for (int i = 0; i < 4; ++i) {
            const uint32_t track = engine.addTrack();
            engine.setTrackOutput(track, bus);
            if (i % 2) {
                synth(engine, track, randomNotes(rng, 4.0));
            } else {
                engine.setTrackClips(track, {clip(noise, i * 0.5, 1.0)});
                utility(engine, track, i);
            }
            if (haveTestPlugins() && i == 3) latentEffectAt(engine, track, 100 * (bus % 4) + 37);
        }
    }
    sameOnAnyThreads(engine, 4 * kBeat);
}

TEST_CASE("solo and mute on any threads") {
    sub::Engine engine;
    Rng rng(3);
    const uint32_t bus = engine.addTrack();
    std::vector<uint32_t> tracks;
    for (int i = 0; i < 8; ++i) {
        const uint32_t track = engine.addTrack();
        synth(engine, track, randomNotes(rng, 4.0));
        if (i < 4) engine.setTrackOutput(track, bus);
        tracks.push_back(track);
    }
    const std::vector<std::pair<std::vector<uint32_t>, std::vector<uint32_t>>> cases{
        {{tracks[0]}, {}}, {{bus}, {tracks[1]}}, {{tracks[5], tracks[2]}, {bus}}, {{}, {tracks[6]}}};
    int n = 0;
    for (const auto& [solo, mute] : cases) {
        INFO("case " + std::to_string(n++));
        for (const uint32_t track : joined({bus}, tracks)) {
            engine.setTrackSolo(track, std::find(solo.begin(), solo.end(), track) != solo.end());
            engine.setTrackMute(track, std::find(mute.begin(), mute.end(), track) != mute.end());
        }
        sameOnAnyThreads(engine, 4 * kBeat);
    }
}

TEST_CASE("stress") {
    // Many tracks, many renders, the thread count changing in between: always the same.
    sub::Engine engine;
    Rng rng(4);
    const std::string noise = makeWav(rng.uniformSamples(kSampleRate * 2, -0.5, 0.5), 2);
    engine.loadSource(noise);
    std::vector<uint32_t> groups;
    for (int i = 0; i < 6; ++i) groups.push_back(engine.addTrack());
    for (size_t i = 1; i < groups.size(); ++i) engine.setTrackOutput(groups[i], groups[i / 2]);
    for (int i = 0; i < 58; ++i) {
        const uint32_t track = engine.addTrack();
        engine.setTrackOutput(track, groups[static_cast<size_t>(i) % groups.size()]);
        if (i % 3 == 0) {
            synth(engine, track, randomNotes(rng, 2.0, 6));
        } else {
            engine.setTrackClips(track, {clip(noise, rng.uniform(0.0, 1.0), 1.0)});
            utility(engine, track, rng.uniform(-12.0, 0.0));
        }
    }
    settle(engine);
    const Samples expected = render(engine, 1, 2 * kBeat);
    CHECK(maxAbs(expected) > 0.0);
    for (int run = 0; run < 30; ++run) {
        INFO("run " + std::to_string(run));
        const int threads = std::vector<int>{2, 3, kThreads, 8}[static_cast<size_t>(run % 4)];
        CHECK_ARRAY_EQUAL(render(engine, threads, 2 * kBeat), expected);
    }
    CHECK(engine.nodesOnWorkers() > 0);
}

TEST_CASE("few tracks render serially") {
    // Below the threshold (few tracks with work, small blocks) the workers aren't woken.
    sub::Engine engine;
    const uint32_t track = engine.addTrack();
    synth(engine, track, {{0.0, 4.0, 60, 100}});
    render(engine, kThreads, 4 * kBeat);
    CHECK_EQ(engine.nodesOnWorkers(), uint64_t{0});  // one track with devices: nothing to share
}

// --- Cost ordering ------------------------------------------------------------------

TEST_CASE("the longest paths start first") {
    // A node's rank is the work from it to the end of its path; the nodes without
    // inputs are queued by rank, highest first, ties (and unknown costs) in order.
    // 0 -> master; 1, 2 -> group 5 -> master; 3 -> group 4 -> group 5; 6 -> master
    const std::vector<int> outputs{-1, 5, 5, 4, 5, -1, -1};
    const std::vector<float> costs{1.0f, 2.0f, 0.5f, 0.25f, 1.0f, 5.0f, 4.0f};
    const auto [order, ranks] = taskGraphOrder(outputs, costs);
    const std::vector<double> expected{1.0, 7.0, 5.5, 6.25, 6.0, 5.0, 4.0};
    REQUIRE(ranks.size() == expected.size());
    for (size_t i = 0; i < ranks.size(); ++i) CHECK_APPROX(ranks[i], expected[i]);
    CHECK(order == (std::vector<int>{1, 3, 2, 6, 0}));  // a light track feeding heavy groups goes before a heavier one alone
    CHECK(taskGraphOrder(outputs, std::vector<float>(7, 0.f)).first == (std::vector<int>{0, 1, 2, 3, 6}));
    CHECK(taskGraphOrder(std::vector<int>{-1, -1, -1}, {2.f, 2.f, 3.f}).first == (std::vector<int>{2, 0, 1}));
    CHECK_THROWS_AS(taskGraphOrder(std::vector<int>{-1, 0}, {1.f, 1.f}), std::invalid_argument);  // a group listed before what goes into it
    CHECK_THROWS_AS(taskGraphOrder(std::vector<int>{-1, -1}, {1.f}), std::invalid_argument);
}

TEST_CASE("track costs are measured") {
    sub::Engine engine;
    const uint32_t light = engine.addTrack();
    utility(engine, light, 0.0);
    const uint32_t heavy = engine.addTrack();
    std::vector<sub::NoteDesc> chord;
    for (int key = 48; key < 72; key += 3) chord.push_back({0.0, 4.0, key, 100});
    synth(engine, heavy, chord);
    for (int i = 0; i < 4; ++i) engine.addBuiltinProcessor(engine.trackChain(heavy), "ott", -1);
    std::map<uint32_t, float> costs;
    for (const auto& c : engine.trackCosts()) costs[c.trackId] = c.nsPerFrame;
    CHECK(costs == (std::map<uint32_t, float>{{light, 0.f}, {heavy, 0.f}}));
    render(engine, kThreads, 4 * kBeat);
    costs.clear();
    for (const auto& c : engine.trackCosts()) costs[c.trackId] = c.nsPerFrame;
    CHECK(costs[light] > 0.f);
    CHECK(costs[heavy] > 4 * costs[light]);
}

TEST_CASE("cost ordering changes nothing but the order") {
    // Heavy tracks last in routing order: started first by cost, last without; the same render either way.
    sub::Engine engine;
    Rng rng(5);
    const uint32_t bus = engine.addTrack();
    for (int i = 0; i < 12; ++i) {
        const uint32_t track = engine.addTrack();
        synth(engine, track, randomNotes(rng, 4.0));
        if (i % 3 == 0) engine.setTrackOutput(track, bus);
        if (i >= 10)
            for (int j = 0; j < 3; ++j) engine.addBuiltinProcessor(engine.trackChain(track), "ott", -1);
    }
    CHECK(engine.costOrdering());
    const Samples byCost = sameOnAnyThreads(engine, 4 * kBeat);
    engine.setCostOrdering(false);
    CHECK(!engine.costOrdering());
    CHECK_ARRAY_EQUAL(render(engine, kThreads, 4 * kBeat), byCost);
}
