// Parallel track processing: tracks rendered on one thread and on several.
//
//     parallel_render_bench [--tracks 32] [--device synth|ott|plugin]
//         [--plugin PATH --name NAME] [--heavy N [--heavy-otts 8]] [--compare-ordering]
//         [--seconds 20] [--threads 1,2,4,8] [--live 64,256] [--live-seconds 5] [--json out.json]
//
// Every track plays dense chords on the built-in Synth (with --device ott, through
// an OTT after it; with --device plugin, a noise clip and the notes through the
// named VST3 plug-in). With --heavy, the last N tracks (the last ones in routing
// order, so the last to start without cost ordering) go on through a chain of
// OTTs. --compare-ordering runs each thread count twice: the tracks with the most
// work first (the engine's default), and in routing order. Offline: the
// arrangement rendered as an export renders it (1024-frame chunks), best of
// three. Live (--live; needs the engine built with the ASIO SDK, and the tests,
// which build the fake ASIO driver): the fake ASIO driver (tests/asio_driver) in
// manual mode, its buffers processed back to back on this thread at the given
// sizes; the time a buffer takes, against how long it plays, is the audio
// thread's load (over 100% would be a dropout). Renders are checked to be the
// same on any number of threads.
//
// Qt-free: it links the engine alone, and borrows the engine tests' helpers
// (tests/engine/harness: WAV files, seeded random numbers, the fake driver).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "AsioDriver.h"
#include "Engine.h"
#include "Fixtures.h"
#include "Json.h"
#include "Signal.h"
#include "plugins/Vst3Format.h"

// The harness's failure report (its Main.cpp is the tests' own): the fake
// driver's hooks REQUIRE their functions, and a REQUIRE that fails stops the
// benchmark (subtest::Aborted).
namespace subtest {
void fail(const char* file, int line, const std::string& message) {
    std::fprintf(stderr, "%s:%d: %s\n", std::filesystem::path(file).filename().string().c_str(), line, message.c_str());
}
}  // namespace subtest

