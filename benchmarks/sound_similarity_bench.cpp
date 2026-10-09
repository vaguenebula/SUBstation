// sound_similarity_bench: the sound similarity's fingerprints on a real sample
// library: how fast they are made and searched, and how well the nearest
// sounds match what the query is (README.md).
//
//   sound_similarity_bench --folder <library> [--threads N] [--limit N]
//                          [--cache file] [--weights t,m,s,e,p,r] [--tune]
//                          [--show N] [--json file]
//                          [--triplets N file] [--seed N] [--ratings file]
//                          [--misses file] [--robustness N]
//
// The files' kinds come from their names and their folder's (a library sorted
// into Kicks, Snares... folders, or named so): kick, snare, clap, closed and
// open hi-hat, tom, cymbal, rim, shaker, snap, 808, and whether a file is a loop.
// Each labelled one-shot is a query over the whole library; precision@k is the
// share of its k nearest sounds (itself left out) that are one-shots of its
// kind. --tune searches the aspects' weights for the best mean precision@10.
// --cache keeps the fingerprints between runs (they are made again when the
// feature version changes).
//
// Weights from listening (README.md, Weights from listening): --triplets writes N triplets of sounds
// (A, B, C) to rate by ear with tools/similarity_rater (which of B and C is
// more like A?), picked where the aspects disagree about the answer; --ratings
// fits the weights to the answers and runs the rest with them; --misses lists
// the answers the default weights get wrong, to hear again. --robustness N
// checks that N one-shots, made quieter, padded with silence or resampled,
// are still nearest to themselves.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <numeric>
#include <optional>
#include <random>
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


// --- Weights from listening ---

// Each aspect's distance alone between two fingerprints (the mean of its
// features' clipped squared z-differences): the distance is their weighted mean.
using AspectDistances = std::array<float, kAspects>;

struct AspectComparisons {
    std::array<Comparison, kAspects> each;

    AspectComparisons(const std::vector<float>& matrix, size_t count) {
        for (size_t a = 0; a < kAspects; ++a) {
            AspectWeights only;
            only.weight.fill(0.f);
            only.weight[a] = 1.f;
            each[a] = Comparison::fit(matrix.data(), count, nullptr, only);
        }
    }
    AspectDistances operator()(const float* x, const float* y) const {
        AspectDistances d{};
        for (size_t a = 0; a < kAspects; ++a) d[a] = each[a].distance(x, y);
        return d;
    }
};

// Tab-separated lines, '#' lines left out.
std::vector<std::vector<std::string>> readTsv(const std::string& file) {
    std::vector<std::vector<std::string>> rows;
    std::ifstream in(pathOf(file));
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> fields;
        std::stringstream fieldsIn(line);
        for (std::string f; std::getline(fieldsIn, f, '\t');) fields.push_back(f);
        rows.push_back(std::move(fields));
    }
    return rows;
}

