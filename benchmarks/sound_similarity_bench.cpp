// sound_similarity_bench: the sound similarity's fingerprints (Essentia's
// descriptors, EssentiaExtractor) on a real sample library: how fast they are
// made and searched, how fast the index analyses a library, and how well the
// nearest sounds match what the query is (README.md).
//
//   sound_similarity_bench --folder <library> [--threads N] [--limit N]
//                          [--cache file] [--weights t,m,s,e,p,r[,x]] [--tune]
//                          [--show N] [--split] [--index] [--json file]
//
// The files' kinds come from their names and their folder's (a library sorted
// into Kicks, Snares... folders, or named so; TidalCycles' Dirt-Samples'
// folder names, bd, sn, hh...): kick, snare, clap, closed and open hi-hat, tom,
// cymbal, rim, shaker, snap, 808; synth stab, pluck, synth bass, sub bass, arp;
// and whether a file is a loop. Each labelled one-shot is a query over the
// whole library; precision@k is the share of its k nearest sounds (itself left
// out) that are one-shots of its kind. Each loop is a query too, its hits
// other loops. --tune searches the aspects' weights for the best mean
// precision@10 over the one-shot kinds. --split halves the queries by their
// folder (a hash of its path): --tune then tunes on one half, and both halves
// are reported, so what the tuning gained is seen on sounds it didn't see.
// --cache keeps the fingerprints between
// runs (made again when the extractor's schema changes). --index also runs the
// SoundIndex itself over the library (its analysers, at background priority),
// from nothing and again from its saved fingerprints.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "core/AudioReader.h"
#include "similarity/FeatureExtractor.h"
#include "similarity/Similarity.h"
#include "similarity/SoundIndex.h"

namespace fs = std::filesystem;
using namespace sub::intelligence;

namespace {

struct Sound {
    std::string path;
    std::string label;  // "" if unknown
    bool loop = false;
    bool ok = false;
    std::vector<float> fp;
};

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> tokens(const std::string& text) {
    std::vector<std::string> out;
    std::string t;
    for (char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            t += c;
        } else if (!t.empty()) {
            out.push_back(t);
            t.clear();
        }
    }
    if (!t.empty()) out.push_back(t);
    return out;
}

