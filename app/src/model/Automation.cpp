#include "model/Automation.h"

#include <Automation.h>  // the engine's (engine/src/Automation.h): it plays the same curves

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

namespace sub::app::automation {

static_assert(kCurvature == static_cast<double>(sub::kAutomationCurvature),
              "the model's curves must bend as the engine's do");

namespace {

const QString kDevicePrefix = QStringLiteral("device:");

// The gain floor on a volume lane: values at or below it are silence.
const double kVolumeFloor = std::pow(10.0, (kMinVolumeDb - kMaxVolumeDb) / 60.0);

double clampValue(double value, double low = 0.0, double high = 1.0) { return std::min(high, std::max(low, value)); }

double bendOf(const AutomationPoint& start, const AutomationPoint& end) {
    return end.value >= start.value ? start.curve : -start.curve;
}

int countBefore(const Envelope& points, double beat) {
    return static_cast<int>(std::lower_bound(points.begin(), points.end(), beat,
                                             [](const AutomationPoint& p, double b) { return p.beat < b; }) -
                            points.begin());
}

// Drops points that repeat the one before them (same beat and value).
Envelope simplify(const Envelope& points) {
    Envelope out;
    for (const AutomationPoint& p : points) {
        if (!out.empty() && out.back().beat == p.beat && out.back().value == p.value) {
            out.back() = p;
        } else {
            out.push_back(p);
        }
    }
    return out;
}

// The first point at `beat`: where the envelope arrives there from before.
const AutomationPoint& firstAt(const Envelope& points, double beat) {
    for (const AutomationPoint& p : points) {
        if (p.beat == beat) return p;
    }
    throw std::logic_error("no point at that beat");
}

// The last point at `beat`: where the envelope leaves from there.
const AutomationPoint& lastAt(const Envelope& points, double beat) {
    for (auto it = points.rbegin(); it != points.rend(); ++it) {
        if (it->beat == beat) return *it;
    }
    throw std::logic_error("no point at that beat");
}

// The envelope with points at `start` and `end` (it doesn't change).
Envelope edges(const Envelope& points, double start, double end) {
    Envelope edged = splitAt(points, start).first;
    return splitAt(edged, end).first;
}

AutomationPoint withCurve(AutomationPoint p, double curve) {
    p.curve = curve;
    return p;
}

// Whether the envelope stays the same without point i.
bool redundant(const Envelope& points, size_t i) {
    const AutomationPoint& p = points[i];
    const AutomationPoint* before = i > 0 ? &points[i - 1] : nullptr;
    const AutomationPoint* after = i + 1 < points.size() ? &points[i + 1] : nullptr;
    if (before == nullptr || after == nullptr) {
        const AutomationPoint* other = before != nullptr ? before : after;
        return other != nullptr && other->value == p.value && other->beat != p.beat;
    }
    if (before->beat == p.beat || p.beat == after->beat) return false;  // part of a step
    if (before->value == p.value && p.value == after->value) return true;
    if (before->curve != 0.0 || p.curve != 0.0) return false;
    const double onLine =
        before->value + (after->value - before->value) * (p.beat - before->beat) / (after->beat - before->beat);
    return std::abs(onLine - p.value) < 1e-9;
}

}  // namespace

// --- Keys ---

QString deviceKey(const QString& deviceId, const QString& paramId) {
    return kDevicePrefix + deviceId + u':' + paramId;
}

QString sendKey(const QString& returnId) { return kSendPrefix + returnId; }

QString chainKey(const QString& rackId, const QString& chainId, const QString& control) {
    return deviceKey(rackId, kChainPrefix + chainId + u':' + control);
}

QString deviceOnKey(const QString& deviceId) { return deviceKey(deviceId, kDeviceOn); }

std::optional<Target> parseKey(const QString& key) {
    const qsizetype colon = key.indexOf(u':');
    const QString kind = colon < 0 ? key : key.left(colon);
    const QString rest = colon < 0 ? QString() : key.mid(colon + 1);
    if (kind == u"mixer" && kMixerKeys.contains(key)) return Target{kind, rest, {}};
    if (kind == u"send" && !rest.isEmpty()) return Target{kind, rest, {}};
    if (kind == u"device") {
        const qsizetype separator = rest.indexOf(u':');
        if (separator > 0) return Target{kind, rest.left(separator), rest.mid(separator + 1)};
    }
    return std::nullopt;
}

bool isKey(const QString& key) { return parseKey(key).has_value(); }

std::optional<QString> keyDevice(const QString& key) {
    const auto parts = parseKey(key);
    if (!parts || parts->kind != u"device") return std::nullopt;
    return parts->id;
}

std::optional<QString> keySend(const QString& key) {
    if (!key.startsWith(kSendPrefix) || key.size() == kSendPrefix.size()) return std::nullopt;
    return key.mid(kSendPrefix.size());
}

std::optional<QString> keyChain(const QString& key) {
    const auto control = keyChainControl(key);
    if (!control) return std::nullopt;
    return control->chainId;
}

std::optional<ChainControl> keyChainControl(const QString& key) {
    const auto parts = parseKey(key);
    if (!parts || parts->kind != u"device" || !parts->param.startsWith(kChainPrefix)) return std::nullopt;
    const QString rest = parts->param.mid(kChainPrefix.size());
    const qsizetype colon = rest.indexOf(u':');
    const QString chainId = colon < 0 ? rest : rest.left(colon);
    const QString control = colon < 0 ? QString() : rest.mid(colon + 1);
    if (chainId.isEmpty() || (control != kChainVolume && control != kChainPan)) return std::nullopt;
    return ChainControl{chainId, control};
}

bool isMixerKey(const QString& key) { return kMixerKeys.contains(key) || keySend(key).has_value(); }

bool isSwitchKey(const QString& key) {
    if (key == kMixerOn) return true;
    const auto parts = parseKey(key);
    return parts && parts->kind == u"device" && parts->param == kDeviceOn;
}

// --- Mixer mappings ---

double volumeToNormalized(double db) {
    if (db <= kMinVolumeDb) return 0.0;
    return std::min(1.0, std::pow(10.0, (db - kMaxVolumeDb) / 60.0));
}

double normalizedToVolume(double value) {
    if (value <= kVolumeFloor) return kMinVolumeDb;
    return kMaxVolumeDb + 60.0 * std::log10(std::min(1.0, value));
}

double panToNormalized(double pan) { return std::min(1.0, std::max(0.0, (pan + 1.0) / 2.0)); }

double normalizedToPan(double value) { return std::min(1.0, std::max(0.0, value)) * 2.0 - 1.0; }

// --- Evaluating ---

double shape(double x, double bend) {
    const double a = -bend * kCurvature;
    if (std::abs(a) < 1e-4) return x;
    return std::expm1(a * x) / std::expm1(a);
}

double segmentValue(const AutomationPoint& start, const AutomationPoint& end, double beat) {
    const double span = end.beat - start.beat;
    if (span <= 0) return end.value;
    const double x = std::min(1.0, std::max(0.0, (beat - start.beat) / span));
    return start.value + (end.value - start.value) * shape(x, bendOf(start, end));
}

int countAtOrBefore(const Envelope& points, double beat) {
    // Points are kept sorted by beat: bisect them by it.
    return static_cast<int>(std::upper_bound(points.begin(), points.end(), beat,
                                             [](double b, const AutomationPoint& p) { return b < p.beat; }) -
                            points.begin());
}

std::optional<double> valueAt(const Envelope& points, double beat) {
    if (points.empty()) return std::nullopt;
    const int index = countAtOrBefore(points, beat);
    if (index == 0) return points.front().value;
    if (index >= static_cast<int>(points.size())) return points.back().value;
    return segmentValue(points[index - 1], points[index], beat);
}

std::optional<double> leftValue(const Envelope& points, double beat) {
    if (points.empty()) return std::nullopt;
    const int index = countBefore(points, beat);
    if (index == 0) return points.front().value;
    if (index >= static_cast<int>(points.size())) return points.back().value;
    return segmentValue(points[index - 1], points[index], beat);
}

std::vector<std::pair<double, double>> sample(const Envelope& points, double start, double end, int count) {
    std::vector<std::pair<double, double>> result;
    if (count < 2 || points.empty()) return result;
    const double step = (end - start) / (count - 1);
    for (int i = 0; i < count; ++i) result.emplace_back(start + i * step, *valueAt(points, start + i * step));
    return result;
}

// --- Editing ---

Envelope normalize(const Envelope& points) {
    Envelope cleaned;
    cleaned.reserve(points.size());
    for (const AutomationPoint& p : points) {
        cleaned.push_back({std::max(0.0, p.beat), clampValue(p.value), clampValue(p.curve, -1.0, 1.0)});
    }
    std::stable_sort(cleaned.begin(), cleaned.end(),
                     [](const AutomationPoint& a, const AutomationPoint& b) { return a.beat < b.beat; });
    return cleaned;
}

std::pair<Envelope, int> splitAt(const Envelope& original, double beat) {
    Envelope points = original;
    const int index = countAtOrBefore(points, beat);
    const auto value = valueAt(points, beat);
    if (!value) throw std::invalid_argument("an empty envelope has nothing to split");
    double curve = 0.0;
    if (index > 0 && index < static_cast<int>(points.size())) {
        const AutomationPoint before = points[index - 1];
        const AutomationPoint after = points[index];
        const double share = (beat - before.beat) / (after.beat - before.beat);
        points[index - 1].curve = before.curve * share;
        curve = before.curve * (1.0 - share);
    }
    points.insert(points.begin() + index, AutomationPoint{beat, *value, curve});
    return {points, index};
}

std::pair<Envelope, int> addPoint(const Envelope& points, double beat, double value) {
    beat = std::max(0.0, beat);
    if (points.empty()) return {Envelope{AutomationPoint{beat, clampValue(value), 0.0}}, 0};
    auto [split, index] = splitAt(points, beat);
    split[index].value = clampValue(value);
    return {split, index};
}

Envelope movePoints(const Envelope& points, const std::vector<int>& indices, double deltaBeats, double deltaValue) {
    return movePointsMapped(points, indices, deltaBeats, deltaValue).first;
}

std::pair<Envelope, QMap<int, int>> movePointsMapped(const Envelope& points, const std::vector<int>& indices,
                                                     double deltaBeats, double deltaValue) {
    const int count = static_cast<int>(points.size());
    std::set<int> moving;
    for (int i : indices) {
        if (i >= 0 && i < count) moving.insert(i);
    }
    if (moving.empty()) return {points, {}};
    const std::vector<int> chosen(moving.begin(), moving.end());
    if (chosen.size() == 1) {
        const int i = chosen.front();
        double before = 0.0;
        for (int j = i - 1; j >= 0; --j) {
            if (!moving.count(j)) {
                before = points[j].beat;
                break;
            }
        }
        double after = std::numeric_limits<double>::infinity();
        for (int j = i + 1; j < count; ++j) {
            if (!moving.count(j)) {
                after = points[j].beat;
                break;
            }
        }
        deltaBeats = std::min(after - points[i].beat, std::max(before - points[i].beat, deltaBeats));
        Envelope moved = points;
        moved[i].beat = points[i].beat + deltaBeats;
        moved[i].value = clampValue(points[i].value + deltaValue);
        return {moved, QMap<int, int>{{i, i}}};
    }
    double earliest = std::numeric_limits<double>::infinity();
    for (int i : chosen) earliest = std::min(earliest, points[i].beat);
    deltaBeats = std::max(deltaBeats, -earliest);
    const double low = points[chosen.front()].beat + deltaBeats;
    const double high = points[chosen.back()].beat + deltaBeats;
    // (sort key, old index, point): kept points at the edges stay on their side.
    struct Entry {
        double beat;
        int side;
        int index;
        AutomationPoint point;
    };
    std::vector<Entry> entries;
    for (int j = 0; j < count; ++j) {
        const AutomationPoint& p = points[j];
        if (moving.count(j)) {
            AutomationPoint moved{p.beat + deltaBeats, clampValue(p.value + deltaValue), p.curve};
            entries.push_back({moved.beat, 1, j, moved});
        } else if (!(low < p.beat && p.beat < high)) {
            int side = (p.beat < low || (p.beat == low && j < chosen.front())) ? 0 : 2;
            if (p.beat == low && low == high) side = j < chosen.front() ? 0 : 2;
            entries.push_back({p.beat, side, j, p});
        }
    }
    // Stable: moved points keep their order.
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.beat != b.beat) return a.beat < b.beat;
        return a.side < b.side;
    });
    Envelope result;
    QMap<int, int> where;
    for (int n = 0; n < static_cast<int>(entries.size()); ++n) {
        result.push_back(entries[n].point);
        if (moving.count(entries[n].index)) where.insert(entries[n].index, n);
    }
    return {result, where};
}