// Writes `count` triplets, one an anchor: A, and two of its 40 nearest one-shots
// (by `weights`), B and C, in a random order. Of 150 pairs it takes the one the
// aspects most disagree about (the aspects, weighed alike, pulling both ways as
// evenly as they can), which is what an answer tells the weights most about;
// one in seven is a random pair instead, so plain cases are in it too. One in
// twenty is asked again 20 to 100 questions later, B and C swapped: how often you answer it the
// same is how consistent the answers are.
void writeTriplets(const std::vector<Sound>& sounds, const std::vector<uint32_t>& rows, const std::vector<float>& matrix,
                   const AspectWeights& weights, size_t count, uint32_t seed, const std::string& file) {
    const Comparison comparison = Comparison::fit(matrix.data(), rows.size(), nullptr, weights);
    const AspectComparisons aspects(matrix, rows.size());
    auto fp = [&](uint32_t r) { return matrix.data() + static_cast<size_t>(r) * kDims; };
    // One-shots: not loops, at most 4 s long (quick to listen to).
    std::vector<uint32_t> shots;
    for (uint32_t r = 0; r < rows.size(); ++r)
        if (!sounds[rows[r]].loop && fp(r)[feature::Length] <= std::log10(4.f)) shots.push_back(r);
    std::mt19937 random(seed);
    std::shuffle(shots.begin(), shots.end(), random);
    const float sameSound = 0.02f;  // closer than this: the same sound, or as good as
    struct Triplet {
        uint32_t a, b, c;
        int repeatOf = -1;
    };
    std::vector<Triplet> triplets;
    std::vector<std::pair<float, uint32_t>> nearest;
    // Each one-shot an A in turn (again from the first if there are fewer than `count`).
    for (size_t next = 0; triplets.size() < count && next < shots.size() * 4; ++next) {
        const uint32_t a = shots[next % shots.size()];
        nearest.clear();
        for (const uint32_t r : shots) {
            const float d = comparison.distance(fp(a), fp(r));
            if (r != a && d >= sameSound) nearest.push_back({d, r});
        }
        const size_t k = std::min<size_t>(40, nearest.size());
        if (k < 2) continue;
        std::partial_sort(nearest.begin(), nearest.begin() + static_cast<ptrdiff_t>(k), nearest.end());
        std::uniform_int_distribution<size_t> pick(0, k - 1);
        const bool plain = random() % 7 == 0;
        float best = -1.f;
        Triplet t{a, 0, 0};
        for (int tries = 0; tries < 150; ++tries) {
            const uint32_t b = nearest[pick(random)].second, c = nearest[pick(random)].second;
            if (b == c || comparison.distance(fp(b), fp(c)) < sameSound) continue;
            const AspectDistances ab = aspects(fp(a), fp(b)), ac = aspects(fp(a), fp(c));
            float towardsB = 0.f, towardsC = 0.f;
            for (size_t i = 0; i < kAspects; ++i) (ac[i] > ab[i] ? towardsB : towardsC) += std::abs(ac[i] - ab[i]);
            const float disagreement = plain ? 1.f : std::min(towardsB, towardsC) / std::max(1e-6f, towardsB + towardsC);
            if (disagreement > best) {
                best = disagreement;
                t.b = b;
                t.c = c;
            }
            if (plain) break;
        }
        if (best >= 0.f) triplets.push_back(t);
    }
    // The repeats, each somewhere after its first time.
    const size_t firsts = triplets.size();
    for (size_t i = 0; i < firsts / 20; ++i) {
        const size_t of = std::uniform_int_distribution<size_t>(0, firsts - 1)(random);
        if (triplets[of].repeatOf >= 0) continue;
        Triplet again{triplets[of].a, triplets[of].c, triplets[of].b, static_cast<int>(of)};
        // Soon after (20 to 100 questions on), so a few hundred answers already have some.
        const size_t at = std::min(triplets.size(), std::uniform_int_distribution<size_t>(of + 20, of + 100)(random));
        triplets.insert(triplets.begin() + static_cast<ptrdiff_t>(at), again);
        for (Triplet& u : triplets)
            if (u.repeatOf >= static_cast<int>(at)) ++u.repeatOf;
    }
    // Whole paths: the rater opens them from wherever it runs.
    auto whole = [&](uint32_t r) { return utf8(fs::absolute(pathOf(sounds[rows[r]].path)).lexically_normal()); };
    std::ofstream out(pathOf(file), std::ios::binary);
    out << "# sound_similarity_bench triplets: which of B and C is more like A? (tools/similarity_rater)\n"
        << "# id\trepeat of\tA\tB\tC\n";
    for (size_t i = 0; i < triplets.size(); ++i) {
        const Triplet& t = triplets[i];
        bool swap = t.repeatOf < 0 && random() % 2;
        out << i << '\t' << (t.repeatOf >= 0 ? std::to_string(t.repeatOf) : "") << '\t' << whole(t.a) << '\t'
            << whole(swap ? t.c : t.b) << '\t' << whole(swap ? t.b : t.c) << '\n';
    }
    std::printf("%zu triplets (%zu asked again) written to %s\n", triplets.size(), triplets.size() - firsts, file.c_str());
}

// An answer: how much further C is than B from A in each aspect, and whether B
// was picked as the closer.
struct Answer {
    AspectDistances further{};
    bool pickedB = false;
    bool labelled = false;  // A is a one-shot of a known kind (a drum, by its name)
    std::string anchor, b, c;
    long ms = -1;  // how long the question took, if the rater says
};

