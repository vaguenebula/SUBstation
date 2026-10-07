// sound_similarity_bench: the sound similarity's fingerprints on a real sample
// library: how fast they are made and searched, and how well the nearest
// sounds match what the query is (README.md).
//
//   sound_similarity_bench --folder <library> [--threads N] [--limit N]
//                          [--cache file] [--weights t,m,s,e,p,r] [--tune]
//                          [--show N] [--json file]
//
// The files' kinds come from their names and their folder's (a library sorted
// into Kicks, Snares... folders, or named so): kick, snare, clap, closed and
// open hi-hat, tom, cymbal, rim, shaker, snap, 808, and whether a file is a loop.
// Each labelled one-shot is a query over the whole library; precision@k is the
// share of its k nearest sounds (itself left out) that are one-shots of its
// kind. --tune searches the aspects' weights for the best mean precision@10.
// --cache keeps the fingerprints between runs (they are made again when the
// feature version changes).

#include <algorithm>
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
#include "similarity/Similarity.h"
#include "similarity/SoundFeatures.h"

namespace fs = std::filesystem;
using namespace sub::intelligence;

namespace {

struct Sound {
    std::string path;
    std::string label;  // "" if unknown
    bool loop = false;
    bool ok = false;
    Fingerprint fp{};
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
    const std::string text = lower(utf8(path.parent_path().filename()) + "/" + utf8(path.stem()));
    const auto words = tokens(text);
    auto word = [&](auto pred) { return std::any_of(words.begin(), words.end(), pred); };
    const bool loop = has(text, "loop") || word([](const std::string& w) {
        return w == "fill" || w == "fills" || w == "groove" || w == "break" || w == "breaks" || w == "top" ||
               w == "tops" || w == "bpm" || w == "roll" || w == "rolls";
    });
    const bool kick = has(text, "kick") || has(text, "kik") || word([](const std::string& w) { return w == "bd"; });
    const bool snare = has(text, "snare") || has(text, "snr");
    const bool clap = has(text, "clap");
    const bool hat = has(text, "hat") || has(text, "hihat") ||
                     word([](const std::string& w) { return w == "hh" || w == "hhc" || w == "hho" || w == "chh" || w == "ohh"; });
    const bool open = has(text, "open") || word([](const std::string& w) { return w == "ohh" || w == "hho" || w == "oh"; });
    std::string label;
    if (has(text, "808") && !kick && !snare && !clap && !hat) label = "808";
    else if (hat) label = open ? "open hat" : "closed hat";
    else if (kick) label = "kick";
    else if (snare) label = "snare";
    else if (clap) label = "clap";
    else if (word([](const std::string& w) { return w == "rim" || w == "rims" || w == "rimshot" || w == "rimshots"; }))
        label = "rim";
    else if (word([](const std::string& w) { return w.rfind("tom", 0) == 0 && w.size() <= 6 && w != "tomorrow"; })) label = "tom";
    else if (has(text, "crash") || has(text, "ride") || has(text, "cymbal")) label = "cymbal";
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

// The fingerprint cache: "SIMBENCH", version, then (path, ok, floats) per file.
void saveCache(const std::string& file, const std::vector<Sound>& sounds) {
    std::ofstream out(pathOf(file), std::ios::binary);
    out.write("SIMBENCH", 8);
    const uint32_t version = kFeatureVersion, dims = kDims;
    out.write(reinterpret_cast<const char*>(&version), 4);
    out.write(reinterpret_cast<const char*>(&dims), 4);
    for (const Sound& s : sounds) {
        const auto len = static_cast<uint32_t>(s.path.size());
        out.write(reinterpret_cast<const char*>(&len), 4);
        out.write(s.path.data(), len);
        const char ok = s.ok;
        out.write(&ok, 1);
        out.write(reinterpret_cast<const char*>(s.fp.data()), sizeof(float) * kDims);
    }
}

std::map<std::string, std::pair<bool, Fingerprint>> loadCache(const std::string& file) {
    std::map<std::string, std::pair<bool, Fingerprint>> out;
    std::ifstream in(pathOf(file), std::ios::binary);
    char magic[8];
    uint32_t version = 0, dims = 0;
    if (!in.read(magic, 8) || std::memcmp(magic, "SIMBENCH", 8) != 0) return out;
    in.read(reinterpret_cast<char*>(&version), 4);
    in.read(reinterpret_cast<char*>(&dims), 4);
    if (version != kFeatureVersion || dims != kDims) return out;
    for (;;) {
        uint32_t len = 0;
        if (!in.read(reinterpret_cast<char*>(&len), 4)) break;
        std::string path(len, '\0');
        char ok = 0;
        Fingerprint fp{};
        if (!in.read(path.data(), len) || !in.read(&ok, 1) ||
            !in.read(reinterpret_cast<char*>(fp.data()), sizeof(float) * kDims))
            break;
        out[path] = {ok != 0, fp};
    }
    return out;
}

struct Score {
    std::map<std::string, std::pair<int, std::array<double, 3>>> byLabel;  // queries, sum of P@1, P@5, P@10
    double macro10 = 0.0, micro10 = 0.0, macro1 = 0.0;
};

Score evaluate(const std::vector<Sound>& sounds, const std::vector<float>& matrix, const std::vector<uint32_t>& rows,
               const std::vector<uint32_t>& queries, const AspectWeights& weights) {
    const Comparison comparison = Comparison::fit(matrix.data(), rows.size(), nullptr, weights);
    Score score;
    std::vector<std::pair<float, uint32_t>> nearest;
    double all10 = 0.0;
    for (const uint32_t q : queries) {
        const float* query = matrix.data() + static_cast<size_t>(q) * kDims;
        nearest.clear();
        for (uint32_t r = 0; r < rows.size(); ++r)
            if (r != q) nearest.push_back({comparison.distance(query, matrix.data() + static_cast<size_t>(r) * kDims), r});
        const size_t k = std::min<size_t>(10, nearest.size());
        std::partial_sort(nearest.begin(), nearest.begin() + static_cast<ptrdiff_t>(k), nearest.end());
        const Sound& s = sounds[rows[q]];
        int hits = 0;
        std::array<double, 3> p{};
        for (size_t i = 0; i < k; ++i) {
            const Sound& t = sounds[rows[nearest[i].second]];
            if (t.label == s.label && !t.loop) ++hits;
            if (i == 0) p[0] = hits;
            if (i == 4) p[1] = hits / 5.0;
            if (i == 9) p[2] = hits / 10.0;
        }
        auto& entry = score.byLabel[s.label];
        ++entry.first;
        for (int i = 0; i < 3; ++i) entry.second[i] += p[i];
        all10 += p[2];
    }
    for (const auto& [label, entry] : score.byLabel) {
        score.macro10 += entry.second[2] / entry.first;
        score.macro1 += entry.second[0] / entry.first;
    }
    if (!score.byLabel.empty()) {
        score.macro10 /= static_cast<double>(score.byLabel.size());
        score.macro1 /= static_cast<double>(score.byLabel.size());
    }
    score.micro10 = queries.empty() ? 0.0 : all10 / static_cast<double>(queries.size());
    return score;
}

void print(const Score& score) {
    std::printf("  %-12s %8s %7s %7s %7s\n", "kind", "queries", "P@1", "P@5", "P@10");
    for (const auto& [label, entry] : score.byLabel)
        std::printf("  %-12s %8d %7.3f %7.3f %7.3f\n", label.c_str(), entry.first, entry.second[0] / entry.first,
                    entry.second[1] / entry.first, entry.second[2] / entry.first);
    std::printf("  mean P@1 %.3f, mean P@10 %.3f (over kinds), P@10 over all queries %.3f\n", score.macro1,
                score.macro10, score.micro10);
}

std::string weightsText(const AspectWeights& w) {
    std::ostringstream out;
    for (size_t a = 0; a < kAspects; ++a) out << (a ? "," : "") << w.weight[a];
    return out.str();
}

}  // namespace

int main(int argc, char** argv) {
    std::string folder, cacheFile, jsonFile;
    unsigned threads = std::max(1u, std::thread::hardware_concurrency() / 2);
    size_t limit = 0;
    int show = 0;
    bool tune = false;
    AspectWeights weights;
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
        else if (arg == "--weights") {
            std::stringstream list(next());
            std::string item;
            for (size_t a = 0; a < kAspects && std::getline(list, item, ','); ++a) weights.weight[a] = std::stof(item);
        } else {
            std::cerr << "unknown argument " << arg << "\n";
            return 2;
        }
    }
    if (folder.empty()) {
        std::cerr << "usage: sound_similarity_bench --folder <sample library> [--threads N] [--limit N] [--cache file] "
                     "[--weights t,m,s,e,p,r] [--tune] [--show N] [--json file]\n";
        return 2;
    }

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
    const auto cached = cacheFile.empty() ? decltype(loadCache("")){} : loadCache(cacheFile);
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
            SoundAnalyzer analyzer;
            for (size_t i; (i = next++) < todo.size();) {
                Sound& s = sounds[todo[i]];
                const double t0 = now();
                try {
                    const auto fp = analyzer.analyzeFile(s.path);
                    if (fp) {
                        s.fp = *fp;
                        s.ok = true;
                    }
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
    if (!cacheFile.empty()) saveCache(cacheFile, sounds);

    std::vector<uint32_t> rows;
    for (uint32_t i = 0; i < sounds.size(); ++i)
        if (sounds[i].ok) rows.push_back(i);
    std::vector<float> matrix(rows.size() * kDims);
    for (size_t r = 0; r < rows.size(); ++r) std::copy(sounds[rows[r]].fp.begin(), sounds[rows[r]].fp.end(), matrix.begin() + r * kDims);
    std::vector<uint32_t> queries;
    std::map<std::string, int> kinds;
    for (uint32_t r = 0; r < rows.size(); ++r) {
        const Sound& s = sounds[rows[r]];
        if (!s.label.empty() && !s.loop) {
            queries.push_back(r);
            ++kinds[s.label];
        }
    }
    std::printf("%zu fingerprints; %zu labelled one-shots as queries\n", rows.size(), queries.size());

    // --- Search speed: every file against one query, as the index does ---
    {
        const Comparison comparison = Comparison::fit(matrix.data(), rows.size(), nullptr, weights);
        std::vector<float> d(rows.size());
        const double t0 = now();
        const int repeats = 20;
        for (int k = 0; k < repeats; ++k)
            for (size_t r = 0; r < rows.size(); ++r) d[r] = comparison.distance(matrix.data(), matrix.data() + r * kDims);
        std::printf("one search over %zu fingerprints: %.3f ms\n", rows.size(), 1000 * (now() - t0) / repeats);
    }

    // --- Retrieval ---
    std::printf("\nweights %s\n", weightsText(weights).c_str());
    const Score base = evaluate(sounds, matrix, rows, queries, weights);
    print(base);
    // Each aspect alone, for the record.
    for (size_t a = 0; a < kAspects; ++a) {
        AspectWeights only;
        only.weight.fill(0.f);
        only.weight[a] = 1.f;
        const Score s = evaluate(sounds, matrix, rows, queries, only);
        std::printf("  %-13s alone: mean P@10 %.3f\n", aspectName(static_cast<Aspect>(a)), s.macro10);
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
                    const double s = evaluate(sounds, matrix, rows, queries, trial).macro10;
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
        print(evaluate(sounds, matrix, rows, queries, best));
        weights = best;
    }

    if (show > 0) {
        const Comparison comparison = Comparison::fit(matrix.data(), rows.size(), nullptr, weights);
        std::set<std::string> shown;
        for (const uint32_t q : queries) {
            const Sound& s = sounds[rows[q]];
            if (shown.count(s.label) >= 1) continue;
            shown.insert(s.label);
            std::vector<std::pair<float, uint32_t>> nearest;
            for (uint32_t r = 0; r < rows.size(); ++r)
                nearest.push_back({comparison.distance(matrix.data() + q * kDims, matrix.data() + r * kDims), r});
            std::partial_sort(nearest.begin(), nearest.begin() + std::min<ptrdiff_t>(show + 1, static_cast<ptrdiff_t>(nearest.size())),
                              nearest.end());
            std::printf("\n%s (%s):\n", utf8(pathOf(s.path).filename()).c_str(), s.label.c_str());
            for (int i = 1; i <= show && i < static_cast<int>(nearest.size()); ++i) {
                const Sound& t = sounds[rows[nearest[i].second]];
                std::printf("  %3.0f%%  %-12s %s\n", 100 * Comparison::similarity(nearest[i].first),
                            (t.label + (t.loop ? " loop" : "")).c_str(), utf8(pathOf(t.path).filename()).c_str());
            }
        }
    }

    if (!jsonFile.empty()) {
        const Score s = evaluate(sounds, matrix, rows, queries, weights);
        std::ofstream out(pathOf(jsonFile));
        out << "{\"files\": " << sounds.size() << ", \"fingerprints\": " << rows.size() << ", \"queries\": " << queries.size()
            << ", \"weights\": [" << weightsText(weights) << "], \"meanP10\": " << s.macro10 << ", \"meanP1\": " << s.macro1
            << ", \"allP10\": " << s.micro10 << "}\n";
    }
    return 0;
}