Envelope deletePoints(const Envelope& points, const std::vector<int>& indices) {
    const std::set<int> gone(indices.begin(), indices.end());
    Envelope result;
    for (int i = 0; i < static_cast<int>(points.size()); ++i) {
        if (!gone.count(i)) result.push_back(points[i]);
    }
    return result;
}

Envelope setCurve(const Envelope& points, int index, double curve) {
    if (index < 0 || index >= static_cast<int>(points.size())) return points;
    Envelope result = points;
    result[index].curve = clampValue(curve, -1.0, 1.0);
    return result;
}

std::optional<int> segmentIndex(const Envelope& points, double beat) {
    const int index = countAtOrBefore(points, beat);
    if (index == 0 || index >= static_cast<int>(points.size())) return std::nullopt;
    return index - 1;
}

Envelope removeRange(const Envelope& points, double start, double end) {
    if (points.empty() || end <= start || !hasPointsIn(points, start, end)) return points;
    const bool all = std::all_of(points.begin(), points.end(),
                                 [&](const AutomationPoint& p) { return start <= p.beat && p.beat <= end; });
    if (all) return {};
    const Envelope edged = edges(points, start, end);
    Envelope joined;
    for (const AutomationPoint& p : edged) {
        if (p.beat < start) joined.push_back(p);
    }
    joined.push_back(withCurve(firstAt(edged, start), 0.0));
    joined.push_back(lastAt(edged, end));
    for (const AutomationPoint& p : edged) {
        if (p.beat > end) joined.push_back(p);
    }
    return simplify(joined);
}