// The chance B is picked: sigmoid(sum of u[a] * further[a]), u >= 0; u is the
// weights times how sure the answers are. The fit: the most likely u, pulled
// towards the default weights' shape by `pull` (a penalty on the part of u not
// along them: how sure isn't penalised, only how the aspects share it). With no
// pull it is the answers' alone, which with a few hundred close calls follows
// their noise; with a lot, the defaults. A little ridge besides keeps u finite
// when the answers never contradict an aspect (more than 0.01 biased a
// simulated rater's weights). Newton's method on the aspects not held at 0.
std::array<double, kAspects> fitAnswers(const std::vector<Answer>& answers, const std::vector<size_t>& use,
                                        double pull) {
    constexpr double ridge = 0.01;
    // The defaults' direction, of length 1.
    std::array<double, kAspects> d{};
    {
        const auto w = AspectWeights::defaults();
        double length = 0.0;
        for (size_t a = 0; a < kAspects; ++a) length += static_cast<double>(w[a]) * w[a];
        for (size_t a = 0; a < kAspects; ++a) d[a] = w[a] / std::sqrt(std::max(length, 1e-12));
    }
    auto off = [&](const std::array<double, kAspects>& v) {  // v less its part along the defaults
        double along = 0.0;
        for (size_t a = 0; a < kAspects; ++a) along += v[a] * d[a];
        std::array<double, kAspects> o;
        for (size_t a = 0; a < kAspects; ++a) o[a] = v[a] - along * d[a];
        return o;
    };
    std::array<double, kAspects> u;
    for (size_t a = 0; a < kAspects; ++a) u[a] = 0.1 * d[a] + 1e-3;
    auto loss = [&](const std::array<double, kAspects>& v) {
        double sum = 0.0;
        for (const size_t i : use) {
            double z = 0.0;
            for (size_t a = 0; a < kAspects; ++a) z += v[a] * answers[i].further[a];
            if (!answers[i].pickedB) z = -z;
            sum += z > 0 ? std::log1p(std::exp(-z)) : -z + std::log1p(std::exp(z));
        }
        const auto o = off(v);
        for (size_t a = 0; a < kAspects; ++a) sum += 0.5 * ridge * v[a] * v[a] + 0.5 * pull * o[a] * o[a];
        return sum;
    };
    double current = loss(u);
    for (int iteration = 0; iteration < 100; ++iteration) {
        std::array<double, kAspects> gradient{};
        std::array<std::array<double, kAspects>, kAspects> hessian{};
        for (const size_t i : use) {
            const auto& x = answers[i].further;
            double z = 0.0;
            for (size_t a = 0; a < kAspects; ++a) z += u[a] * x[a];
            const double p = 1.0 / (1.0 + std::exp(-z));
            const double r = p - (answers[i].pickedB ? 1.0 : 0.0);
            for (size_t a = 0; a < kAspects; ++a) {
                gradient[a] += r * x[a];
                for (size_t b = 0; b < kAspects; ++b) hessian[a][b] += p * (1.0 - p) * x[a] * x[b];
            }
        }
        const auto o = off(u);
        for (size_t a = 0; a < kAspects; ++a) {
            gradient[a] += ridge * u[a] + pull * o[a];
            for (size_t b = 0; b < kAspects; ++b) hessian[a][b] += pull * ((a == b ? 1.0 : 0.0) - d[a] * d[b]);
            hessian[a][a] += ridge;
        }
        // Held at 0: the aspects at 0 the loss would push below it.
        std::array<bool, kAspects> free{};
        for (size_t a = 0; a < kAspects; ++a) free[a] = u[a] > 1e-9 || gradient[a] < 0.0;
        // Solve hessian * step = gradient over the free aspects (Gaussian elimination).
        std::vector<size_t> f;
        for (size_t a = 0; a < kAspects; ++a)
            if (free[a]) f.push_back(a);
        const size_t n = f.size();
        std::vector<std::vector<double>> m(n, std::vector<double>(n + 1));
        for (size_t i = 0; i < n; ++i) {
            for (size_t j = 0; j < n; ++j) m[i][j] = hessian[f[i]][f[j]];
            m[i][n] = gradient[f[i]];
        }
        for (size_t col = 0; col < n; ++col) {
            size_t pivot = col;
            for (size_t r = col + 1; r < n; ++r)
                if (std::abs(m[r][col]) > std::abs(m[pivot][col])) pivot = r;
            std::swap(m[col], m[pivot]);
            for (size_t r = 0; r < n; ++r) {
                if (r == col) continue;
                const double k = m[r][col] / m[col][col];
                for (size_t c = col; c <= n; ++c) m[r][c] -= k * m[col][c];
            }
        }
        std::array<double, kAspects> step{};
        for (size_t i = 0; i < n; ++i) step[f[i]] = m[i][n] / m[i][i];
        // Back off until the loss falls, clamping at 0.
        bool moved = false;
        for (double t = 1.0; t > 1e-6; t *= 0.5) {
            std::array<double, kAspects> trial;
            for (size_t a = 0; a < kAspects; ++a) trial[a] = std::max(0.0, u[a] - t * step[a]);
            const double l = loss(trial);
            if (l < current - 1e-10) {
                u = trial;
                moved = current - l > 1e-9;
                current = l;
                break;
            }
        }
        if (!moved) break;
    }
    return u;
}

