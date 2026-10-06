// Background freezing (docs/engine/background-freeze.md): what playing strips
// from their cache saves, and what keeping it costs, on a song of real VST3
// plug-ins.
//
//     background_freeze_bench [--bars 32] [--threads 1] [--buffer 256] [--warm 8] [--idle 0] [--background 1]
//
// The song is the one the plug-in CPU experiments measured (perf-experiments
// branch, benchmarks/plugin_cpu_bench.cpp): 16 tracks in sections of 8 bars,
// audio loops and the built-in Synth and DISTRHO's Kars, through LSP, ZamAudio
// and DISTRHO effects, into a drum and a music group, sends into a Dragonfly
// Hall and a ZamDelay return, and a mastering chain. It needs the open-source
// plug-ins Ubuntu packages (lsp-plugins-vst3, dragonfly-reverb-vst3,
// zam-plugins, dpf-plugins) in /usr/lib/vst3.
//
// It plays the song through the "Manual" driver, a buffer at a time on this
// thread, with background freezing off and on (no wait after a change unless
// --idle; a warm-up of --warm seconds): through once, again, again after a
// note edit on one track in the third section, and again after a parameter of
// a drum track's EQ changed. The cache store's work is done after each buffer,
// as its thread would in real time, and timed apart. With --background 1 (phase
// 2), before each pass the background renderer renders all it would before the
// pass plays (on this thread: its CPU time is the background's, apart; the
// first time, it makes the shadow instances too). For each pass: the CPU
// time of the buffers on this thread (with --threads 1, all the rendering),
// their wall time, the store's CPU time, the share of strips' frames played
// from the cache, and how far the output is from the same pass with the cache
// off (the energy of the difference against the signal's: the song has
// plug-ins that never render the same twice, so two passes without the cache
// differ too, shown as the baseline).

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "Engine.h"
#include "Fixtures.h"
#include "backends/ManualBackend.h"
#include "plugins/Vst3Format.h"

// The harness's failure report (its Main.cpp is the tests' own).
namespace subtest {
void fail(const char* file, int line, const std::string& message) {
    std::fprintf(stderr, "%s:%d: %s\n", std::filesystem::path(file).filename().string().c_str(), line,
                 message.c_str());
}
}  // namespace subtest

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kRate = 48000;
constexpr double kBeatSec = 0.5;  // 120 BPM
constexpr int kBarBeats = 4;
constexpr int kSectionBars = 8;
constexpr double kSectionSec = kSectionBars * kBarBeats * kBeatSec;  // 16 s
const char* const kLsp = "/usr/lib/vst3/lsp-plugins.vst3";

std::string vst3(const std::string& name) { return "/usr/lib/vst3/" + name + ".vst3"; }