// Paths as UTF-8 strings (what the index takes) and back.
std::string utf8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return {s.begin(), s.end()};
}
fs::path pathOf(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

bool has(const std::string& text, const char* what) { return text.find(what) != std::string::npos; }

// The kind of sound a file is, by its name and its folder's.
std::pair<std::string, bool> labelOf(const fs::path& path) {
    const std::string folder = lower(utf8(path.parent_path().filename()));
    const std::string text = folder + "/" + lower(utf8(path.stem()));
    const auto words = tokens(text);
    auto word = [&](auto pred) { return std::any_of(words.begin(), words.end(), pred); };
    auto is = [&](std::initializer_list<const char*> names) {
        return std::any_of(names.begin(), names.end(), [&](const char* n) { return folder == n; });
    };
    const bool loop = has(text, "loop") || has(folder, "breaks") || word([](const std::string& w) {
        return w == "fill" || w == "fills" || w == "groove" || w == "break" || w == "breaks" || w == "top" ||
               w == "tops" || w == "bpm" || w == "roll" || w == "rolls";
    });
    // Dirt-Samples' folders (github.com/tidalcycles/Dirt-Samples), by name.
    if (is({"bd", "808bd", "clubkick", "hardkick", "kicklinn", "popkick", "reverbkick"})) return {"kick", loop};
    if (is({"sn", "sd", "808sd"})) return {"snare", loop};
    if (is({"cp", "realclaps"})) return {"clap", loop};
    if (is({"hh", "hh27", "linnhats"})) return {"closed hat", loop};
    if (is({"oh", "ho", "808oh"})) return {"open hat", loop};
    if (is({"lt", "mt", "ht", "808lt", "808mt", "808ht"})) return {"tom", loop};
    if (is({"cr", "808cy"})) return {"cymbal", loop};
    if (is({"rm", "rs"})) return {"rim", loop};
    if (is({"stab"})) return {"synth stab", loop};
    if (is({"pluck"})) return {"pluck", loop};
    if (is({"bass", "bass3", "jvbass"})) return {"synth bass", loop};
    if (is({"jungbass"})) return {"sub bass", loop};
    if (is({"arpy"})) return {"arp", loop};
    if (is({"808lc", "808mc", "808hc"})) return {"", loop};  // (congas)
    const bool kit = is({"808", "909"});                     // (a drum machine's sounds, named by abbreviation)

    // Drum machines' file names abbreviate too (DR110CLP, Sd 180, CH 03, BD0000):
    // as a word, or a word's start followed by digits.
    const std::string stem = lower(utf8(path.stem()));
    const auto stemWords = tokens(stem);
    auto abbreviated = [&](std::initializer_list<const char*> names) {
        for (const std::string& w : stemWords)
            for (const char* n : names) {
                const size_t len = std::strlen(n);
                if (w.rfind(n, 0) == 0 &&
                    std::all_of(w.begin() + static_cast<ptrdiff_t>(len), w.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
                    return true;
            }
        return false;
    };
    const bool kick = has(text, "kick") || has(text, "kik") || word([](const std::string& w) { return w == "bd"; }) ||
                      abbreviated({"bd", "kck"});
    const bool snare = has(text, "snare") || has(text, "snr") || abbreviated({"sd", "sn"});
    const bool clap = has(text, "clap") || has(stem, "clp") || abbreviated({"cp"});
    const bool hat = has(text, "hat") || has(text, "hihat") ||
                     word([](const std::string& w) { return w == "hh" || w == "hhc" || w == "hho" || w == "chh" || w == "ohh"; }) ||
                     abbreviated({"hh", "ch", "oh", "chh", "ohh"});
    const bool open = has(text, "open") || word([](const std::string& w) { return w == "ohh" || w == "hho" || w == "oh"; }) ||
                      abbreviated({"oh", "ohh"});
    std::string label;
    if (has(text, "808") && !kit && !kick && !snare && !clap && !hat) label = "808";
    else if (hat) label = open ? "open hat" : "closed hat";
    else if (kick) label = "kick";
    else if (snare) label = "snare";
    else if (clap) label = "clap";
    else if (word([](const std::string& w) { return w == "rim" || w == "rims" || w == "rimshot" || w == "rimshots"; }) ||
             abbreviated({"rs", "rim"}))
        label = "rim";
    else if (word([](const std::string& w) { return w.rfind("tom", 0) == 0 && w.size() <= 6 && w != "tomorrow"; }) ||
             abbreviated({"lt", "mt", "ht"}))
        label = "tom";
    else if (has(text, "crash") || has(text, "ride") || has(text, "cymbal") || abbreviated({"cy", "cym", "cr"})) label = "cymbal";
    else if (has(text, "shaker")) label = "shaker";
    else if (has(text, "snap")) label = "snap";
    return {label, loop};
}

bool isAudio(const fs::path& p) {
    const std::string ext = lower(utf8(p.extension()));
    return ext == ".wav" || ext == ".wave" || ext == ".flac" || ext == ".mp3";
}

double now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The fingerprint cache: "SIMBENC2", the schema's key and dims, then (path, ok, floats) per file.
void saveCache(const std::string& file, const FeatureSchema& schema, const std::vector<Sound>& sounds) {
    std::ofstream out(pathOf(file), std::ios::binary);
    out.write("SIMBENC2", 8);
    const uint64_t key = schema.key();
    const auto dims = static_cast<uint32_t>(schema.dims());
    out.write(reinterpret_cast<const char*>(&key), 8);
    out.write(reinterpret_cast<const char*>(&dims), 4);
    for (const Sound& s : sounds) {
        const auto len = static_cast<uint32_t>(s.path.size());
        out.write(reinterpret_cast<const char*>(&len), 4);
        out.write(s.path.data(), len);
        const char ok = s.ok;
        out.write(&ok, 1);
        std::vector<float> fp = s.fp;
        fp.resize(dims, 0.f);
        out.write(reinterpret_cast<const char*>(fp.data()), static_cast<std::streamsize>(sizeof(float) * dims));
    }
}

std::map<std::string, std::pair<bool, std::vector<float>>> loadCache(const std::string& file, const FeatureSchema& schema) {
    std::map<std::string, std::pair<bool, std::vector<float>>> out;
    std::ifstream in(pathOf(file), std::ios::binary);
    char magic[8];
    uint64_t key = 0;
    uint32_t dims = 0;
    if (!in.read(magic, 8) || std::memcmp(magic, "SIMBENC2", 8) != 0) return out;
    in.read(reinterpret_cast<char*>(&key), 8);
    in.read(reinterpret_cast<char*>(&dims), 4);
    if (key != schema.key() || dims != schema.dims()) return out;
    for (;;) {
        uint32_t len = 0;
        if (!in.read(reinterpret_cast<char*>(&len), 4)) break;
        std::string path(len, '\0');
        char ok = 0;
        std::vector<float> fp(dims);
        if (!in.read(path.data(), len) || !in.read(&ok, 1) ||
            !in.read(reinterpret_cast<char*>(fp.data()), static_cast<std::streamsize>(sizeof(float) * dims)))
            break;
        out[path] = {ok != 0, std::move(fp)};
    }
    return out;
}

constexpr const char* kLoopKind = "loop";

struct Score {
    std::map<std::string, std::pair<int, std::array<double, 3>>> byLabel;  // queries, sum of P@1, P@5, P@10
    double macro10 = 0.0, micro10 = 0.0, macro1 = 0.0;  // over the one-shot kinds
    double loops10 = 0.0;                               // loops' P@10
};

Score evaluate(const FeatureSchema& schema, const std::vector<Sound>& sounds, const std::vector<float>& matrix,
               const std::vector<uint32_t>& rows, const std::vector<uint32_t>& queries, const AspectWeights& weights) {
    const size_t dims = schema.dims();
    const Comparison comparison = Comparison::fit(schema, matrix.data(), rows.size(), nullptr, weights);
    Score score;
    std::vector<std::pair<float, uint32_t>> nearest;
    double all10 = 0.0;
    int oneShots = 0;
    for (const uint32_t q : queries) {
        const float* query = matrix.data() + static_cast<size_t>(q) * dims;
        nearest.clear();
        for (uint32_t r = 0; r < rows.size(); ++r)
            if (r != q) nearest.push_back({comparison.distance(query, matrix.data() + static_cast<size_t>(r) * dims), r});
        const size_t k = std::min<size_t>(10, nearest.size());
        std::partial_sort(nearest.begin(), nearest.begin() + static_cast<ptrdiff_t>(k), nearest.end());
        const Sound& s = sounds[rows[q]];
        int hits = 0;
        std::array<double, 3> p{};
        for (size_t i = 0; i < k; ++i) {
            const Sound& t = sounds[rows[nearest[i].second]];
            if (s.loop ? t.loop : (t.label == s.label && !t.loop)) ++hits;
            if (i == 0) p[0] = hits;
            if (i == 4) p[1] = hits / 5.0;
            if (i == 9) p[2] = hits / 10.0;
        }
        auto& entry = score.byLabel[s.loop ? kLoopKind : s.label];
        ++entry.first;
        for (int i = 0; i < 3; ++i) entry.second[i] += p[i];
        if (!s.loop) {
            all10 += p[2];
            ++oneShots;
        }
    }
    int kinds = 0;
    for (const auto& [label, entry] : score.byLabel) {
        if (label == kLoopKind) {
            score.loops10 = entry.second[2] / entry.first;
            continue;
        }
        score.macro10 += entry.second[2] / entry.first;
        score.macro1 += entry.second[0] / entry.first;
        ++kinds;
    }
    if (kinds) {
        score.macro10 /= kinds;
        score.macro1 /= kinds;
    }
    score.micro10 = oneShots ? all10 / oneShots : 0.0;
    return score;
}

void print(const Score& score) {
    std::printf("  %-12s %8s %7s %7s %7s\n", "kind", "queries", "P@1", "P@5", "P@10");
    for (const auto& [label, entry] : score.byLabel)
        std::printf("  %-12s %8d %7.3f %7.3f %7.3f\n", label.c_str(), entry.first, entry.second[0] / entry.first,
                    entry.second[1] / entry.first, entry.second[2] / entry.first);
    std::printf("  one-shots: mean P@1 %.3f, mean P@10 %.3f (over kinds), P@10 over all queries %.3f; loops: P@10 %.3f\n",
                score.macro1, score.macro10, score.micro10, score.loops10);
}

std::string weightsText(const AspectWeights& w) {
    std::ostringstream out;
    for (size_t a = 0; a < kAspects; ++a) out << (a ? "," : "") << w.weight[a];
    return out.str();
}

// The index itself over the library: from nothing, then from what it saved.
void benchIndex(const std::vector<Sound>& sounds, unsigned threads) {
    const std::string store = utf8(fs::temp_directory_path() / "sound_similarity_bench-index.bin");
    std::error_code ignored;
    fs::remove(pathOf(store), ignored);
    auto library = std::make_shared<SoundIndex::Library>();
    for (const Sound& s : sounds) library->push_back(s.path);
    SoundIndexOptions o;
    o.store = store;
    o.threads = threads;
    {
        SoundIndex index(o);
        const double t0 = now();
        index.setLibrary(*library);
        if (!index.waitIdle(3600.0)) std::printf("index: timed out\n");
        const double elapsed = now() - t0;
        const SoundIndexStatus s = index.status();
        std::printf("\nindex, %u analysers at background priority: %llu files (%llu analysed, %llu not analysable) in %.1f s: "
                    "%.0f files/s, %.1f ms a file a thread\n",
                    threads, static_cast<unsigned long long>(s.library), static_cast<unsigned long long>(s.analysed),
                    static_cast<unsigned long long>(s.failed), elapsed, static_cast<double>(s.library) / elapsed,
                    1000.0 * elapsed * threads / static_cast<double>(std::max<uint64_t>(s.library, 1)));
        const double c0 = now();
        index.close();
        std::printf("index: closing (saving) %.0f ms, %.1f MB\n", 1000 * (now() - c0),
                    static_cast<double>(fs::file_size(pathOf(store), ignored)) / 1e6);
    }
    SoundIndex index(o);
    const double t0 = now();
    index.setLibrary(*library);
    index.waitIdle(3600.0);
    const SoundIndexStatus s = index.status();
    std::printf("index, next start: reading the saved fingerprints %.0f ms, every file checked against its stamp in %.0f ms "
                "(%llu analysed again)\n",
                s.loadMs, 1000 * (now() - t0), static_cast<unsigned long long>(s.analysedThisRun));
    const double f0 = now();
    index.find(SoundQuery{sounds.front().path});
    index.waitIdle(60.0);
    const auto result = index.take().result;
    std::printf("index: a search over %zu fingerprints %.2f ms (asked to taken %.2f ms)\n", result ? result->scored : 0,
                result ? result->searchMs : 0.0, 1000 * (now() - f0));
    index.close();
    fs::remove(pathOf(store), ignored);
}

}  // namespace

int main(int argc, char** argv) {
    std::string folder, cacheFile, jsonFile;
    unsigned threads = std::max(1u, std::thread::hardware_concurrency() / 2);
    size_t limit = 0;
    int show = 0;
    bool tune = false, indexToo = false, split = false;
    const ExtractorFactory factory = defaultExtractorFactory();
    const FeatureSchema schema = factory()->schema();
    const size_t dims = schema.dims();
    AspectWeights weights = schema.weights;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (arg == "--folder") folder = next();
        else if (arg == "--threads") threads = static_cast<unsigned>(std::stoul(next()));
        else if (arg == "--limit") limit = std::stoul(next());
        else if (arg == "--cache") cacheFile = next();
        else if (arg == "--json") jsonFile = next();
        else if (arg == "--show") show = std::stoi(next());
        else if (arg == "--tune") tune = true;
        else if (arg == "--index") indexToo = true;
        else if (arg == "--split") split = true;
        else if (arg == "--weights") {
            std::stringstream list(next());
            std::string item;
            weights.weight.fill(0.f);
            for (size_t a = 0; a < kAspects && std::getline(list, item, ','); ++a) weights.weight[a] = std::stof(item);
        } else {
            std::cerr << "unknown argument " << arg << "\n";
            return 2;
        }
    }
    if (folder.empty()) {
        std::cerr << "usage: sound_similarity_bench --folder <sample library> [--threads N] [--limit N] [--cache file] "
                     "[--weights t,m,s,e,p,r[,x]] [--tune] [--show N] [--split] [--index] [--json file]\n";
        return 2;
    }
    std::printf("extractor %s (version %u, %zu features): %s\n", schema.extractor.c_str(), schema.version, dims,
                schema.settings.c_str());

    // --- The library ---
    std::vector<Sound> sounds;
    for (auto it = fs::recursive_directory_iterator(pathOf(folder), fs::directory_options::skip_permission_denied);
         it != fs::recursive_directory_iterator(); ++it) {
        if (!it->is_regular_file() || !isAudio(it->path())) continue;
        Sound s;
        s.path = utf8(it->path());
        std::tie(s.label, s.loop) = labelOf(it->path());
        sounds.push_back(std::move(s));
        if (limit && sounds.size() >= limit) break;
    }
    std::sort(sounds.begin(), sounds.end(), [](const Sound& a, const Sound& b) { return a.path < b.path; });
    std::printf("%zu audio files under %s\n", sounds.size(), folder.c_str());

    // --- Fingerprints ---
    const auto cached = cacheFile.empty() ? decltype(loadCache("", schema)){} : loadCache(cacheFile, schema);
    std::vector<size_t> todo;
    for (size_t i = 0; i < sounds.size(); ++i) {
        const auto it = cached.find(sounds[i].path);
        if (it != cached.end()) std::tie(sounds[i].ok, sounds[i].fp) = it->second;
        else todo.push_back(i);
    }
    std::atomic<size_t> next{0}, failed{0};
    std::vector<double> perFile(todo.size(), 0.0);
    const double start = now();
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; ++t) {
        pool.emplace_back([&] {
            const std::unique_ptr<FeatureExtractor> extractor = factory();
            for (size_t i; (i = next++) < todo.size();) {
                Sound& s = sounds[todo[i]];
                const double t0 = now();
                try {
                    s.fp = extractor->extractFile(s.path);
                    s.ok = !s.fp.empty();
                } catch (const std::exception&) {
                    ++failed;
                }
                perFile[i] = now() - t0;
            }
        });
    }
    for (auto& t : pool) t.join();
    const double elapsed = now() - start;
    if (!todo.empty()) {
        std::vector<double> sorted = perFile;
        std::sort(sorted.begin(), sorted.end());
        std::printf("analysed %zu files on %u threads in %.1f s: %.1f files/s; per file median %.1f ms, 95%% %.1f ms, "
                    "max %.1f ms; %zu could not be read\n",
                    todo.size(), threads, elapsed, todo.size() / elapsed, 1000 * sorted[sorted.size() / 2],
                    1000 * sorted[sorted.size() * 95 / 100], 1000 * sorted.back(), failed.load());
    }
    if (!cacheFile.empty()) saveCache(cacheFile, schema, sounds);

    std::vector<uint32_t> rows;
    for (uint32_t i = 0; i < sounds.size(); ++i)
        if (sounds[i].ok) rows.push_back(i);
    std::vector<float> matrix(rows.size() * dims);
    for (size_t r = 0; r < rows.size(); ++r)
        std::copy(sounds[rows[r]].fp.begin(), sounds[rows[r]].fp.end(), matrix.begin() + static_cast<ptrdiff_t>(r * dims));
    std::vector<uint32_t> queries;
    size_t oneShotQueries = 0, loopQueries = 0;
    for (uint32_t r = 0; r < rows.size(); ++r) {
        const Sound& s = sounds[rows[r]];
        if (s.loop) {
            queries.push_back(r);
            ++loopQueries;
        } else if (!s.label.empty()) {
            queries.push_back(r);
            ++oneShotQueries;
        }
    }
    std::printf("%zu fingerprints; %zu labelled one-shots and %zu loops as queries\n", rows.size(), oneShotQueries, loopQueries);
    if (rows.empty()) return 1;
    // The halves: by a hash of the query's folder.
    std::vector<uint32_t> tuneQueries, heldOut;
    for (const uint32_t q : queries) {
        const std::string folderPath = utf8(pathOf(sounds[rows[q]].path).parent_path());
        uint64_t h = 1469598103934665603ull;
        for (const char c : folderPath) h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ull;
        (h % 2 == 0 ? tuneQueries : heldOut).push_back(q);
    }
    if (!split) tuneQueries = queries;
    auto report = [&](const char* title, const AspectWeights& w) {
        if (!split) {
            print(evaluate(schema, sounds, matrix, rows, queries, w));
            return;
        }
        std::printf("%s, on the half tuned on (%zu queries):\n", title, tuneQueries.size());
        print(evaluate(schema, sounds, matrix, rows, tuneQueries, w));
        std::printf("%s, held out (%zu queries):\n", title, heldOut.size());
        print(evaluate(schema, sounds, matrix, rows, heldOut, w));
    };

    // --- Search speed: every file against one query, as the index does ---
    {
        const Comparison comparison = Comparison::fit(schema, matrix.data(), rows.size(), nullptr, weights);
        std::vector<float> d(rows.size());
        const double t0 = now();
        const int repeats = 20;
        for (int k = 0; k < repeats; ++k)
            for (size_t r = 0; r < rows.size(); ++r) d[r] = comparison.distance(matrix.data(), matrix.data() + r * dims);
        std::printf("one search over %zu fingerprints: %.3f ms\n", rows.size(), 1000 * (now() - t0) / repeats);
        const double s0 = now();
        const FeatureStatistics statistics = FeatureStatistics::measure(schema, matrix.data(), rows.size());
        std::printf("measuring the library's statistics (%llu fingerprints): %.2f ms\n",
                    static_cast<unsigned long long>(statistics.count), 1000 * (now() - s0));
    }

    // --- Retrieval ---
    std::printf("\nweights %s\n", weightsText(weights).c_str());
    const Score base = evaluate(schema, sounds, matrix, rows, tuneQueries, weights);
    report("weights as they are", weights);
    // Each aspect alone, for the record.
    for (size_t a = 0; a < kAspects; ++a) {
        if (std::none_of(schema.features.begin(), schema.features.end(),
                         [a](const FeatureInfo& f) { return static_cast<size_t>(f.aspect) == a; }))
            continue;
        const Score s = evaluate(schema, sounds, matrix, rows, tuneQueries, AspectWeights::only(static_cast<Aspect>(a)));
        std::printf("  %-13s alone: mean P@10 %.3f, loops %.3f\n", aspectName(static_cast<Aspect>(a)), s.macro10, s.loops10);
    }

    if (tune) {
        AspectWeights best = weights;
        double bestScore = base.macro10;
        const float steps[] = {0.f, 0.25f, 0.5f, 0.75f, 1.f, 1.5f, 2.f, 3.f};
        for (int pass = 0; pass < 3; ++pass) {
            bool improved = false;
            for (size_t a = 0; a < kAspects; ++a) {
                for (const float v : steps) {
                    AspectWeights trial = best;
                    trial.weight[a] = v;
                    const double s = evaluate(schema, sounds, matrix, rows, tuneQueries, trial).macro10;
                    if (s > bestScore + 1e-4) {
                        bestScore = s;
                        best = trial;
                        improved = true;
                    }
                }
            }
            std::printf("tune pass %d: %s -> mean P@10 %.3f\n", pass + 1, weightsText(best).c_str(), bestScore);
            if (!improved) break;
        }
        std::printf("\ntuned weights %s\n", weightsText(best).c_str());
        report("tuned", best);
        weights = best;
    }

    if (show > 0) {
        const Comparison comparison = Comparison::fit(schema, matrix.data(), rows.size(), nullptr, weights);
        std::set<std::string> shown;
        for (const uint32_t q : queries) {
            const Sound& s = sounds[rows[q]];
            const std::string kind = s.loop ? kLoopKind : s.label;
            if (shown.count(kind) >= 1) continue;
            shown.insert(kind);
            std::vector<std::pair<float, uint32_t>> nearest;
            for (uint32_t r = 0; r < rows.size(); ++r)
                nearest.push_back({comparison.distance(matrix.data() + q * dims, matrix.data() + r * dims), r});
            std::partial_sort(nearest.begin(), nearest.begin() + std::min<ptrdiff_t>(show + 1, static_cast<ptrdiff_t>(nearest.size())),
                              nearest.end());
            std::printf("\n%s (%s):\n", utf8(pathOf(s.path).filename()).c_str(), kind.c_str());
            for (int i = 1; i <= show && i < static_cast<int>(nearest.size()); ++i) {
                const Sound& t = sounds[rows[nearest[i].second]];
                std::printf("  %3.0f%%  %-12s %s\n", 100 * Comparison::similarity(nearest[i].first),
                            (t.label + (t.loop ? " loop" : "")).c_str(), utf8(pathOf(t.path).filename()).c_str());
            }
        }
    }

    if (indexToo) benchIndex(sounds, threads);

    if (!jsonFile.empty()) {
        const Score s = evaluate(schema, sounds, matrix, rows, queries, weights);
        std::ofstream out(pathOf(jsonFile));
        out << "{\"extractor\": \"" << schema.extractor << "\", \"files\": " << sounds.size()
            << ", \"fingerprints\": " << rows.size() << ", \"queries\": " << queries.size() << ", \"weights\": ["
            << weightsText(weights) << "], \"meanP10\": " << s.macro10 << ", \"meanP1\": " << s.macro1
            << ", \"allP10\": " << s.micro10 << ", \"loopsP10\": " << s.loops10 << "}\n";
    }
    return 0;
}