// The weights a fit gives, scaled to add up to what the defaults do.
AspectWeights weightsOf(const std::array<double, kAspects>& u) {
    const auto defaults = AspectWeights::defaults();
    const double total = std::accumulate(defaults.begin(), defaults.end(), 0.0);
    const double sum = std::accumulate(u.begin(), u.end(), 0.0);
    AspectWeights w;
    for (size_t a = 0; a < kAspects; ++a) w.weight[a] = sum > 0.0 ? static_cast<float>(u[a] * total / sum) : 0.f;
    return w;
}

// How many answers the weights agree with (the closer by them is the one picked).
double agreement(const std::vector<Answer>& answers, const std::vector<size_t>& use, const AspectWeights& w) {
    if (use.empty()) return 0.0;
    double hits = 0.0;
    for (const size_t i : use) {
        double z = 0.0;
        for (size_t a = 0; a < kAspects; ++a) z += w.weight[a] * answers[i].further[a];
        hits += z == 0.0 ? 0.5 : ((z > 0.0) == answers[i].pickedB ? 1.0 : 0.0);
    }
    return hits / static_cast<double>(use.size());
}

// How unlikely an answer is by u (its negative log-likelihood).
double surprise(const Answer& answer, const std::array<double, kAspects>& u) {
    double z = 0.0;
    for (size_t a = 0; a < kAspects; ++a) z += u[a] * answer.further[a];
    if (!answer.pickedB) z = -z;
    return z > 0 ? std::log1p(std::exp(-z)) : -z + std::log1p(std::exp(z));
}

// How hard to pull towards the defaults: each pull of a range fitted on four
// of five groups of `use` (by A) and judged by how unlikely it finds the fifth's
// answers. The strongest pull within one standard error of the best is taken:
// the defaults unless the answers show clearly enough that they're off.
double choosePull(const std::vector<Answer>& answers, const std::vector<size_t>& use) {
    static constexpr double pulls[] = {0.0, 1.0, 3.0, 10.0, 30.0, 100.0, 300.0, 1000.0, 1e4};
    constexpr size_t count = std::size(pulls);
    std::hash<std::string> hash;
    std::vector<std::array<double, count>> each(use.size());  // each answer's surprise, by pull
    for (size_t p = 0; p < count; ++p)
        for (size_t g = 0; g < 5; ++g) {
            std::vector<size_t> train;
            for (const size_t i : use)
                if (hash(answers[i].anchor + "#inner") % 5 != g) train.push_back(i);
            const auto u = fitAnswers(answers, train, pulls[p]);
            for (size_t k = 0; k < use.size(); ++k)
                if (hash(answers[use[k]].anchor + "#inner") % 5 == g) each[k][p] = surprise(answers[use[k]], u);
        }
    const double n = static_cast<double>(use.size());
    std::array<double, count> mean{};
    for (const auto& e : each)
        for (size_t p = 0; p < count; ++p) mean[p] += e[p] / n;
    const size_t best = static_cast<size_t>(std::min_element(mean.begin(), mean.end()) - mean.begin());
    for (size_t p = count; p-- > best;) {
        // The standard error of the difference from the best (answer by answer).
        double sum = 0.0, squares = 0.0;
        for (const auto& e : each) {
            const double d = e[p] - e[best];
            sum += d;
            squares += d * d;
        }
        const double m = sum / n, se = std::sqrt(std::max(0.0, squares / n - m * m) / std::max(1.0, n - 1.0));
        if (m <= se) return pulls[p];
    }
    return pulls[best];
}