struct Stop : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// CPU time of the calling thread (on Windows in the scheduler's quanta: right
// over many buffers, not one).
double threadCpuSeconds() {
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0.0;
    const auto ticks = [](const FILETIME& t) {
        return (static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
    };
    return static_cast<double>(ticks(kernel) + ticks(user)) * 1e-7;  // (100 ns ticks)
#else
    timespec t{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return static_cast<double>(t.tv_sec) + static_cast<double>(t.tv_nsec) * 1e-9;
#endif
}

class TempFolder {
public:
    TempFolder() {
        std::random_device random;
        path_ = std::filesystem::temp_directory_path() / ("sub-freeze-bench-" + std::to_string(random()));
        std::filesystem::create_directories(path_);
    }
    ~TempFolder() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TempFolder(const TempFolder&) = delete;
    TempFolder& operator=(const TempFolder&) = delete;
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

// --- Plug-ins ---------------------------------------------------------------------------------

struct PluginRef {
    std::string path;
    std::string name;
};

std::string uidOf(const PluginRef& plugin) {
    static std::map<std::string, std::vector<sub::PluginDescription>> scanned;
    auto it = scanned.find(plugin.path);
    if (it == scanned.end()) it = scanned.emplace(plugin.path, sub::vst3::Vst3Format::instance().scanFile(plugin.path)).first;
    for (const auto& d : it->second)
        if (d.name == plugin.name) return d.uid;
    throw Stop("No plug-in named '" + plugin.name + "' in " + plugin.path);
}

uint32_t addPlugin(sub::Engine& engine, uint32_t chain, const PluginRef& plugin) {
    return engine.addPluginProcessor(chain, "VST3", plugin.path, uidOf(plugin), -1);
}

const PluginRef kEq{kLsp, "Parametric Equalizer x8 Stereo"};
const PluginRef kComp{kLsp, "Compressor Stereo"};
const PluginRef kChorus{kLsp, "Chorus Stereo"};
const PluginRef kFilter{kLsp, "Filter Stereo"};
const PluginRef kMultiband{kLsp, "Multiband Compressor Stereo x8"};
const PluginRef kLimiter{kLsp, "Limiter Stereo"};
const PluginRef kDynamics{kLsp, "Dynamics Processor Stereo"};
const PluginRef kHall{vst3("DragonflyHallReverb"), "Dragonfly Hall Reverb"};
const PluginRef kDelay{vst3("ZamDelay"), "ZamDelay"};
const PluginRef kSoulForce{vst3("SoulForce"), "Soul Force"};
const PluginRef kCompX2{vst3("ZamCompX2"), "ZamCompX2"};
const PluginRef kKars{vst3("Kars"), "Kars"};

// --- Audio for the song --------------------------------------------------------------------------

struct Sounds {
    std::string kick, snare, hats, perc, vocal, guitar, riser, impact;
};

// An 8-bar loop (stereo) built from one-shots at beat positions.
std::vector<float> loopOf(const std::vector<float>& hit, const std::vector<double>& beatsInBar) {
    const auto frames = static_cast<size_t>(kSectionSec * kRate);
    std::vector<float> out(frames * 2, 0.f);
    for (int bar = 0; bar < kSectionBars; ++bar) {
        for (const double beat : beatsInBar) {
            const auto at = static_cast<size_t>((bar * kBarBeats + beat) * kBeatSec * kRate);
            for (size_t i = 0; i < hit.size() && at + i < frames; ++i) {
                out[2 * (at + i)] += hit[i];
                out[2 * (at + i) + 1] += hit[i];
            }
        }
    }
    return out;
}

Sounds makeSounds(const std::filesystem::path& folder) {
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> uniform(-1.f, 1.f);
    const auto decaying = [&](double seconds, double decay, double freq, double noise) {
        std::vector<float> hit(static_cast<size_t>(seconds * kRate));
        double phase = 0.0;
        for (size_t i = 0; i < hit.size(); ++i) {
            const double t = static_cast<double>(i) / kRate;
            const double f = freq * (1.0 + 2.0 * std::exp(-t * 40.0));  // a pitch drop, as a kick's
            phase += 2.0 * M_PI * f / kRate;
            const double env = std::exp(-t / decay);
            hit[i] = static_cast<float>(0.7 * env * ((1.0 - noise) * std::sin(phase) + noise * uniform(rng)));
        }
        return hit;
    };
    Sounds s;
    s.kick = subtest::writeWav(folder / "kick.wav", loopOf(decaying(0.35, 0.08, 50, 0.05), {0, 1, 2, 3}), 2, kRate);
    s.snare = subtest::writeWav(folder / "snare.wav", loopOf(decaying(0.25, 0.06, 180, 0.7), {1, 3}), 2, kRate);
    s.hats = subtest::writeWav(folder / "hats.wav",
                               loopOf(decaying(0.06, 0.015, 8000, 0.95), {0, 0.5, 1, 1.5, 2, 2.5, 3, 3.5}), 2, kRate);
    s.perc = subtest::writeWav(folder / "perc.wav", loopOf(decaying(0.15, 0.04, 400, 0.4), {0.75, 2.25, 3.5}), 2, kRate);
    // "Vocal" and "guitar": phrases of a few beats with gaps, tones with vibrato and some noise.
    const auto phrases = [&](double freq, double noise, double phraseBeats, double gapBeats) {
        const auto frames = static_cast<size_t>(kSectionSec * kRate);
        std::vector<float> out(frames * 2, 0.f);
        double phase = 0.0;
        for (size_t i = 0; i < frames; ++i) {
            const double beat = static_cast<double>(i) / kRate / kBeatSec;
            const double inPhrase = std::fmod(beat, phraseBeats + gapBeats);
            if (inPhrase >= phraseBeats) continue;
            const double env = std::min({1.0, inPhrase * 8.0, (phraseBeats - inPhrase) * 8.0});
            phase += 2.0 * M_PI * freq * (1.0 + 0.01 * std::sin(beat * 10.0)) / kRate;
            const auto v = static_cast<float>(0.3 * env * ((1.0 - noise) * std::sin(phase) + noise * uniform(rng)));
            out[2 * i] = v;
            out[2 * i + 1] = v;
        }
        return out;
    };
    s.vocal = subtest::writeWav(folder / "vocal.wav", phrases(330, 0.2, 6, 2), 2, kRate);
    s.guitar = subtest::writeWav(folder / "guitar.wav", phrases(196, 0.1, 3, 1), 2, kRate);
    {
        const auto frames = static_cast<size_t>(kSectionSec * kRate);
        std::vector<float> riser(frames * 2);
        for (size_t i = 0; i < frames; ++i) {
            const auto v = static_cast<float>(0.25 * static_cast<double>(i) / static_cast<double>(frames) * uniform(rng));
            riser[2 * i] = v;
            riser[2 * i + 1] = v;
        }
        s.riser = subtest::writeWav(folder / "riser.wav", riser, 2, kRate);
    }
    s.impact = subtest::writeWav(folder / "impact.wav", loopOf(decaying(1.5, 0.4, 60, 0.5), {0}), 2, kRate);
    return s;
}

// --- The song ------------------------------------------------------------------------------------

struct Song {
    std::vector<uint32_t> tracks;  // the 16 instruments
    uint32_t drums = 0, music = 0, reverb = 0, delay = 0;
    uint32_t kickEq = 0;  // the kick's EQ (the parameter edit)
    uint32_t lead = 0;    // a synth (the note edit)
    std::vector<sub::NoteDesc> leadNotes;
};

// Sections: 0 intro, 1 verse, 2 build, 3 chorus, 4 break, 5 chorus.
using Sections = std::vector<int>;

std::vector<sub::ClipDesc> clipsIn(const std::string& path, const Sections& sections, int numSections) {
    std::vector<sub::ClipDesc> clips;
    for (const int s : sections)
        if (s < numSections) clips.push_back(subtest::clip(path, s * kSectionBars * kBarBeats, kSectionSec));
    return clips;
}

std::vector<sub::NoteDesc> notesIn(const Sections& sections, int numSections, const std::vector<int>& chord,
                                   double every, double length, int octaveShiftEvery = 0) {
    std::vector<sub::NoteDesc> notes;
    const int roots[4] = {0, 5, 3, 7};
    for (const int s : sections) {
        if (s >= numSections) continue;
        const double start = s * kSectionBars * kBarBeats;
        for (double beat = 0; beat < kSectionBars * kBarBeats; beat += every) {
            const int root = roots[static_cast<int>(beat / 8) % 4];
            const int shift = octaveShiftEvery ? 12 * (static_cast<int>(beat / every) % octaveShiftEvery) : 0;
            for (const int key : chord) notes.push_back({start + beat, length, key + root + shift, 100});
        }
    }
    return notes;
}

Song buildSong(sub::Engine& engine, const Sounds& sounds, int bars) {
    Song song;
    const int numSections = (bars + kSectionBars - 1) / kSectionBars;
    for (const std::string& path : {sounds.kick, sounds.snare, sounds.hats, sounds.perc, sounds.vocal, sounds.guitar,
                                    sounds.riser, sounds.impact})
        engine.loadSource(path);

    // Groups and returns.
    song.drums = engine.addTrack();
    addPlugin(engine, engine.trackChain(song.drums), kComp);
    addPlugin(engine, engine.trackChain(song.drums), kSoulForce);
    song.music = engine.addTrack();
    addPlugin(engine, engine.trackChain(song.music), kEq);
    addPlugin(engine, engine.trackChain(song.music), kCompX2);
    song.reverb = engine.addTrack();
    addPlugin(engine, engine.trackChain(song.reverb), kHall);
    addPlugin(engine, engine.trackChain(song.reverb), kEq);
    song.delay = engine.addTrack();
    addPlugin(engine, engine.trackChain(song.delay), kDelay);
    addPlugin(engine, engine.trackChain(song.delay), kFilter);
    // The master.
    const uint32_t master = engine.trackChain(sub::Engine::kMaster);
    addPlugin(engine, master, kEq);
    addPlugin(engine, master, kMultiband);
    addPlugin(engine, master, kLimiter);

    const auto route = [&](uint32_t track, uint32_t group, float reverbSend, float delaySend, float gain) {
        if (group) engine.setTrackOutput(track, group);
        if (reverbSend > 0) engine.setTrackSend(track, song.reverb, reverbSend, false);
        if (delaySend > 0) engine.setTrackSend(track, song.delay, delaySend, false);
        engine.setTrackGain(track, gain);
        song.tracks.push_back(track);
    };
    const auto audioTrack = [&](const std::string& path, const Sections& sections, uint32_t group,
                                const std::vector<PluginRef>& chain, float reverbSend, float delaySend) {
        const uint32_t track = engine.addTrack();
        engine.setTrackClips(track, clipsIn(path, sections, numSections));
        for (const auto& plugin : chain) addPlugin(engine, engine.trackChain(track), plugin);
        route(track, group, reverbSend, delaySend, 0.5f);
        return track;
    };
    const auto synthTrack = [&](const std::vector<sub::NoteDesc>& notes, int waveform, const PluginRef* instrument,
                                uint32_t group, const std::vector<PluginRef>& chain, float reverbSend, float delaySend) {
        const uint32_t track = engine.addTrack();
        if (instrument) {
            addPlugin(engine, engine.trackChain(track), *instrument);
        } else {
            const uint32_t synth = engine.addBuiltinProcessor(engine.trackChain(track), "synth", -1);
            engine.setProcessorParam(synth, 0, static_cast<float>(waveform));
        }
        engine.setTrackNotes(track, notes);
        for (const auto& plugin : chain) addPlugin(engine, engine.trackChain(track), plugin);
        route(track, group, reverbSend, delaySend, 0.3f);
        return track;
    };

    // Drums.
    const uint32_t kick = audioTrack(sounds.kick, {1, 2, 3, 5}, song.drums, {kEq, kComp}, 0, 0);
    song.kickEq = engine.chainProcessors(engine.trackChain(kick)).front();
    audioTrack(sounds.snare, {1, 3, 5}, song.drums, {kEq, kComp}, 0.3f, 0);
    audioTrack(sounds.hats, {0, 1, 2, 3, 5}, song.drums, {kEq}, 0, 0);
    audioTrack(sounds.perc, {2, 3, 5}, song.drums, {kEq, kDelay}, 0.2f, 0);
    // Music.
    synthTrack(notesIn({1, 2, 3, 5}, numSections, {36}, 0.5, 0.4), 1, nullptr, song.music, {kEq, kComp, kSoulForce}, 0, 0);
    synthTrack(notesIn({3, 5}, numSections, {24}, 2.0, 1.9), 0, nullptr, song.music, {kEq, kComp}, 0, 0);
    synthTrack(notesIn({0, 2, 4, 5}, numSections, {48, 52, 55, 59}, 8.0, 7.9), 2, nullptr, song.music, {kEq, kChorus}, 0.4f, 0);
    song.leadNotes = notesIn({3, 5}, numSections, {72}, 0.75, 0.5, 2);
    song.lead = synthTrack(song.leadNotes, 1, nullptr, song.music, {kEq, kComp}, 0.3f, 0.3f);
    synthTrack(notesIn({1, 2, 3}, numSections, {60, 67}, 0.25, 0.2, 3), 3, nullptr, song.music, {kEq, kFilter}, 0, 0.2f);
    synthTrack(notesIn({1, 4}, numSections, {60, 64, 67}, 2.0, 1.5), 0, &kKars, song.music, {kEq, kComp}, 0.2f, 0);
    audioTrack(sounds.vocal, {1, 3, 5}, 0, {kEq, kComp, kDynamics}, 0.3f, 0.2f);
    audioTrack(sounds.riser, {2}, 0, {kFilter}, 0.5f, 0);
    synthTrack(notesIn({4, 5}, numSections, {55, 59, 62, 67}, 4.0, 3.9), 2, nullptr, song.music, {kEq}, 0.5f, 0);
    audioTrack(sounds.guitar, {1, 3}, song.music, {kSoulForce, kEq}, 0.2f, 0);
    audioTrack(sounds.vocal, {3, 5}, 0, {kEq, kComp}, 0.3f, 0);
    audioTrack(sounds.impact, {2, 3, 4}, 0, {kEq}, 0.6f, 0);
    engine.idle();
    return song;
}

// --- Playing it ----------------------------------------------------------------------------------

struct Options {
    int bars = 32;
    int threads = 1;
    int buffer = 256;
    double warm = 8.0;
    double idle = 0.0;
    bool background = true;
};

struct Pass {
    std::string name;
    double backgroundCpu = 0.0;  // rendering in the background before it
    double renderCpu = 0.0, renderWall = 0.0, storeCpu = 0.0;
    double fromCache = 0.0;  // share of the strips' frames
    std::vector<float> out;
};

uint64_t sum(const sub::BackgroundFreezingStats& stats, bool cache) {
    return cache ? stats.framesFromCache : stats.framesLive + stats.framesFromCache;
}

Pass play(sub::Engine& engine, const std::string& name, int64_t frames, int buffer, bool background) {
    Pass pass;
    pass.name = name;
    sub::ManualBackend* device = sub::ManualBackend::current();
    if (!device) throw Stop("The Manual driver isn't open");
    engine.setPositionBeats(0.0);
    if (background) {
        // All it would render before the pass plays (the song and its warm-ups: never more than four times it).
        const double cpu = threadCpuSeconds();
        engine.renderInBackground(4 * frames);
        engine.serviceBackgroundFreezing();
        pass.backgroundCpu = threadCpuSeconds() - cpu;
    }
    const sub::BackgroundFreezingStats before = engine.backgroundFreezingStats();
    const auto buffers = (frames + buffer - 1) / buffer;
    for (int64_t b = 0; b < buffers; ++b) {
        const double cpu = threadCpuSeconds();
        const auto wall = Clock::now();
        device->process(1);
        pass.renderWall += std::chrono::duration<double>(Clock::now() - wall).count();
        const double served = threadCpuSeconds();
        pass.renderCpu += served - cpu;
        engine.serviceBackgroundFreezing();
        pass.storeCpu += threadCpuSeconds() - served;
        if (b % 16 == 15) engine.idle();
    }
    pass.out = device->takeOutput();
    const sub::BackgroundFreezingStats after = engine.backgroundFreezingStats();
    const auto all = static_cast<double>(sum(after, false) - sum(before, false));
    pass.fromCache = all > 0 ? static_cast<double>(sum(after, true) - sum(before, true)) / all : 0.0;
    return pass;
}

// The energy of a - b against b's, in dB (-inf: the same).
double differenceDb(const std::vector<float>& a, const std::vector<float>& b) {
    double diff = 0.0, signal = 0.0;
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
        diff += (static_cast<double>(a[i]) - b[i]) * (static_cast<double>(a[i]) - b[i]);
        signal += static_cast<double>(b[i]) * b[i];
    }
    return diff == 0.0 ? -INFINITY : 10.0 * std::log10(diff / std::max(signal, 1e-30));
}

std::vector<Pass> run(const Options& options, const Sounds& sounds, bool freezing) {
    sub::Engine engine;
    if (options.threads > 1) engine.setAudioThreads(options.threads);
    sub::BackgroundFreezingSettings settings;
    settings.enabled = freezing;
    settings.warmSeconds = options.warm;
    settings.idleSeconds = options.idle;
    settings.render = options.background;
    settings.renderThread = false;  // (play() renders in the background when it should, timed apart)
    engine.setBackgroundFreezing(settings);
    sub::DeviceConfig config;
    config.driver = sub::ManualBackend::kName;
    config.sampleRate = kRate;
    config.bufferFrames = static_cast<uint32_t>(options.buffer);
    engine.openDevice(config);
    const Song song = buildSong(engine, sounds, options.bars);
    const int64_t frames = static_cast<int64_t>(options.bars * kBarBeats * kBeatSec * kRate);

    std::vector<Pass> passes;
    engine.play();
    const bool background = freezing && options.background;
    passes.push_back(play(engine, "first", frames, options.buffer, background));
    passes.push_back(play(engine, "second", frames, options.buffer, background));
    // One note of the lead moved in the third section (the chorus), if the song has one.
    std::vector<sub::NoteDesc> notes = song.leadNotes;
    if (!notes.empty()) notes.front().key += 2;
    engine.setTrackNotes(song.lead, notes);
    passes.push_back(play(engine, "note edit", frames, options.buffer, background));
    // A parameter of the kick's EQ: the kick, the drum group and the master change throughout.
    const int param = 2;
    engine.setProcessorParam(song.kickEq, param, engine.processorParam(song.kickEq, param) * 0.9f + 0.05f);
    passes.push_back(play(engine, "param edit", frames, options.buffer, background));
    engine.stop();
    if (freezing) {
        const sub::BackgroundFreezingStats stats = engine.backgroundFreezingStats();
        std::printf("  cache: %zu blocks (%zu silent), %.1f MB; %zu shadows\n", stats.blocks, stats.silentBlocks,
                    static_cast<double>(stats.bytes) / (1024.0 * 1024.0), stats.shadows);
        std::printf("  background: %.1f s kept (strips' seconds), %.1f s of it rendered with devices running, "
                    "%.1f s kept from what a change had rung out of\n",
                    static_cast<double>(stats.framesRendered) / kRate,
                    static_cast<double>(stats.framesRenderedLive) / kRate,
                    static_cast<double>(stats.framesReplayed) / kRate);
    }
    engine.closeDevice();
    engine.idle(true);
    return passes;
}

int parseInt(const char* text) { return std::atoi(text); }

}  // namespace

int main(int argc, char** argv) {
    const char* const usage =
        "background_freeze_bench [--bars 32] [--threads 1] [--buffer 256] [--warm 8] [--idle 0] [--background 1]\n";
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool valued = arg == "--bars" || arg == "--threads" || arg == "--buffer" || arg == "--warm" ||
                            arg == "--idle" || arg == "--background";
        if (!valued) {
            std::printf("%s", usage);
            return arg == "--help" ? 0 : 2;
        }
        if (i + 1 >= argc) {
            std::fprintf(stderr, "Missing a value after %s\n%s", arg.c_str(), usage);
            return 2;
        }
        const char* value = argv[++i];
        if (arg == "--bars") {
            options.bars = std::max(8, parseInt(value));
        } else if (arg == "--threads") {
            options.threads = std::max(1, parseInt(value));
        } else if (arg == "--buffer") {
            options.buffer = std::max(16, parseInt(value));
        } else if (arg == "--warm") {
            options.warm = std::atof(value);
        } else if (arg == "--background") {
            options.background = parseInt(value) != 0;
        } else {
            options.idle = std::atof(value);
        }
    }
    try {
        const TempFolder folder;
        const Sounds sounds = makeSounds(folder.path());
        const double seconds = options.bars * kBarBeats * kBeatSec;
        std::printf("%d bars (%.0f s), %d thread(s), %d-frame buffers, warm-up %.1f s, idle %.1f s, background %s\n",
                    options.bars, seconds, options.threads, options.buffer, options.warm, options.idle,
                    options.background ? "on" : "off");
        std::printf("off:\n");
        const std::vector<Pass> off = run(options, sounds, false);
        std::printf("on:\n");
        const std::vector<Pass> on = run(options, sounds, true);
        std::printf("\n%-11s %12s %12s %10s %12s %12s %10s %14s %12s\n", "pass", "off cpu s", "on cpu s", "saved",
                    "on wall s", "store cpu s", "cached", "vs off, dB", "bg cpu s");
        for (size_t p = 0; p < on.size(); ++p) {
            const Pass& a = off[p];
            const Pass& b = on[p];
            std::printf("%-11s %12.2f %12.2f %9.0f%% %12.2f %12.3f %9.0f%% %14.1f %12.2f\n", b.name.c_str(),
                        a.renderCpu, b.renderCpu, 100.0 * (1.0 - b.renderCpu / a.renderCpu), b.renderWall, b.storeCpu,
                        100.0 * b.fromCache, differenceDb(b.out, a.out), b.backgroundCpu);
        }
        std::printf("baseline: the second pass against the first, cache off: %.1f dB\n",
                    differenceDb(off[1].out, off[0].out));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
    return 0;
}
