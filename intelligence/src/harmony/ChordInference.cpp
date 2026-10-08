#include "harmony/ChordInference.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace sub::intelligence::harmony {

namespace {

constexpr int kChords = 12 * kQualities;
constexpr int kNoChord = kChords;  // the state "no chord"
constexpr int kStates = kChords + 1;

// Krumhansl and Kessler's key profiles (1982): how well each pitch class, from
// the tonic up, fits a major and a minor key.
constexpr std::array<double, 12> kMajorProfile{6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88};
constexpr std::array<double, 12> kMinorProfile{6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17};
constexpr double kMinKeyCorrelation = 0.4;  // a weaker best fit is no key

// Scoring a chord against a step (see the header).
constexpr double kUnexplained = 0.5;  // lost for each share of the profile the chord doesn't explain
constexpr double kPresent = 0.3;      // a chord note this loud (of the step's loudest pitch class) or more is there
constexpr double kMissingRoot = 0.25;
constexpr double kMissingThird = 0.15;  // and a sus chord's second or fourth, a diminished or augmented fifth
constexpr double kMissingFifth = 0.05;
constexpr double kMissingSeventh = 0.25;
constexpr double kBassRoot = 0.25;
constexpr double kBassTone = 0.08;
constexpr double kBassForeign = 0.1;
constexpr double kInKey = 0.05;
// Major, Minor, Diminished, Augmented, Sus2, Sus4, Dominant7, Major7, Minor7, HalfDiminished7, Diminished7.
constexpr std::array<double, kQualities> kPrior{0.0, 0.0, -0.06, -0.10, -0.07, -0.06, -0.02, -0.03, -0.02, -0.04, -0.08};
constexpr double kFullStep = 2.0;  // note-beats a beat of a step needs to count fully: two notes
// A thin step (fewer than three pitch classes: a melody, a bass line) says
// little about the chord; how likely each chord is in the key fills in the rest.
// By the degree of the key's scale it is the triad on: in a major key I, IV and
// V most, then vi and ii; in a minor one i and iv, then VI and VII, III and v.
constexpr std::array<double, 7> kMajorDegrees{0.3, 0.15, 0.05, 0.3, 0.3, 0.15, 0.0};
constexpr std::array<double, 7> kMinorDegrees{0.3, 0.0, 0.15, 0.3, 0.15, 0.2, 0.2};
constexpr double kThinDominant = 0.2;   // V7, and a minor key's major V (its leading note raised)
constexpr double kThinOutside = -0.2;   // a chord with notes outside the key
constexpr double kThinColour = -0.3;    // sus, augmented and diminished sevenths: what a melody alone never implies
constexpr double kThinSeventh = -0.1;   // without a key: a seventh rather than its triad
constexpr double kRest = 0.1;      // what "no chord" scores a beat where nothing sounds
// What changing chord costs, by where.
constexpr double kChangeOnBar = 0.25;
constexpr double kChangeOnHalfBar = 0.45;
constexpr double kChangeOnBeat = 0.7;
constexpr double kChangeOffBeat = 1.0;
// The lowest note counts as the bass fully up to kBassFull (E2), a quarter from kBassFaint (G3) up.
constexpr int kBassFull = 52;
constexpr int kBassFaint = 67;
constexpr double kFaintBass = 0.25;
constexpr double kBassInversion = 0.5;  // how much of a chord one of its other notes must be the bass under to be its bass

using Profile = std::array<float, 12>;

double velocityWeight(int velocity) { return 0.5 + 0.5 * std::clamp(velocity, 1, 127) / 127.0; }

double bassTrust(int pitch) {
    if (pitch <= kBassFull) return 1.0;
    if (pitch >= kBassFaint) return kFaintBass;
    return 1.0 - (1.0 - kFaintBass) * (pitch - kBassFull) / double(kBassFaint - kBassFull);
}

bool onMultiple(double value, double step) {
    if (step <= 0.0) return false;
    const double q = value / step;
    return std::abs(q - std::round(q)) < 1e-6;
}

double changeCost(double beat, double barBeats) {
    if (onMultiple(beat, barBeats)) return kChangeOnBar;
    if (onMultiple(beat, barBeats / 2)) return kChangeOnHalfBar;
    if (onMultiple(beat, 1.0)) return kChangeOnBeat;
    return kChangeOffBeat;
}

// What scoring a chord needs, worked out once.
struct Candidate {
    Chord chord;
    std::array<int, 4> tones{};
    std::array<double, 4> missing{};  // what each note costs when it isn't there
    int size = 0;
    double prior = 0.0;  // the quality's, and the key's
    double thin = 0.0;   // more in a thin step, by how likely it is in the key
};

double thinPrior(const Chord& chord, const std::optional<Key>& key) {
    const Quality q = chord.quality;
    if (q == Quality::Sus2 || q == Quality::Sus4 || q == Quality::Augmented || q == Quality::Diminished7)
        return kThinColour;
    const bool triad = q == Quality::Major || q == Quality::Minor || q == Quality::Diminished;
    if (!key) return triad ? (q == Quality::Diminished ? kThinOutside : 0.0) : kThinSeventh;
    const int dominant = pitchClass(key->tonic + 7);
    if (chord.root == dominant && (q == Quality::Dominant7 || (key->minor && q == Quality::Major))) return kThinDominant;
    if (!key->isDiatonic(chord)) return kThinOutside;
    if (!triad) return 0.0;
    for (int degree = 0; degree < 7; ++degree) {
        if (key->triad(degree) == chord) return (key->minor ? kMinorDegrees : kMajorDegrees)[static_cast<size_t>(degree)];
    }
    return 0.0;
}

std::array<Candidate, kChords> candidates(const std::optional<Key>& key) {
    std::array<Candidate, kChords> all{};
    for (int q = 0; q < kQualities; ++q) {
        const QualityInfo& info = qualityInfo(static_cast<Quality>(q));
        for (int root = 0; root < 12; ++root) {
            Candidate& c = all[static_cast<size_t>(q * 12 + root)];
            c.chord = {root, static_cast<Quality>(q)};
            c.tones = c.chord.tones();
            c.size = info.size;
            for (int i = 0; i < info.size; ++i) {
                c.missing[i] = i == 0                   ? kMissingRoot
                               : i == 3                 ? kMissingSeventh
                               : info.intervals[i] == 7 ? kMissingFifth
                                                        : kMissingThird;
            }
            c.prior = kPrior[static_cast<size_t>(q)] + (key && key->isDiatonic(c.chord) ? kInKey : 0.0);
            c.thin = thinPrior(c.chord, key);
        }
    }
    return all;
}

// Adds `weight` a beat for [a, b) to the steps it overlaps.
void spread(std::vector<Profile>& steps, double origin, double step, double a, double b, int pc, double weight) {
    if (b <= a) return;
    const auto first = static_cast<size_t>(std::max(0.0, std::floor((a - origin) / step)));
    for (size_t t = first; t < steps.size(); ++t) {
        const double s = origin + static_cast<double>(t) * step;
        if (s >= b) break;
        const double overlap = std::min(b, s + step) - std::max(a, s);
        if (overlap > 0.0) steps[t][static_cast<size_t>(pc)] += static_cast<float>(overlap * weight);
    }
}

}  // namespace