// The answers the default weights get wrong: the closer by them is the one not
// picked. Printed (the 30 they were surest of, each aspect's say: + for the one
// picked) and written as triplets, the picked one as B, to hear again with
// `rate.py --names`: what they share that the fingerprint misses.
void writeMisses(const std::vector<Answer>& answers, const std::string& file) {
    const AspectWeights w;
    double total = 0.0;
    for (const float x : w.weight) total += x;
    struct Miss {
        double sureness;  // how much closer, by the defaults, the one not picked is
        const Answer* answer;
    };
    std::vector<Miss> misses;
    for (const Answer& answer : answers) {
        double z = 0.0;
        for (size_t a = 0; a < kAspects; ++a) z += w.weight[a] / total * answer.further[a];
        if (z != 0.0 && (z > 0.0) != answer.pickedB) misses.push_back({std::abs(z), &answer});
    }
    std::sort(misses.begin(), misses.end(), [](const Miss& x, const Miss& y) { return x.sureness > y.sureness; });
    auto name = [](const std::string& path) { return utf8(pathOf(path).filename()); };
    std::printf("\n%zu of %zu answers the default weights get wrong; the surest of them (each aspect: how much it\n"
                "pulled towards the one picked, + right, - wrong):\n",
                misses.size(), answers.size());
    std::printf("  %6s %6s  ", "wrong", "ms");
    for (const char* a : {"timbre", "motion", "spectr", "envel", "pitch", "rhythm"}) std::printf("%6s ", a);
    std::printf(" A / picked / not picked\n");
    for (size_t i = 0; i < misses.size() && i < 30; ++i) {
        const Answer& m = *misses[i].answer;
        std::printf("  %6.2f %6ld  ", misses[i].sureness, m.ms);
        for (size_t a = 0; a < kAspects; ++a) std::printf("%+6.2f ", m.pickedB ? m.further[a] : -m.further[a]);
        std::printf(" %s / %s / %s\n", name(m.anchor).c_str(), name(m.pickedB ? m.b : m.c).c_str(),
                    name(m.pickedB ? m.c : m.b).c_str());
    }
    std::ofstream out(pathOf(file), std::ios::binary);
    out << "# sound_similarity_bench misses: B is the one picked, C the one the default weights call closer\n"
        << "# id\trepeat of\tA\tB\tC\n";
    for (size_t i = 0; i < misses.size(); ++i) {
        const Answer& m = *misses[i].answer;
        out << i << "\t\t" << m.anchor << '\t' << (m.pickedB ? m.b : m.c) << '\t' << (m.pickedB ? m.c : m.b) << '\n';
    }
    std::printf("  all %zu written to %s, surest first (B the one picked)\n", misses.size(), file.c_str());
}

