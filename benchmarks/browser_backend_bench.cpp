// The browser's backend alone, on a large library: the native index and search
// (sub_browser, as the application drives it) timed, and every query's results
// compared, item by item and in order, with the reference's: the Python index
// and search from before the native backend, ported for the tests
// (tests/app/support/BrowserReference.h). Fails if any query differs.
//
//     browser_backend_bench [--size 200000] [--used 500] [--audio] [--json out.json]
//
// Index     building it (the reference's walk; the native scan with a new index
//           file), and starting again with the saved one (native only)
// Query     the time a search takes: the reference's on this thread; the native
//           backend's on its own thread, and from asking until the results can be
//           taken on this one; and making the first page of rows from them
// Indexing  native queries while a full rescan runs
// With --audio the engine plays (silently) through the default output meanwhile,
// as browser_ui_bench.py does: its CPU load, and how far its playhead fell behind
// the wall clock at worst (a late audio callback leaves it behind for good).
//
// The reference is C++ now, so its times are not the Python times the README
// compares with: it is there to check the results.

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "Browser.h"
#include "BrowserReference.h"
#include "Engine.h"
#include "Fixtures.h"
#include "Json.h"
#include "LibraryGen.h"
#include "PyRandom.h"
#include "Signal.h"
#include "browser/BrowserSearch.h"
#include "browser/FileIndex.h"
#include "browser/Library.h"
#include "browser/PathKeys.h"

namespace {

using sub::app::BrowserItem;
using sub::app::FileIndex;
using sub::app::Library;
using sub::app::SearchResult;
using sub::bench::format;
using sub::bench::Json;
using sub::bench::rounded;
namespace backend = sub::browser;
namespace reference = sub::app::test::reference;
using Clock = std::chrono::steady_clock;

constexpr double kNow = 1'000'000'000.0;
const std::vector<QString> kQueries{QString(),           QStringLiteral("kick"),      QStringLiteral("e"),
                                    QStringLiteral("808 bass"), QStringLiteral("kick deep"), QStringLiteral("zzqx")};

double msSince(Clock::time_point start) { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); }

int wholeMs(double ms) { return static_cast<int>(std::lround(ms)); }

// Python's repr() of a query, as the report shows it.
std::string quoted(const QString& text) { return "'" + text.toStdString() + "'"; }

std::string describe(const BrowserItem& item) {
    return QStringLiteral("%1 | %2 | %3 | %4").arg(item.name, item.path, kindName(item.kind), item.detail).toStdString();
}

// Where two lists of items first differ (empty if they don't).
std::string firstDifference(const std::vector<BrowserItem>& native, const std::vector<BrowserItem>& expected) {
    for (size_t i = 0; i < std::min(native.size(), expected.size()); ++i) {
        const std::string a = describe(native[i]), b = describe(expected[i]);
        if (a != b) return format("row %zu: native \"%s\", reference \"%s\"", i, a.c_str(), b.c_str());
    }
    if (native.size() != expected.size())
        return format("%zu rows, the reference %zu", native.size(), expected.size());
    return {};
}

backend::Limits limits() {
    backend::Limits l;
    l.maxFiles = FileIndex::kMaxFiles;
    l.maxDepth = FileIndex::kMaxDepth;
    for (const QString& extension : FileIndex::audioExtensions()) l.extensions.push_back(extension.toStdString());
    return l;
}

// The backend as the application starts it: the index saved in `store`, the library as its place.
std::unique_ptr<backend::Browser> openNative(const QString& store, const QString& root) {
    auto browser = std::make_unique<backend::Browser>(sub::app::toBackendPath(store), limits());
    browser->setPlaces({sub::app::placeSpec(root)});
    return browser;
}