namespace {

using sub::bench::format;
using sub::bench::Json;
using Clock = std::chrono::steady_clock;

constexpr int kRate = 48000;
constexpr int kSpb = kRate / 2;  // samples per beat at 120 BPM

// What stops the benchmark, with a message for the user (Python's sys.exit(message)).
struct Stop : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Options {
    int tracks = 32;
    std::string device = "synth";  // synth, ott or plugin
    std::string plugin;            // a .vst3 file or bundle (--device plugin)
    std::string name;              // the plug-in's name in it (--device plugin)
    int heavy = 0;                 // this many tracks (the last) are heavy
    int heavyOtts = 8;             // the OTTs on a heavy track
    bool compareOrdering = false;  // also render with the tracks started in routing order
    double seconds = 20.0;         // offline: the length rendered
    std::vector<int> threads;
    std::vector<int> live;  // buffer sizes to play live through the fake ASIO driver
    double liveSeconds = 5.0;
    std::string json;  // write the results here too
};

// A folder of its own in the temporary folder, gone with it.
class TempFolder {
public:
    TempFolder() {
        std::random_device random;
        path_ = std::filesystem::temp_directory_path() / ("sub-parallel-bench-" + std::to_string(random()));
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

// Stereo noise at a quarter of full scale.
std::string noiseWav(const std::filesystem::path& folder, double seconds) {
    subtest::Rng rng(0);
    const auto frames = static_cast<size_t>(seconds * kRate);
    return subtest::writeWav(folder / "noise.wav", rng.uniformSamples(frames * 2, -0.25, 0.25), 2, kRate);
}

void build(sub::Engine& engine, const Options& options, double beats, const std::filesystem::path& folder) {
    subtest::Rng rng(1);
    std::string noise;
    std::string pluginUid;
    if (options.device == "plugin") {
        if (options.plugin.empty() || options.name.empty()) throw Stop("--device plugin needs --plugin PATH and --name NAME");
        for (const sub::PluginDescription& d : sub::vst3::Vst3Format::instance().scanFile(options.plugin))
            if (d.name == options.name) {
                pluginUid = d.uid;
                break;
            }
        if (pluginUid.empty()) throw Stop("No plug-in named '" + options.name + "' in " + options.plugin);
        noise = noiseWav(folder, beats / 2 + 1);
        engine.loadSource(noise);
    }
    for (int t = 0; t < options.tracks; ++t) {
        const uint32_t track = engine.addTrack();
        const uint32_t chain = engine.trackChain(track);
        const uint32_t synth = engine.addBuiltinProcessor(chain, "synth", -1);
        engine.setProcessorParam(synth, 0, static_cast<float>(t % 4));  // the waveforms in turn
        std::vector<sub::NoteDesc> notes;
        const auto bars = static_cast<int>(std::floor(beats / 4.0));
        for (int bar = 0; bar < bars; ++bar) {
            const auto root = static_cast<int>(rng.integers(36, 60));
            for (const int interval : {0, 4, 7, 11, 14, 17, 19, 24})  // eight voices at a time
                notes.push_back({bar * 4.0, 4.0, root + interval, 100});
        }
        engine.setTrackNotes(track, notes);
        if (options.device == "ott") {
            engine.addBuiltinProcessor(chain, "ott", -1);
        } else if (options.device == "plugin") {
            engine.setTrackClips(track, {subtest::clip(noise, 0.0, beats / 2)});
            engine.addPluginProcessor(chain, "VST3", options.plugin, pluginUid, -1);
        }
        if (t >= options.tracks - options.heavy)  // heavy tracks go on through their OTTs, after their device
            for (int i = 0; i < options.heavyOtts; ++i) engine.addBuiltinProcessor(chain, "ott", -1);
        engine.setTrackGain(track, 1.f / static_cast<float>(options.tracks));
    }
    engine.idle();
}

struct Run {
    int threads;
    std::string ordering;  // "cost" or "routing"
};

// (threads, ordering) to measure: one thread renders in order whatever the ordering.
std::vector<Run> runs(const std::vector<int>& threads, const std::vector<std::string>& orderings) {
    std::vector<Run> out;
    for (const int count : threads)
        for (size_t i = 0; i < (count > 1 ? orderings.size() : 1); ++i) out.push_back({count, orderings[i]});
    return out;
}

double secondsSince(Clock::time_point start) { return std::chrono::duration<double>(Clock::now() - start).count(); }

struct OfflineResult {
    int threads;
    std::string ordering;
    double seconds;
    double realtime;
};

std::vector<OfflineResult> benchOffline(sub::Engine& engine, const std::vector<int>& threads,
                                        const std::vector<std::string>& orderings, int64_t frames) {
    std::vector<OfflineResult> results;
    std::vector<float> reference;
    bool first = true;
    for (const Run& run : runs(threads, orderings)) {
        engine.setAudioThreads(run.threads);
        engine.setCostOrdering(run.ordering == "cost");
        std::vector<double> times;
        std::vector<float> out;
        for (int i = 0; i < 3; ++i) {
            const auto start = Clock::now();
            out = engine.renderOffline(0.0, frames);
            times.push_back(secondsSince(start));
        }
        if (first) {
            reference = std::move(out);
            first = false;
        } else if (out != reference) {
            throw Stop(format("The render on %d threads (%s order) differs from the one on %d", run.threads,
                              run.ordering.c_str(), threads.front()));
        }
        const double best = *std::min_element(times.begin(), times.end());
        results.push_back({run.threads, run.ordering, best, static_cast<double>(frames) / kRate / best});
    }
    engine.setCostOrdering(true);
    return results;
}

struct LiveResult {
    int buffer;
    int threads;
    std::string ordering;
    double meanUs, p99Us, maxUs;
    double load;
};

std::vector<LiveResult> benchLive(sub::Engine& engine, const std::vector<int>& threads,
                                  const std::vector<std::string>& orderings, int buffer, double seconds) {
    subtest::AsioDriver driver;  // (reset)
    driver.setManual(true);
    subtest::openAsio(engine, kRate, static_cast<uint32_t>(buffer));
    struct Restore {
        sub::Engine& engine;
        ~Restore() {
            engine.setCostOrdering(true);
            engine.closeDevice();
        }
    } restore{engine};
    std::vector<LiveResult> results;
    const int count = std::max(1, static_cast<int>(seconds * kRate / buffer));
    for (const Run& run : runs(threads, orderings)) {
        engine.setAudioThreads(run.threads);
        engine.setCostOrdering(run.ordering == "cost");
        engine.setPositionBeats(0.0);
        engine.play();
        driver.process(8);  // (the workers wake up)
        std::vector<double> perBuffer;
        perBuffer.reserve(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i) {
            const auto start = Clock::now();
            driver.process(1);
            perBuffer.push_back(secondsSince(start));
            if (i % 256 == 0) driver.clearOutput();
        }
        engine.stop();
        driver.process(2);
        const double budget = static_cast<double>(buffer) / kRate;
        std::sort(perBuffer.begin(), perBuffer.end());
        const double mean = std::accumulate(perBuffer.begin(), perBuffer.end(), 0.0) / static_cast<double>(perBuffer.size());
        results.push_back({buffer, run.threads, run.ordering, mean * 1e6,
                           perBuffer[static_cast<size_t>(static_cast<double>(perBuffer.size()) * 0.99)] * 1e6,
                           perBuffer.back() * 1e6, mean / budget});
    }
    return results;
}

// --- Arguments, as argparse took them -------------------------------------------------------

const char* const kUsage =
    "usage: parallel_render_bench [--tracks 32] [--device synth|ott|plugin] [--plugin PATH --name NAME]\n"
    "           [--heavy N [--heavy-otts 8]] [--compare-ordering] [--seconds 20] [--threads 1,2,4,8]\n"
    "           [--live 64,256] [--live-seconds 5] [--json out.json]\n"
    "\n"
    "Every track plays dense chords on the built-in Synth (with --device ott, through an OTT after\n"
    "it; with --device plugin, a noise clip and the notes through the named VST3 plug-in). With\n"
    "--heavy, the last N tracks go on through a chain of --heavy-otts OTTs. --compare-ordering also\n"
    "renders each thread count with the tracks started in routing order. Offline: rendered as an\n"
    "export renders it, best of three, on each number of --threads; the renders must be the same.\n"
    "Live (--live; needs ASIO and the fake ASIO driver the tests build): the driver's buffers\n"
    "processed back to back at the given sizes, for --live-seconds each.\n";

int parseInt(const std::string& name, const std::string& text) {
    try {
        size_t used = 0;
        const int value = std::stoi(text, &used);
        if (used == text.size()) return value;
    } catch (const std::exception&) {
    }
    throw Stop("argument " + name + ": invalid int value: '" + text + "'");
}

double parseDouble(const std::string& name, const std::string& text) {
    try {
        size_t used = 0;
        const double value = std::stod(text, &used);
        if (used == text.size()) return value;
    } catch (const std::exception&) {
    }
    throw Stop("argument " + name + ": invalid float value: '" + text + "'");
}

// "1,2,4": the numbers (empty parts skipped).
std::vector<int> parseList(const std::string& name, const std::string& text) {
    std::vector<int> out;
    size_t at = 0;
    while (at <= text.size()) {
        const size_t comma = std::min(text.find(',', at), text.size());
        if (comma > at) out.push_back(parseInt(name, text.substr(at, comma - at)));
        at = comma + 1;
    }
    return out;
}

Options parseArguments(int argc, char** argv) {
    Options options;
    std::vector<int> defaultThreads{1, 2, 4, sub::Engine::defaultAudioThreads()};
    std::sort(defaultThreads.begin(), defaultThreads.end());
    defaultThreads.erase(std::unique(defaultThreads.begin(), defaultThreads.end()), defaultThreads.end());
    options.threads = defaultThreads;
    for (int i = 1; i < argc; ++i) {
        std::string name = argv[i];
        std::string value;
        bool inlineValue = false;
        if (name.starts_with("--") && name.find('=') != std::string::npos) {  // --name=value
            value = name.substr(name.find('=') + 1);
            name = name.substr(0, name.find('='));
            inlineValue = true;
        }
        const auto takeValue = [&]() -> std::string {
            if (inlineValue) return value;
            if (i + 1 >= argc) throw Stop("argument " + name + ": expected one argument");
            return argv[++i];
        };
        if (name == "--help" || name == "-h") {
            std::printf("%s", kUsage);
            std::exit(0);
        } else if (name == "--tracks") {
            options.tracks = parseInt(name, takeValue());
        } else if (name == "--device") {
            options.device = takeValue();
            if (options.device != "synth" && options.device != "ott" && options.device != "plugin")
                throw Stop("argument --device: invalid choice: '" + options.device + "' (choose from 'synth', 'ott', 'plugin')");
        } else if (name == "--plugin") {
            options.plugin = takeValue();
        } else if (name == "--name") {
            options.name = takeValue();
        } else if (name == "--heavy") {
            options.heavy = parseInt(name, takeValue());
        } else if (name == "--heavy-otts") {
            options.heavyOtts = parseInt(name, takeValue());
        } else if (name == "--compare-ordering") {
            options.compareOrdering = true;
        } else if (name == "--seconds") {
            options.seconds = parseDouble(name, takeValue());
        } else if (name == "--threads") {
            options.threads = parseList(name, takeValue());
        } else if (name == "--live") {
            options.live = parseList(name, takeValue());
        } else if (name == "--live-seconds") {
            options.liveSeconds = parseDouble(name, takeValue());
        } else if (name == "--json") {
            options.json = takeValue();
        } else {
            throw Stop("unrecognized arguments: " + std::string(argv[i]));
        }
    }
    if (options.threads.empty()) throw Stop("argument --threads: no thread counts");
    return options;
}

int run(const Options& options) {
    const std::vector<std::string> orderings =
        options.compareOrdering ? std::vector<std::string>{"cost", "routing"} : std::vector<std::string>{"cost"};
    const unsigned cores = std::thread::hardware_concurrency();
    TempFolder folder;
    sub::Engine engine;
    const double beats = options.seconds * kRate / kSpb;
    build(engine, options, std::max(beats, 8.0), folder.path());
    Json report = Json::object();
    report.set("tracks", options.tracks).set("device", options.device).set("heavy", options.heavy);
    report.set("heavy_otts", options.heavyOtts).set("cores", cores);

    const std::string heavy =
        options.heavy ? format(", %d of them with %d OTTs", options.heavy, options.heavyOtts) : std::string();
    std::printf("%d tracks (%s%s), %u logical cores; offline, %g s:\n", options.tracks, options.device.c_str(),
                heavy.c_str(), cores, options.seconds);
    std::printf("%8s %8s %9s %11s %9s\n", "threads", "order", "time", "x realtime", "speed-up");
    std::fflush(stdout);
    const std::vector<OfflineResult> offline =
        benchOffline(engine, options.threads, orderings, static_cast<int64_t>(options.seconds * kRate));
    Json offlineJson = Json::array();
    const double serial = offline.front().seconds;
    for (const OfflineResult& r : offline) {
        std::printf("%8d %8s %8.3fs %10.1fx %8.2fx\n", r.threads, r.ordering.c_str(), r.seconds, r.realtime,
                    serial / r.seconds);
        offlineJson.push(Json::object()
                             .set("threads", r.threads)
                             .set("ordering", r.ordering)
                             .set("seconds", r.seconds)
                             .set("realtime", r.realtime));
    }
    report.set("offline", offlineJson);

    Json liveJson = Json::array();
    if (!options.live.empty()) {
        if (!subtest::haveTestAsio()) {
            std::printf("\nLive: skipped (the engine was built without the ASIO SDK)\n");
        } else {
            std::printf("\nLive, %g s per run (time per buffer; load = time / buffer length):\n", options.liveSeconds);
            std::printf("%7s %8s %8s %9s %9s %9s %7s\n", "buffer", "threads", "order", "mean", "p99", "max", "load");
            std::fflush(stdout);
            for (const int buffer : options.live) {
                for (const LiveResult& r : benchLive(engine, options.threads, orderings, buffer, options.liveSeconds)) {
                    std::printf("%7d %8d %8s %7.0fus %7.0fus %7.0fus %5.0f%%\n", r.buffer, r.threads, r.ordering.c_str(),
                                r.meanUs, r.p99Us, r.maxUs, r.load * 100.0);
                    std::fflush(stdout);
                    liveJson.push(Json::object()
                                      .set("buffer", r.buffer)
                                      .set("threads", r.threads)
                                      .set("ordering", r.ordering)
                                      .set("mean_us", r.meanUs)
                                      .set("p99_us", r.p99Us)
                                      .set("max_us", r.maxUs)
                                      .set("load", r.load));
                }
            }
        }
    }
    report.set("live", liveJson);
    std::printf("\n(tracks rendered on workers: %llu)\n", static_cast<unsigned long long>(engine.nodesOnWorkers()));
    std::fflush(stdout);
    if (!options.json.empty() && !report.save(options.json, 2)) throw Stop("can't write " + options.json);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(parseArguments(argc, argv));
    } catch (const Stop& stop) {
        std::fflush(stdout);
        std::fprintf(stderr, "%s\n", stop.what());
    } catch (const subtest::Skipped& skipped) {
        std::fprintf(stderr, "skipped: %s\n", skipped.reason.c_str());
    } catch (const subtest::Aborted&) {
    } catch (const std::exception& e) {
        std::fflush(stdout);
        std::fprintf(stderr, "parallel_render_bench: %s\n", e.what());
    }
    return 1;
}