// Whether a sound's fingerprint survives what shouldn't change how it sounds:
// for `count` one-shots, each change's copy is analysed and placed among the
// library by its distance from the original. The copy should be the nearest
// (rank 1), or nearly. Changes: 12 dB quieter, 0.5 s of silence before, 1 s
// after, and resampled (miniaudio, as files above 48 kHz are) to 32 and 22.05 kHz.
void robustness(const std::vector<Sound>& sounds, const std::vector<uint32_t>& rows, const std::vector<float>& matrix,
                const AspectWeights& weights, size_t count, uint32_t seed) {
    const Comparison comparison = Comparison::fit(matrix.data(), rows.size(), nullptr, weights);
    const AspectComparisons aspects(matrix, rows.size());
    auto fp = [&](uint32_t r) { return matrix.data() + static_cast<size_t>(r) * kDims; };
    std::vector<uint32_t> shots;
    for (uint32_t r = 0; r < rows.size(); ++r)
        if (!sounds[rows[r]].loop && fp(r)[feature::Length] <= std::log10(4.f)) shots.push_back(r);
    std::mt19937 random(seed);
    std::shuffle(shots.begin(), shots.end(), random);
    if (shots.size() > count) shots.resize(count);
    const char* const names[] = {"12 dB quieter", "0.5 s silence before", "1 s silence after", "at 32 kHz",
                                 "at 22.05 kHz"};
    constexpr size_t kChanges = std::size(names);
    struct Tally {
        std::vector<double> distance;
        std::vector<size_t> rank;
        AspectDistances aspectSum{};
    };
    std::array<Tally, kChanges> tally;
    SoundAnalyzer analyzer;
    const double decode = kAnalysisSeconds + kLeadInSeconds + 1.0;
    for (const uint32_t r : shots) {
        const std::string& path = sounds[rows[r]].path;
        for (size_t c = 0; c < kChanges; ++c) {
            std::optional<Fingerprint> copy;
            try {
                MonoAudio audio = readMono(path, 0.0, decode, c == 3 ? 32000 : c == 4 ? 22050 : 48000);
                auto& x = audio.samples;
                const auto rate = audio.sampleRate;
                double seconds = audio.fileSeconds;
                if (c == 0)
                    for (float& v : x) v *= 0.25f;
                if (c == 1) {
                    x.insert(x.begin(), static_cast<size_t>(0.5 * rate), 0.f);
                    seconds += 0.5;
                }
                if (c == 2 && !audio.truncated) {
                    x.insert(x.end(), static_cast<size_t>(1.0 * rate), 0.f);
                    seconds += 1.0;
                }
                copy = analyzer.analyze(x.data(), x.size(), rate, seconds, audio.truncated);
            } catch (const std::exception&) {
            }
            if (!copy) continue;
            const float d = comparison.distance(fp(r), copy->data());
            size_t closer = 0;
            for (uint32_t o = 0; o < rows.size(); ++o)
                if (o != r && comparison.distance(fp(r), fp(o)) < d) ++closer;
            tally[c].distance.push_back(d);
            tally[c].rank.push_back(closer + 1);
            const AspectDistances ad = aspects(fp(r), copy->data());
            for (size_t a = 0; a < kAspects; ++a) tally[c].aspectSum[a] += ad[a];
        }
    }
    std::printf("\nrobustness: %zu one-shots, each changed and placed among the library by its distance from the "
                "original\n", shots.size());
    std::printf("  %-22s %6s %8s %8s %8s %9s   mean distance by aspect (", "change", "copies", "rank 1", "top 10",
                "median d", "worst rank");
    for (size_t a = 0; a < kAspects; ++a) std::printf("%s%s", a ? " " : "", aspectName(static_cast<Aspect>(a)));
    std::printf(")\n");
    for (size_t c = 0; c < kChanges; ++c) {
        auto& t = tally[c];
        if (t.rank.empty()) continue;
        const double n = static_cast<double>(t.rank.size());
        const double first = static_cast<double>(std::count(t.rank.begin(), t.rank.end(), size_t{1}));
        const double top = static_cast<double>(std::count_if(t.rank.begin(), t.rank.end(), [](size_t k) { return k <= 10; }));
        std::sort(t.distance.begin(), t.distance.end());
        std::printf("  %-22s %6zu %7.1f%% %7.1f%% %8.3f %9zu  ", names[c], t.rank.size(), 100 * first / n, 100 * top / n,
                    t.distance[t.distance.size() / 2], *std::max_element(t.rank.begin(), t.rank.end()));
        for (size_t a = 0; a < kAspects; ++a) std::printf(" %.3f", t.aspectSum[a] / n);
        std::printf("\n");
    }
}