Envelope copyRange(const Envelope& points, double start, double end) {
    if (points.empty() || end <= start) return {};
    const Envelope edged = edges(points, start, end);
    Envelope inside{lastAt(edged, start)};
    for (const AutomationPoint& p : edged) {
        if (start < p.beat && p.beat < end) inside.push_back(p);
    }
    inside.push_back(firstAt(edged, end));
    for (AutomationPoint& p : inside) p.beat -= start;
    return inside;
}

Envelope pasteRange(const Envelope& points, const Envelope& content, double start, double length) {
    const double end = start + length;
    Envelope shifted;
    for (const AutomationPoint& p : content) {
        if (p.beat <= length) shifted.push_back({p.beat + start, p.value, p.curve});
    }
    if (points.empty()) return normalize(shifted);
    const Envelope edged = edges(points, start, end);
    Envelope joined;
    for (const AutomationPoint& p : edged) {
        if (p.beat < start) joined.push_back(p);
    }
    joined.push_back(firstAt(edged, start));
    joined.insert(joined.end(), shifted.begin(), shifted.end());
    joined.push_back(lastAt(edged, end));
    for (const AutomationPoint& p : edged) {
        if (p.beat > end) joined.push_back(p);
    }
    return simplify(joined);
}

Envelope dropRedundant(const Envelope& points, const std::vector<double>& beats) {
    const std::set<double> at(beats.begin(), beats.end());
    Envelope out = points;
    size_t i = 0;
    while (i < out.size()) {
        if (at.count(out[i].beat) && redundant(out, i)) {
            out.erase(out.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    return out;
}

Envelope moveRange(const Envelope& points, double start, double end, double deltaBeats, double deltaValue) {
    if (points.empty() || end <= start) return points;
    deltaBeats = std::max(deltaBeats, -start);
    Envelope content = copyRange(points, start, end);
    for (AutomationPoint& p : content) p.value = clampValue(p.value + deltaValue);
    Envelope base = points;
    if (deltaBeats != 0.0) {
        // Where it was, straight across (keeping the envelope outside, even if all of it moves).
        const Envelope edged = edges(points, start, end);
        Envelope joined;
        for (const AutomationPoint& p : edged) {
            if (p.beat < start) joined.push_back(p);
        }
        joined.push_back(withCurve(firstAt(edged, start), 0.0));
        joined.push_back(lastAt(edged, end));
        for (const AutomationPoint& p : edged) {
            if (p.beat > end) joined.push_back(p);
        }
        base = simplify(joined);
    }
    const Envelope moved = pasteRange(base, content, start + deltaBeats, end - start);
    return dropRedundant(moved, {start, end, start + deltaBeats, end + deltaBeats});
}

bool hasPointsIn(const Envelope& points, double start, double end) {
    return std::any_of(points.begin(), points.end(),
                       [&](const AutomationPoint& p) { return start <= p.beat && p.beat <= end; });
}

std::vector<std::pair<double, double>> mergeSpans(std::vector<std::pair<double, double>> spans) {
    std::sort(spans.begin(), spans.end());
    std::vector<std::pair<double, double>> merged;
    for (const auto& [start, end] : spans) {
        if (!merged.empty() && start <= merged.back().second) {
            merged.back().second = std::max(merged.back().second, end);
        } else {
            merged.emplace_back(start, end);
        }
    }
    return merged;
}

Envelope shift(const Envelope& points, double deltaBeats) {
    Envelope moved = points;
    for (AutomationPoint& p : moved) p.beat += deltaBeats;
    return normalize(moved);
}

}  // namespace sub::app::automation