std::optional<Key> estimateKey(const std::vector<Note>& notes) {
    // How long each pitch class sounds, and again as long as it is the bass
    // (lowest, in a bass register): the bass tells C major from E minor.
    std::array<double, 12> sounding{};
    double total = 0.0;
    std::vector<std::pair<double, int>> events;  // (time, +pitch + 1 starts / -(pitch + 1) ends)
    for (const Note& n : notes) {
        const double length = n.end - n.start;
        if (length <= 0.0 || n.pitch < 0 || n.pitch > 127) continue;
        sounding[static_cast<size_t>(pitchClass(n.pitch))] += length;
        total += length;
        events.emplace_back(n.start, n.pitch + 1);
        events.emplace_back(n.end, -(n.pitch + 1));
    }
    const auto classes = std::count_if(sounding.begin(), sounding.end(), [](double d) { return d > 0.0; });
    if (total < kMinKeyBeats || classes < 3) return std::nullopt;
    std::sort(events.begin(), events.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::array<int, 128> held{};
    for (size_t i = 0; i < events.size();) {
        const double time = events[i].first;
        for (; i < events.size() && events[i].first == time; ++i)
            held[static_cast<size_t>(std::abs(events[i].second) - 1)] += events[i].second > 0 ? 1 : -1;
        if (i == events.size()) break;
        const auto lowest = std::find_if(held.begin(), held.end(), [](int count) { return count > 0; });
        if (lowest == held.end()) continue;
        const int pitch = static_cast<int>(lowest - held.begin());
        sounding[static_cast<size_t>(pitchClass(pitch))] += (events[i].first - time) * bassTrust(pitch);
    }

    const auto correlation = [&](const std::array<double, 12>& profile, int tonic) {
        double meanX = 0.0, meanY = 0.0;
        for (int pc = 0; pc < 12; ++pc) {
            meanX += sounding[static_cast<size_t>(pc)];
            meanY += profile[static_cast<size_t>(pitchClass(pc - tonic))];
        }
        meanX /= 12;
        meanY /= 12;
        double xy = 0.0, xx = 0.0, yy = 0.0;
        for (int pc = 0; pc < 12; ++pc) {
            const double x = sounding[static_cast<size_t>(pc)] - meanX;
            const double y = profile[static_cast<size_t>(pitchClass(pc - tonic))] - meanY;
            xy += x * y;
            xx += x * x;
            yy += y * y;
        }
        return xx > 0.0 && yy > 0.0 ? xy / std::sqrt(xx * yy) : 0.0;
    };
    std::optional<Key> best;
    double bestFit = kMinKeyCorrelation;
    for (int tonic = 0; tonic < 12; ++tonic) {
        for (const bool minor : {false, true}) {
            const double fit = correlation(minor ? kMinorProfile : kMajorProfile, tonic);
            if (fit > bestFit) {
                bestFit = fit;
                best = Key{tonic, minor};
            }
        }
    }
    return best;
}

Harmony inferHarmony(const std::vector<Note>& input, const InferenceOptions& options) {
    Harmony result;
    std::vector<Note> notes;
    notes.reserve(input.size());
    for (const Note& n : input) {
        if (n.end > n.start && n.pitch >= 0 && n.pitch <= 127) notes.push_back(n);
    }
    result.key = options.key ? options.key : estimateKey(notes);
    if (notes.empty()) return result;

    double first = notes.front().start, last = notes.front().end;
    for (const Note& n : notes) {
        first = std::min(first, n.start);
        last = std::max(last, n.end);
    }
    double step = options.step > 0.0 ? options.step : 0.5;
    double origin = std::floor(first / step) * step;
    const auto stepsFor = [&] { return static_cast<size_t>(std::ceil((last - origin) / step - 1e-9)); };
    while (stepsFor() > static_cast<size_t>(kMaxSteps)) {
        step *= 2;
        origin = std::floor(first / step) * step;
    }
    const size_t steps = std::max<size_t>(1, stepsFor());

    // What sounds in each step, and what is lowest.
    std::vector<Profile> sounding(steps, Profile{}), bass(steps, Profile{});
    for (const Note& n : notes)
        spread(sounding, origin, step, n.start, n.end, pitchClass(n.pitch), velocityWeight(n.velocity));
    struct Event {
        double time;
        int pitch;
        int change;  // +1 starts, -1 ends
    };
    std::vector<Event> events;
    events.reserve(notes.size() * 2);
    for (const Note& n : notes) {
        events.push_back({n.start, n.pitch, +1});
        events.push_back({n.end, n.pitch, -1});
    }
    std::sort(events.begin(), events.end(), [](const Event& a, const Event& b) { return a.time < b.time; });
    std::array<int, 128> held{};
    for (size_t i = 0; i < events.size();) {
        const double time = events[i].time;
        for (; i < events.size() && events[i].time == time; ++i)
            held[static_cast<size_t>(events[i].pitch)] += events[i].change;
        if (i == events.size()) break;
        const auto lowest = std::find_if(held.begin(), held.end(), [](int count) { return count > 0; });
        if (lowest == held.end()) continue;
        const int pitch = static_cast<int>(lowest - held.begin());
        spread(bass, origin, step, time, events[i].time, pitchClass(pitch), bassTrust(pitch));
    }

    // Each state's score for a step.
    const std::array<Candidate, kChords> chords = candidates(result.key);
    std::vector<double> score(kStates);
    const auto scoreStep = [&](size_t t) {
        const Profile& p = sounding[t];
        double energy = 0.0, loudest = 0.0;
        for (const float w : p) {
            energy += w;
            loudest = std::max(loudest, double(w));
        }
        if (energy <= 1e-9) {
            std::fill(score.begin(), score.end(), 0.0);
            score[kNoChord] = kRest * step;
            return;
        }
        const double weight = step * std::min(1.0, energy / (kFullStep * step));
        int classes = 0;
        for (const float w : p) classes += w >= kPresent * loudest;
        const double thinness = std::clamp((3 - classes) / 2.0, 0.0, 1.0);  // 1: one pitch class; 0: three or more
        std::array<double, 12> b{};
        double bassTotal = 0.0;
        for (size_t pc = 0; pc < 12; ++pc) {
            b[pc] = bass[t][pc] / step;
            bassTotal += b[pc];
        }
        for (int c = 0; c < kChords; ++c) {
            const Candidate& candidate = chords[static_cast<size_t>(c)];
            double explained = 0.0, missing = 0.0, bassFit = 0.0, bassIn = 0.0;
            for (int i = 0; i < candidate.size; ++i) {
                const auto pc = static_cast<size_t>(candidate.tones[static_cast<size_t>(i)]);
                explained += p[pc];
                missing += candidate.missing[static_cast<size_t>(i)] * std::max(0.0, 1.0 - p[pc] / (kPresent * loudest));
                bassIn += b[pc];
                bassFit += (i == 0 ? kBassRoot : kBassTone) * b[pc];
            }
            explained /= energy;
            bassFit -= kBassForeign * (bassTotal - bassIn);
            const double fit = explained - kUnexplained * (1.0 - explained) - missing + bassFit + candidate.prior +
                               thinness * candidate.thin;
            score[static_cast<size_t>(c)] = weight * fit;
        }
        score[kNoChord] = 0.0;
    };

    // The best path, where changing state costs changeCost.
    std::vector<double> total(kStates), next(kStates);
    std::vector<uint8_t> changed(steps * kStates, 0);
    std::vector<int> bestBefore(steps, 0);
    scoreStep(0);
    total = score;
    for (size_t t = 1; t < steps; ++t) {
        const int best = static_cast<int>(std::max_element(total.begin(), total.end()) - total.begin());
        bestBefore[t] = best;
        const double cost = changeCost(origin + static_cast<double>(t) * step, options.barBeats);
        scoreStep(t);
        for (int s = 0; s < kStates; ++s) {
            const double stay = total[static_cast<size_t>(s)];
            const double change = total[static_cast<size_t>(best)] - cost;
            const bool changes = s != best && change > stay;
            changed[t * kStates + static_cast<size_t>(s)] = changes;
            next[static_cast<size_t>(s)] = (changes ? change : stay) + score[static_cast<size_t>(s)];
        }
        std::swap(total, next);
    }
    std::vector<int> path(steps);
    int state = static_cast<int>(std::max_element(total.begin(), total.end()) - total.begin());
    for (size_t t = steps; t-- > 0;) {
        path[t] = state;
        if (t > 0 && changed[t * kStates + static_cast<size_t>(state)]) state = bestBefore[t];
    }

    // Runs of a chord, each with its bass.
    for (size_t t = 0; t < steps;) {
        size_t end = t + 1;
        while (end < steps && path[end] == path[t]) ++end;
        if (path[t] != kNoChord) {
            Chord chord = chords[static_cast<size_t>(path[t])].chord;
            Profile under{};
            for (size_t i = t; i < end; ++i) {
                for (size_t pc = 0; pc < 12; ++pc) under[pc] += bass[i][pc];
            }
            const int lowest = static_cast<int>(std::max_element(under.begin(), under.end()) - under.begin());
            // (Counted by how much it is a bass: a melody's lowest notes make no inversion.)
            const double length = static_cast<double>(end - t) * step;
            const bool inversion = lowest != chord.root && chord.contains(lowest);
            if (inversion && under[static_cast<size_t>(lowest)] >= kBassInversion * length) chord.bass = lowest;
            const double start = std::max(first, origin + static_cast<double>(t) * step);
            const double stop = std::min(last, origin + static_cast<double>(end) * step);
            result.chords.push_back({start, stop, chord});
        }
        t = end;
    }
    return result;
}

}  // namespace sub::intelligence::harmony