// Fits the weights to the rater's answers (tools/similarity_rater) and says how
// far to trust them. The aspects' distances come from this library's spreads:
// rate and fit on the same library.
std::optional<AspectWeights> fitRatings(const std::vector<Sound>& sounds, const std::vector<uint32_t>& rows,
                                        const std::vector<float>& matrix, const std::string& file, uint32_t seed,
                                        const std::string& missesFile) {
    const AspectComparisons aspects(matrix, rows.size());
    // By whole path, however the library's folder was written.
    auto whole = [](const std::string& path) {
        std::error_code error;
        const fs::path p = fs::weakly_canonical(pathOf(path), error);
        return utf8(error ? fs::absolute(pathOf(path)).lexically_normal() : p);
    };
    std::map<std::string, uint32_t> rowOf;
    for (uint32_t r = 0; r < rows.size(); ++r) rowOf[whole(sounds[rows[r]].path)] = r;
    // A library moved since the answers (another drive or folder): its files by
    // their last three parts (two folders and the name), where those are unique.
    auto tail = [](const std::string& path) {
        const fs::path p = pathOf(path);
        std::vector<std::string> parts;
        for (const auto& part : p) parts.push_back(utf8(part));
        std::string key;
        for (size_t i = parts.size() > 3 ? parts.size() - 3 : 0; i < parts.size(); ++i) key += "/" + parts[i];
        return key;
    };
    std::map<std::string, int64_t> rowOfTail;  // -1: more than one file
    for (uint32_t r = 0; r < rows.size(); ++r) {
        const auto [it, added] = rowOfTail.emplace(tail(sounds[rows[r]].path), r);
        if (!added) it->second = -1;
    }
    auto find = [&](const std::string& path) -> std::optional<uint32_t> {
        if (const auto it = rowOf.find(whole(path)); it != rowOf.end()) return it->second;
        if (const auto it = rowOfTail.find(tail(path)); it != rowOfTail.end() && it->second >= 0)
            return static_cast<uint32_t>(it->second);
        return std::nullopt;
    };
    auto fp = [&](uint32_t r) { return matrix.data() + static_cast<size_t>(r) * kDims; };
    std::vector<Answer> answers;
    size_t skipped = 0, missing = 0;
    // The same question asked twice (B and C in either order): did the answers agree?
    std::map<std::string, std::vector<std::string>> asked;
    for (const auto& line : readTsv(file)) {
        // id, choice (b, c or skip), A, B, C[, ms]
        if (line.size() < 5) continue;
        const std::string& choice = line[1];
        if (choice != "b" && choice != "c") {
            ++skipped;
            continue;
        }
        const auto a = find(line[2]), b = find(line[3]), c = find(line[4]);
        if (!a || !b || !c) {
            ++missing;
            continue;
        }
        const AspectDistances ab = aspects(fp(*a), fp(*b)), ac = aspects(fp(*a), fp(*c));
        Answer answer;
        for (size_t i = 0; i < kAspects; ++i) answer.further[i] = ac[i] - ab[i];
        answer.pickedB = choice == "b";
        // Where the files are now (the answers may name where they were).
        auto now = [&](uint32_t r) { return utf8(fs::absolute(pathOf(sounds[rows[r]].path)).lexically_normal()); };
        answer.labelled = !sounds[rows[*a]].label.empty() && !sounds[rows[*a]].loop;
        answer.anchor = now(*a);
        answer.b = now(*b);
        answer.c = now(*c);
        if (line.size() > 5) answer.ms = std::strtol(line[5].c_str(), nullptr, 10);
        answers.push_back(answer);
        const bool ordered = line[3] < line[4];
        asked[line[2] + '\t' + (ordered ? line[3] + '\t' + line[4] : line[4] + '\t' + line[3])].push_back(
            line[choice == "b" ? 3 : 4]);
    }
    std::printf("\n%zu answers from %s (%zu can't tell, %zu with a sound not in this library)\n", answers.size(),
                file.c_str(), skipped, missing);
    if (!missesFile.empty() && !answers.empty()) writeMisses(answers, missesFile);
    if (answers.size() < 20) {
        std::printf("  too few to fit\n");
        return std::nullopt;
    }
    int repeats = 0, same = 0;
    for (const auto& [question, picks] : asked)
        for (size_t i = 1; i < picks.size(); ++i) {
            ++repeats;
            same += picks[i] == picks[0];
        }
    if (repeats)
        std::printf("  asked again: the same answer %d times out of %d (%.0f%%; chance is 50%%, and the questions are "
                    "close calls on purpose)\n", same, repeats, 100.0 * same / repeats);

    std::vector<size_t> all(answers.size());
    std::iota(all.begin(), all.end(), size_t{0});
    const double pull = choosePull(answers, all);
    const AspectWeights fitted = weightsOf(fitAnswers(answers, all, pull));

    // Cross-validated: answers in five groups by their A, each group judged by
    // weights fitted on the other four (their pull chosen from those four alone).
    std::map<std::string, int> group;
    std::vector<std::string> anchors;
    for (const Answer& answer : answers)
        if (group.emplace(answer.anchor, 0).second) anchors.push_back(answer.anchor);
    std::mt19937 random(seed);
    std::shuffle(anchors.begin(), anchors.end(), random);
    for (size_t i = 0; i < anchors.size(); ++i) group[anchors[i]] = static_cast<int>(i % 5);
    double heldOutFitted = 0.0, heldOutDefaults = 0.0;
    for (int g = 0; g < 5; ++g) {
        std::vector<size_t> train, test;
        for (size_t i = 0; i < answers.size(); ++i) (group[answers[i].anchor] == g ? test : train).push_back(i);
        heldOutFitted +=
            agreement(answers, test, weightsOf(fitAnswers(answers, train, choosePull(answers, train)))) * test.size();
        heldOutDefaults += agreement(answers, test, AspectWeights{}) * test.size();
    }
    // Where the defaults do well and where not: drums (A a one-shot of a known
    // kind) and the rest (instruments, vocals, FX, loops).
    {
        std::vector<size_t> drums, rest;
        for (size_t i = 0; i < answers.size(); ++i) (answers[i].labelled ? drums : rest).push_back(i);
        std::printf("  the default weights agree with %.1f%% of %zu answers about drums (A a kick, snare, hat...), "
                    "%.1f%% of %zu about the rest\n",
                    100.0 * agreement(answers, drums, AspectWeights{}), drums.size(),
                    100.0 * agreement(answers, rest, AspectWeights{}), rest.size());
    }
    std::printf("  pulled towards the default weights by %g (0: not at all; 10000: all but the defaults)\n", pull);
    std::printf("  answers agreed with, on answers held out of the fit: default weights %.1f%%, fitted %.1f%% "
                "(on all, fitted on all: %.1f%%)\n",
                100.0 * heldOutDefaults / answers.size(), 100.0 * heldOutFitted / answers.size(),
                100.0 * agreement(answers, all, fitted));

    // How sure each weight is: refitted on the answers drawn again (by A, 200
    // times), unpulled: with the pull the intervals would only show the defaults.
    std::map<std::string, std::vector<size_t>> byAnchor;
    for (size_t i = 0; i < answers.size(); ++i) byAnchor[answers[i].anchor].push_back(i);
    std::array<std::vector<float>, kAspects> drawn;
    std::uniform_int_distribution<size_t> pick(0, anchors.size() - 1);
    for (int draw = 0; draw < 200; ++draw) {
        std::vector<size_t> use;
        for (size_t i = 0; i < anchors.size(); ++i) {
            const auto& those = byAnchor[anchors[pick(random)]];
            use.insert(use.end(), those.begin(), those.end());
        }
        const AspectWeights w = weightsOf(fitAnswers(answers, use, 0.0));
        for (size_t a = 0; a < kAspects; ++a) drawn[a].push_back(w.weight[a]);
    }
    const AspectWeights defaults;
    std::printf("  %-13s %8s %8s %26s\n", "aspect", "default", "fitted", "90% interval, answers alone");
    for (size_t a = 0; a < kAspects; ++a) {
        auto& d = drawn[a];
        std::sort(d.begin(), d.end());
        std::printf("  %-13s %8.2f %8.2f %7.2f - %-7.2f\n", aspectName(static_cast<Aspect>(a)), defaults.weight[a],
                    fitted.weight[a], d[d.size() / 20], d[d.size() * 19 / 20]);
    }
    std::printf("  fitted weights %s\n", weightsText(fitted).c_str());
    return fitted;
}

}  // namespace