// Until the index settled: when the first files showed, and when it was done.
std::pair<double, double> settle(backend::Browser& browser) {
    const auto start = Clock::now();
    std::optional<double> first;
    for (;;) {
        const backend::IndexStatus status = browser.status();
        if (!status.busy && status.version) break;
        if (!first && status.files) first = msSince(start);
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    const double wall = msSince(start);
    return {first.value_or(wall), wall};
}

// What the index holds and how long its work took.
Json indexReport(double firstMs, double wallMs, const backend::IndexStatus& status) {
    Json json = Json::object();
    json.set("first_files_ms", wholeMs(firstMs)).set("wall_ms", wholeMs(wallMs));
    json.set("files", status.files).set("folders", status.folders);
    json.set("load_ms", status.loadMs).set("build_ms", status.buildMs).set("pass_ms", status.passMs);
    json.set("listed", status.listed).set("checked", status.checked);
    return json;
}

// A search, from asking until its results can be taken here (polled every 0.2 ms).
std::pair<std::shared_ptr<const backend::Result>, double> nativeQuery(backend::Browser& browser, const QString& text,
                                                                      const QString& sort, const std::string& prefix = {}) {
    const auto start = Clock::now();
    backend::Query query;
    query.text = text.toStdString();
    query.sort = sort == QStringLiteral("name") ? backend::Sort::Name : backend::Sort::Rank;
    query.now = kNow;
    query.groups = {backend::kAudioGroup};
    query.placePrefix = prefix;
    browser.search(std::move(query));
    std::shared_ptr<const backend::Result> result;
    while (!(result = browser.take().result)) std::this_thread::sleep_for(std::chrono::microseconds(200));
    return {result, msSince(start)};
}

// The engine playing silently: its playhead against the wall clock, sampled on a
// thread of its own (see browser_ui_bench.py's Playback).
class Playback {
public:
    explicit Playback(const QString& folder) {
        start(folder);
        tempo_ = engine_.tempo();
        thread_ = std::thread([this] { run(); });
    }
    ~Playback() {
        if (thread_.joinable()) finishSampling();
    }
    Playback(const Playback&) = delete;
    Playback& operator=(const Playback&) = delete;

    // Stops it: the report, and a line for the summary.
    std::pair<Json, std::string> stop() {
        finishSampling();
        double lag = 0.0;
        std::optional<double> ahead;
        std::vector<double> loads;
        for (const Sample& sample : samples_) {
            const double offset = sample.played - sample.wall;
            ahead = ahead ? std::max(*ahead, offset) : offset;
            lag = std::max(lag, *ahead - offset);
            loads.push_back(sample.load);
        }
        std::sort(loads.begin(), loads.end());
        const double loadMax = loads.empty() ? 0.0 : loads.back();
        const size_t n = loads.size();
        const double loadMedian = n == 0 ? 0.0 : (n % 2 ? loads[n / 2] : (loads[n / 2 - 1] + loads[n / 2]) / 2.0);
        const sub::DeviceStatus status = engine_.deviceStatus();
        const double sampleRate = engine_.sampleRate();
        engine_.stop();
        engine_.closeDevice();
        const double bufferMs = rounded(1000.0 * status.bufferFrames / sampleRate, 1);
        Json json = Json::object();
        json.set("cpu_load_max", rounded(loadMax, 3)).set("cpu_load_median", rounded(loadMedian, 3));
        json.set("playhead_max_lag_ms", rounded(lag * 1000.0, 1)).set("device", status.name);
        json.set("buffer_ms", bufferMs);
        return {json, format("CPU load max %.3f, median %.3f; the playhead's worst lag %.1f ms; %s, %.1f ms buffer",
                             loadMax, loadMedian, lag * 1000.0, status.name.c_str(), bufferMs)};
    }

private:
    struct Sample {
        double wall, played, load;
    };

    // An arrangement of a few tones, played silently (master gain 0), from the
    // start and without looping, so the playhead follows the wall clock.
    void start(const QString& folder) {
        engine_.openDevice(sub::DeviceConfig{});
        engine_.setMasterGain(0.f);
        for (int i = 0; i < 8; ++i) {
            const std::string path = subtest::writeWav(reference::fsPath(folder + QStringLiteral("/tone%1.wav").arg(i)),
                                                       subtest::sine(110.0 * (i + 1), 4.0, 0.3), 1);
            engine_.loadSource(path);
            const uint32_t track = engine_.addTrack();
            const bool warp = i % 2 == 1;  // half of them time-stretched, the heavier path
            std::vector<sub::ClipDesc> clips;
            for (int beat = 0; beat < 4000; beat += 8) {
                sub::ClipDesc clip = subtest::clip(path, beat, 4.0, 0.0, 1.f);
                clip.warp = warp;
                clip.segmentBpm = warp ? 100.0 : 0.0;
                clips.push_back(clip);
            }
            engine_.setTrackClips(track, clips);
        }
        engine_.play();
    }

    void run() {
        const auto origin = Clock::now();
        std::unique_lock lock(mutex_);
        while (!stopped_) {
            if (wake_.wait_for(lock, std::chrono::milliseconds(20), [this] { return stopped_; })) break;
            const double wall = std::chrono::duration<double>(Clock::now() - origin).count();
            samples_.push_back({wall, engine_.positionBeats() * 60.0 / tempo_, engine_.cpuLoad()});
        }
    }

    void finishSampling() {
        {
            std::lock_guard lock(mutex_);
            stopped_ = true;
        }
        wake_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    sub::Engine engine_;
    double tempo_ = 120.0;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopped_ = false;
    std::vector<Sample> samples_;
    std::thread thread_;
};

struct Options {
    int size = 200000;
    int used = 500;  // items with use counts
    bool audio = false;
    std::string json;
};

struct Report {
    Json json = Json::object();
    std::vector<std::string> lines;  // what it prints
    bool allSame = true;
};

Report run(const Options& options) {
    const std::filesystem::path libraryRoot = sub::bench::makeLibrary(options.size);
    const QString root = reference::fromFs(libraryRoot);
    QTemporaryDir tmp(QDir::tempPath() + QStringLiteral("/sub-backend-bench-XXXXXX"));
    if (!tmp.isValid()) throw std::runtime_error("can't make a temporary folder");
    Report report;
    report.json.set("size", options.size).set("library", QDir::toNativeSeparators(root).toStdString());
    report.json.set("used_records", options.used).set("cpu_count", std::thread::hardware_concurrency());
    report.lines.push_back(format("%d files", options.size));
    std::unique_ptr<Playback> playback;
    if (options.audio) playback = std::make_unique<Playback>(tmp.path());

    // --- Index -------------------------------------------------------------------------
    auto start = Clock::now();
    const std::vector<BrowserItem> items = reference::indexPlaces({root});
    const double referenceMs = msSince(start);
    if (items.empty()) throw std::runtime_error("the library has no files");
    Json index = Json::object();
    index.set("reference", Json::object().set("wall_ms", wholeMs(referenceMs)).set("files", uint64_t{items.size()}));
    const QString store = tmp.filePath(QStringLiteral("index.bin"));

    std::unique_ptr<backend::Browser> browser = openNative(store, root);
    const auto [newFirst, newWall] = settle(*browser);
    index.set("native_new", indexReport(newFirst, newWall, browser->status()));
    browser->close();
    browser = openNative(store, root);
    const auto [restartFirst, restartWall] = settle(*browser);
    index.set("native_restart", indexReport(restartFirst, restartWall, browser->status()));
    report.json.set("index", index);
    report.lines.push_back(format("index: reference %d ms; native new %d ms (first files %d ms), restart %d ms (first files %d ms)",
                                  wholeMs(referenceMs), wholeMs(newWall), wholeMs(newFirst), wholeMs(restartWall),
                                  wholeMs(restartFirst)));

    // --- Query -------------------------------------------------------------------------
    Library library(tmp.filePath(QStringLiteral("library.json")), [] { return kNow; });
    if (static_cast<size_t>(options.used) > items.size()) throw std::runtime_error("--used is more than the files");
    sub::bench::PyRandom rng(1);
    QStringList usedKeys;
    for (const BrowserItem& item : rng.sample(items, static_cast<size_t>(options.used))) usedKeys << item.key();
    library.recordUse(usedKeys);
    browser->setUsage(sub::app::usageRecords(library), Library::kHalfLifeDays);
    // A pack's folder: the folder of the folder of a file's folder.
    const QString place = QFileInfo(QFileInfo(QFileInfo(items[items.size() / 2].path).path()).path()).path();
    Json rows = Json::array();
    for (const QString& scope : {QStringLiteral("samples"), QStringLiteral("place")}) {
        const bool inPlace = scope == QStringLiteral("place");
        const std::string prefix = inPlace ? sub::app::placePrefix(place) : std::string();
        for (const QString& sort : {QStringLiteral("rank"), QStringLiteral("name")}) {
            for (const QString& text : kQueries) {
                start = Clock::now();  // as the panel did it, on the UI thread
                const std::vector<BrowserItem> expected =
                    reference::find(inPlace ? reference::placeItems(items, place) : items, text, library, sort);
                const double referenceQueryMs = msSince(start);
                const auto [result, endToEnd] = nativeQuery(*browser, text, sort, prefix);
                const SearchResult found(result, {});
                const auto pageStart = Clock::now();
                const std::vector<BrowserItem> page = found.items(0, 256);
                const double pageMs = msSince(pageStart);
                const std::string difference = firstDifference(found.items(0, found.total()), expected);
                const bool same = difference.empty();
                if (!same) {
                    report.allSame = false;
                    std::fprintf(stderr, "%s %s %s: %s\n", qPrintable(scope), qPrintable(sort), quoted(text).c_str(),
                                 difference.c_str());
                }
                Json row = Json::object();
                row.set("scope", scope.toStdString()).set("sort", sort.toStdString()).set("query", text.toStdString());
                row.set("results", found.total()).set("reference_ms", rounded(referenceQueryMs, 1));
                row.set("native_search_ms", rounded(result->searchMs, 2)).set("native_end_to_end_ms", rounded(endToEnd, 2));
                row.set("first_page_ms", rounded(pageMs, 2)).set("same_results", same);
                rows.push(row);
                report.lines.push_back(format("  %-7s %-4s %-11s %7d  reference %7.1f  native %6.2f / %6.2f ms  page %5.2f  same %s",
                                              qPrintable(scope), qPrintable(sort), quoted(text).c_str(), found.total(),
                                              rounded(referenceQueryMs, 1), rounded(result->searchMs, 2),
                                              rounded(endToEnd, 2), rounded(pageMs, 2), same ? "True" : "False"));
            }
        }
    }
    report.json.set("query", rows).set("all_results_same", report.allSame);

    // --- While indexing ----------------------------------------------------------------
    Json during = Json::array();
    std::string duringLine = "while indexing: ";
    browser->rescan();
    for (int round = 0, n = 0; round < 3 && browser->status().busy; ++round) {
        for (const QString& text : kQueries) {
            if (!browser->status().busy) break;
            const auto [result, endToEnd] = nativeQuery(*browser, text, QStringLiteral("rank"));
            Json row = Json::object();
            row.set("query", text.toStdString()).set("native_search_ms", rounded(result->searchMs, 2));
            row.set("native_end_to_end_ms", rounded(endToEnd, 2)).set("indexing", browser->status().busy);
            during.push(row);
            duringLine += (n++ ? ", " : "") + sub::bench::pythonFloat(rounded(endToEnd, 2));
        }
    }
    browser->waitIdle(60);
    report.json.set("indexing", during);
    report.lines.push_back(duringLine);
    browser->close();
    if (playback) {
        const auto [json, line] = playback->stop();
        report.json.set("playback", json);
        report.lines.push_back("playback: " + line);
    }
    return report;
}

int usage(const std::string& message) {
    if (!message.empty()) std::fprintf(stderr, "browser_backend_bench: %s\n", message.c_str());
    std::fprintf(stderr, "usage: browser_backend_bench [--size 200000] [--used 500] [--audio] [--json out.json]\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("SUBstation Benchmarks"));  // never the user's settings
    QCoreApplication::setApplicationName(QStringLiteral("SUBstation Benchmarks"));
    Options options;
    const QStringList args = QCoreApplication::arguments();
    for (int i = 1; i < args.size(); ++i) {
        QString name = args[i];
        std::optional<QString> value;  // --name=value
        if (name.startsWith(QStringLiteral("--")) && name.contains(QLatin1Char('='))) {
            value = name.section(QLatin1Char('='), 1);
            name = name.section(QLatin1Char('='), 0, 0);
        }
        const auto takeValue = [&]() -> std::optional<QString> {
            if (value) return value;
            if (i + 1 >= args.size()) return std::nullopt;
            return args[++i];
        };
        if (name == QStringLiteral("--help") || name == QStringLiteral("-h")) {
            usage({});
            return 0;
        } else if (name == QStringLiteral("--audio")) {
            options.audio = true;
        } else if (name == QStringLiteral("--size") || name == QStringLiteral("--used")) {
            const auto text = takeValue();
            bool ok = false;
            const int number = text ? text->toInt(&ok) : 0;
            if (!ok || number < 0) return usage(name.toStdString() + " takes a number");
            (name == QStringLiteral("--size") ? options.size : options.used) = number;
        } else if (name == QStringLiteral("--json")) {
            const auto text = takeValue();
            if (!text) return usage("--json takes a file");
            options.json = text->toStdString();
        } else {
            return usage("unknown argument " + args[i].toStdString());
        }
    }

    Report report;
    try {
        report = run(options);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "browser_backend_bench: %s\n", e.what());
        return 1;
    }
    for (const std::string& line : report.lines) std::printf("%s\n", line.c_str());
    std::fflush(stdout);
    if (!options.json.empty() && !report.json.save(reference::fsPath(QString::fromStdString(options.json)), 1)) {
        std::fprintf(stderr, "browser_backend_bench: can't write %s\n", options.json.c_str());
        return 1;
    }
    if (!report.allSame) {
        std::fprintf(stderr, "native results differ from the reference's\n");
        return 1;
    }
    return 0;
}