int main(int argc, char** argv) {
    std::string folder, cacheFile, jsonFile, tripletsFile, ratingsFile, missesFile;
    size_t tripletCount = 0, robustCount = 0;
    uint32_t seed = 1;
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
        else if (arg == "--triplets") {
            tripletCount = std::stoul(next());
            tripletsFile = next();
        } else if (arg == "--seed") seed = static_cast<uint32_t>(std::stoul(next()));
        else if (arg == "--ratings") ratingsFile = next();
        else if (arg == "--misses") missesFile = next();
        else if (arg == "--robustness") robustCount = std::stoul(next());
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
                     "[--weights t,m,s,e,p,r] [--tune] [--show N] [--json file] [--triplets N file] [--seed N] "
                     "[--ratings file [--misses file]] [--robustness N]\n";
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

    // --- Weights from listening ---
    if (!ratingsFile.empty()) {
        if (const auto fitted = fitRatings(sounds, rows, matrix, ratingsFile, seed, missesFile)) {
            if (!queries.empty())
                std::printf("\nweights %s: mean P@10 %.3f (over kinds)\n", weightsText(weights).c_str(),
                            evaluate(sounds, matrix, rows, queries, weights).macro10);
            weights = *fitted;
        }
    }
    if (robustCount > 0) robustness(sounds, rows, matrix, weights, robustCount, seed);
    if (tripletCount > 0) writeTriplets(sounds, rows, matrix, weights, tripletCount, seed, tripletsFile);

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
