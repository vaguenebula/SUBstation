#include "io/Serialization.h"

#include "io/Json.h"
#include "model/Automation.h"
#include "model/Errors.h"
#include "model/Keys.h"
#include "model/Notes.h"
#include "model/Paths.h"
#include "model/Project.h"
#include "model/Routing.h"
#include "model/TrackNames.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>

namespace sub::app {

namespace {

// --- Reading values as the file's format has them ---

// Something in a file that isn't as it should be; `detail` says what (after
// "<file> is damaged: ").
class Damaged : public std::runtime_error {
public:
    explicit Damaged(const QString& detail) : std::runtime_error(detail.toStdString()), detail_(detail) {}
    QString detail() const { return detail_; }

private:
    QString detail_;
};

[[noreturn]] void damaged(const QString& detail) { throw Damaged(detail); }

QString quoted(const QString& text) { return u'\'' + text + u'\''; }

const char* typeName(const QJsonValue& value) {
    switch (value.type()) {
    case QJsonValue::Null: return "null";
    case QJsonValue::Bool: return "a bool";
    case QJsonValue::Double: return "a number";
    case QJsonValue::String: return "a string";
    case QJsonValue::Array: return "a list";
    case QJsonValue::Object: return "an object";
    case QJsonValue::Undefined: return "nothing";
    }
    return "something else";
}

// A field that must be there (a missing one makes the file damaged, naming it).
QJsonValue need(const QJsonObject& object, const QString& key) {
    const auto it = object.constFind(key);
    if (it == object.constEnd()) damaged(quoted(key));
    return *it;
}

QJsonObject asObject(const QJsonValue& value) {
    if (!value.isObject()) damaged(QStringLiteral("expected an object, not %1").arg(QLatin1String(typeName(value))));
    return value.toObject();
}

// A list field: none if it isn't there.
QJsonArray listOr(const QJsonObject& object, const QString& key) {
    const auto it = object.constFind(key);
    if (it == object.constEnd()) return {};
    if (!it->isArray()) damaged(QStringLiteral("%1 is %2, not a list").arg(quoted(key), QLatin1String(typeName(*it))));
    return it->toArray();
}

// An object field: an empty one if it isn't there.
QJsonObject objectOr(const QJsonObject& object, const QString& key) {
    const auto it = object.constFind(key);
    if (it == object.constEnd()) return {};
    if (!it->isObject()) {
        damaged(QStringLiteral("%1 is %2, not an object").arg(quoted(key), QLatin1String(typeName(*it))));
    }
    return it->toObject();
}

// Whether a value counts as true (as a condition would take it).
bool truthy(const QJsonValue& value) {
    switch (value.type()) {
    case QJsonValue::Bool: return value.toBool();
    case QJsonValue::Double: return value.toDouble() != 0.0;
    case QJsonValue::String: return !value.toString().isEmpty();
    case QJsonValue::Array: return !value.toArray().isEmpty();
    case QJsonValue::Object: return !value.toObject().isEmpty();
    default: return false;
    }
}

// An object, or empty for nothing (null, a missing field, an empty value).
QJsonObject objectOrEmpty(const QJsonValue& value) {
    if (!truthy(value)) return {};
    return asObject(value);
}

bool isWholeNumber(const QJsonValue& value) {
    const QVariant variant = value.toVariant();
    return variant.typeId() == QMetaType::LongLong || variant.typeId() == QMetaType::Int;
}

// A number written as text: spaces around it, "inf" and "nan" allowed, digits
// grouped by single underscores ("1_000").
std::optional<double> numberFromText(QString text) {
    text = text.trimmed();
    if (text.contains(u'_')) {
        static const QRegularExpression grouped(QStringLiteral("(?<=\\d)_(?=\\d)"));
        text.replace(grouped, QString());
    }
    const QString lower = text.toLower();
    if (lower == u"inf" || lower == u"+inf" || lower == u"infinity" || lower == u"+infinity") {
        return std::numeric_limits<double>::infinity();
    }
    if (lower == u"-inf" || lower == u"-infinity") return -std::numeric_limits<double>::infinity();
    if (lower == u"nan" || lower == u"+nan" || lower == u"-nan") return std::numeric_limits<double>::quiet_NaN();
    bool ok = false;
    const double value = text.toDouble(&ok);
    if (!ok) return std::nullopt;
    return value;
}

double toFloat(const QJsonValue& value) {
    switch (value.type()) {
    case QJsonValue::Double: return value.toDouble();
    case QJsonValue::Bool: return value.toBool() ? 1.0 : 0.0;
    case QJsonValue::String:
        if (const auto number = numberFromText(value.toString())) return *number;
        damaged(QStringLiteral("could not convert string to float: %1").arg(quoted(value.toString())));
    default:
        damaged(QStringLiteral("expected a number, not %1").arg(QLatin1String(typeName(value))));
    }
}

long long toInt(const QJsonValue& value) {
    switch (value.type()) {
    case QJsonValue::Double: {
        if (isWholeNumber(value)) return value.toInteger();
        const double number = value.toDouble();
        if (!std::isfinite(number) || std::abs(number) > 9.0e18) damaged(QStringLiteral("cannot convert to integer"));
        return static_cast<long long>(std::trunc(number));  // as a float is made whole: toward 0
    }
    case QJsonValue::Bool: return value.toBool() ? 1 : 0;
    case QJsonValue::String: {
        static const QRegularExpression whole(QStringLiteral("\\A\\s*[+-]?\\d+(?:_\\d+)*\\s*\\z"));
        const QString text = value.toString();
        if (!whole.match(text).hasMatch()) {
            damaged(QStringLiteral("invalid literal for int() with base 10: %1").arg(quoted(text)));
        }
        QString digits = text.trimmed();
        digits.remove(u'_');
        bool ok = false;
        const long long number = digits.toLongLong(&ok);
        if (!ok) damaged(QStringLiteral("integer too large: %1").arg(quoted(text)));
        return number;
    }
    default:
        damaged(QStringLiteral("expected a whole number, not %1").arg(QLatin1String(typeName(value))));
    }
}

int toSmallInt(const QJsonValue& value) {
    const long long number = toInt(value);
    return static_cast<int>(std::clamp<long long>(number, std::numeric_limits<int>::min(),
                                                  std::numeric_limits<int>::max()));
}

// A value as text (a number as it is written; null as "None").
QString toStr(const QJsonValue& value) {
    switch (value.type()) {
    case QJsonValue::String: return value.toString();
    case QJsonValue::Bool: return value.toBool() ? QStringLiteral("True") : QStringLiteral("False");
    case QJsonValue::Double: {
        if (isWholeNumber(value)) return QString::number(value.toInteger());
        QString text = QString::number(value.toDouble(), 'g', QLocale::FloatingPointShortest);
        if (!text.contains(u'.') && !text.contains(u'e') && !text.contains(u'n')) text += QStringLiteral(".0");
        return text;
    }
    case QJsonValue::Null: return QStringLiteral("None");
    default: damaged(QStringLiteral("expected text, not %1").arg(QLatin1String(typeName(value))));
    }
}

std::optional<QString> stringOrNone(const QJsonValue& value) {
    if (!value.isString()) return std::nullopt;
    return value.toString();
}

QJsonValue optionalString(const std::optional<QString>& text) {
    return text ? QJsonValue(*text) : QJsonValue(QJsonValue::Null);
}

double clamped(double value, double low, double high) { return std::max(low, std::min(high, value)); }

// --- Paths ---

// The folder paths are kept relative to ("": none).
QString baseOf(const QString& projectFile) { return projectFile.isEmpty() ? QString() : QFileInfo(projectFile).path(); }

QJsonValue relative(const QString& path, const QString& base) {
    if (base.isEmpty() || path.isEmpty()) return QJsonValue::Null;
    const QString rel = QDir(QFileInfo(base).absoluteFilePath()).relativeFilePath(absoluteCleanPath(path));
    if (QDir::isAbsolutePath(rel)) return QJsonValue::Null;  // another drive
    return rel;
}

QString resolveClipPath(const QJsonObject& data, const QString& base, const QString& key = QStringLiteral("path"),
                        const QString& relativeKey = QStringLiteral("relative_path")) {
    const QString path = toStr(need(data, key));
    const QJsonValue rel = data.value(relativeKey);
    if (!QFileInfo::exists(path) && !base.isEmpty() && truthy(rel)) {
        const QString candidate = QDir::cleanPath(QDir(base).filePath(QDir::fromNativeSeparators(toStr(rel))));
        if (QFileInfo::exists(candidate)) return candidate;
    }
    return path;
}

// The file's name without its last extension ("kick.wav": "kick").
QString stemOf(const QString& path) {
    QString name = path;
#ifdef Q_OS_WIN
    name.replace(u'\\', u'/');
#endif
    name = name.mid(name.lastIndexOf(u'/') + 1);
    const qsizetype dot = name.lastIndexOf(u'.');
    return (dot > 0 && dot < name.size() - 1) ? name.left(dot) : name;
}

// --- Writing ---

QJsonObject clipToJson(const Clip& clip, const QString& base) {
    QJsonObject data;
    data[QStringLiteral("id")] = clip.id;
    data[QStringLiteral("name")] = clip.name;
    data[QStringLiteral("start_beat")] = clip.startBeat;
    if (clip.muted) data[QStringLiteral("muted")] = true;  // (only when deactivated: older projects read the same)
    if (clip.isMidi()) {
        data[QStringLiteral("duration_beats")] = clip.durationBeats;
        data[QStringLiteral("offset_beats")] = clip.offsetBeats;
        QJsonArray notes;
        for (const Note& n : clip.notes) {
            QJsonArray note{n.pitch, n.start, n.length, n.velocity};
            if (n.muted) note.append(true);  // (deactivated)
            notes.append(note);
        }
        data[QStringLiteral("notes")] = notes;
        return data;
    }
    data[QStringLiteral("path")] = clip.path;
    data[QStringLiteral("relative_path")] = relative(clip.path, base);
    data[QStringLiteral("duration_sec")] = clip.durationSec;
    data[QStringLiteral("offset_sec")] = clip.offsetSec;
    data[QStringLiteral("source_duration_sec")] = clip.sourceDurationSec;
    data[QStringLiteral("gain_db")] = clip.gainDb;
    data[QStringLiteral("warp")] = clip.warp;
    data[QStringLiteral("warp_mode")] = clip.warpMode;
    data[QStringLiteral("segment_bpm")] = clip.segmentBpm;
    data[QStringLiteral("transpose")] = clip.transpose;
    data[QStringLiteral("detune")] = clip.detune;
    data[QStringLiteral("pan")] = clip.pan;
    // Fades only where there are some (projects without them read the same).
    if (clip.fadeInSec > 0) {
        data[QStringLiteral("fade_in_sec")] = clip.fadeInSec;
        data[QStringLiteral("fade_in_curve")] = clip.fadeInCurve;
    }
    if (clip.fadeOutSec > 0) {
        data[QStringLiteral("fade_out_sec")] = clip.fadeOutSec;
        data[QStringLiteral("fade_out_curve")] = clip.fadeOutCurve;
    }
    if (!clip.reversedFrom.isEmpty()) {
        data[QStringLiteral("reversed_from")] = clip.reversedFrom;
        data[QStringLiteral("reversed_from_relative")] = relative(clip.reversedFrom, base);
    }
    return data;
}

QJsonObject automationToJson(const EnvelopeMap& envelopes) {
    QJsonObject data;
    for (const auto& [key, points] : envelopes) {
        if (points.empty()) continue;
        QJsonArray list;
        for (const AutomationPoint& p : points) list.append(QJsonArray{p.beat, p.value, p.curve});
        data[key] = list;
    }
    return data;
}

QJsonObject viewToJson(const AutomationView& view) {
    return {{QStringLiteral("shown"), view.shown},
            {QStringLiteral("key"), optionalString(view.key)},
            {QStringLiteral("lanes"), QJsonArray::fromStringList(view.lanes)}};
}

QJsonObject sendsToJson(const SendMap& sends) {
    QJsonObject data;
    for (auto it = sends.constBegin(); it != sends.constEnd(); ++it) {
        data[it.key()] = QJsonObject{{QStringLiteral("level_db"), it->levelDb},
                                     {QStringLiteral("pre_fader"), it->preFader}};
    }
    return data;
}

void addFreeze(QJsonObject& data, const std::optional<Freeze>& freeze, const QString& base) {
    if (!freeze) return;
    QJsonObject frozen{{QStringLiteral("path"), freeze->path},
                       {QStringLiteral("relative_path"), relative(freeze->path, base)},
                       {QStringLiteral("duration_sec"), freeze->durationSec},
                       {QStringLiteral("tempo"), freeze->tempo}};
    if (freeze->segments) {  // (what of it plays, once a time selection over it was edited)
        QJsonArray segments;
        for (const Clip& segment : *freeze->segments) {
            segments.append(QJsonObject{{QStringLiteral("id"), segment.id},
                                        {QStringLiteral("start_beat"), segment.startBeat},
                                        {QStringLiteral("offset_sec"), segment.offsetSec},
                                        {QStringLiteral("duration_sec"), segment.durationSec}});
        }
        frozen[QStringLiteral("segments")] = segments;
    }
    data[QStringLiteral("frozen")] = frozen;
}

QJsonArray devicesToJson(const std::vector<Device>& devices) {
    QJsonArray list;
    for (const Device& d : devices) list.append(deviceToJson(d));
    return list;
}

QJsonObject midiInputToJson(const MidiInput& input) {
    return {{QStringLiteral("device"), input.device}, {QStringLiteral("channel"), input.channel}};
}

QJsonObject masterToJson(const Track& master) {
    return {{QStringLiteral("volume_db"), master.volumeDb},
            {QStringLiteral("pan"), master.pan},
            {QStringLiteral("devices"), devicesToJson(master.devices)},
            {QStringLiteral("automation"), automationToJson(master.automation)},
            {QStringLiteral("automation_view"), viewToJson(master.automationView)}};
}

// Where a track's output goes, unless into its group (the default: none).
std::optional<QJsonObject> outputToJson(const Output& output) {
    switch (output.to) {
    case Output::To::Group: return std::nullopt;
    case Output::To::Master: return QJsonObject{{QStringLiteral("to"), QStringLiteral("master")}};
    case Output::To::None: return QJsonObject{{QStringLiteral("to"), QStringLiteral("none")}};
    case Output::To::Track:
        return QJsonObject{{QStringLiteral("to"), QStringLiteral("track")}, {QStringLiteral("track"), output.id}};
    case Output::To::Sidechain:
        return QJsonObject{{QStringLiteral("to"), QStringLiteral("sidechain")}, {QStringLiteral("device"), output.id}};
    }
    return std::nullopt;
}

void addOutput(QJsonObject& data, const Output& output) {
    if (const auto json = outputToJson(output)) data[QStringLiteral("output")] = *json;
}

QJsonObject trackToJson(const Track& t, const QString& base) {
    QJsonObject data;
    data[QStringLiteral("id")] = t.id;
    data[QStringLiteral("kind")] = t.kind;
    data[QStringLiteral("name")] = t.nameSource();  // its template: "# Kick"
    data[QStringLiteral("color")] = t.color;
    data[QStringLiteral("volume_db")] = t.volumeDb;
    data[QStringLiteral("pan")] = t.pan;
    data[QStringLiteral("mute")] = t.mute;
    data[QStringLiteral("solo")] = t.solo;
    data[QStringLiteral("height")] = t.height;
    data[QStringLiteral("devices")] = devicesToJson(t.devices);
    QJsonArray clips;
    for (const Clip& c : t.clips) clips.append(clipToJson(c, base));
    data[QStringLiteral("clips")] = clips;
    data[QStringLiteral("automation")] = automationToJson(t.automation);
    data[QStringLiteral("automation_view")] = viewToJson(t.automationView);
    QJsonArray input;
    for (int channel : t.input) input.append(channel);
    data[QStringLiteral("input")] = input;
    if (t.inputTrack) data[QStringLiteral("input_track")] = *t.inputTrack;
    if (t.inputTap != kPostFader) data[QStringLiteral("input_tap")] = t.inputTap;
    if (t.isMidi()) {
        data[QStringLiteral("midi_input")] =
            t.midiInput ? QJsonValue(midiInputToJson(*t.midiInput)) : QJsonValue(QJsonValue::Null);
    }
    data[QStringLiteral("monitor")] = t.monitor;
    data[QStringLiteral("armed")] = t.armed;
    data[QStringLiteral("parent")] = optionalString(t.parent);
    data[QStringLiteral("folded")] = t.folded;
    data[QStringLiteral("sends")] = sendsToJson(t.sends);
    addOutput(data, t.output);
    addFreeze(data, t.frozen, base);
    return data;
}

QJsonObject returnToJson(const Track& t, const QString& base) {
    QJsonObject data{{QStringLiteral("id"), t.id},
                     {QStringLiteral("kind"), t.kind},
                     {QStringLiteral("name"), t.nameSource()},
                     {QStringLiteral("color"), t.color},
                     {QStringLiteral("volume_db"), t.volumeDb},
                     {QStringLiteral("pan"), t.pan},
                     {QStringLiteral("mute"), t.mute},
                     {QStringLiteral("solo"), t.solo},
                     {QStringLiteral("height"), t.height},
                     {QStringLiteral("devices"), devicesToJson(t.devices)},
                     {QStringLiteral("automation"), automationToJson(t.automation)},
                     {QStringLiteral("automation_view"), viewToJson(t.automationView)},
                     {QStringLiteral("sends"), sendsToJson(t.sends)}};
    addOutput(data, t.output);
    addFreeze(data, t.frozen, base);
    return data;
}

// --- Reading ---

// Where a track's output goes (into its group: none there, or one this version doesn't know).
Output outputFromJson(const QJsonValue& value) {
    if (!value.isObject()) return {};
    const QJsonObject data = value.toObject();
    const QString to = data.value(QStringLiteral("to")).toString();
    if (to == u"master") return Output::master();
    if (to == u"none") return Output::none();
    const QJsonValue track = data.value(QStringLiteral("track")), device = data.value(QStringLiteral("device"));
    if (to == u"track" && track.isString()) return Output::track(track.toString());
    if (to == u"sidechain" && device.isString()) return Output::sidechain(device.toString());
    return {};
}

QString inputTapFromJson(const QJsonValue& value) {
    const QString tap = value.toString();
    return tap == kPreFader || tap == kPreFx ? tap : kPostFader;
}

std::optional<Sidechain> sidechainFromJson(const QJsonValue& value) {
    if (!value.isObject()) return std::nullopt;
    const QJsonObject data = value.toObject();
    if (!data.value(QStringLiteral("track")).isString()) return std::nullopt;
    const QJsonValue tap = data.value(QStringLiteral("tap"));
    Sidechain sidechain;
    sidechain.trackId = data.value(QStringLiteral("track")).toString();
    sidechain.tap = (tap.isString() && !tap.toString().isEmpty()) ? tap.toString() : kPostFader;
    return sidechain;
}

MacroMapping macroFromJson(const QJsonObject& m) {
    MacroMapping mapping;
    mapping.macro = toSmallInt(need(m, QStringLiteral("macro")));
    mapping.deviceId = toStr(need(m, QStringLiteral("device")));
    mapping.paramId = toStr(need(m, QStringLiteral("param")));
    mapping.low = m.contains(QStringLiteral("low")) ? toFloat(m.value(QStringLiteral("low"))) : 0.0;
    mapping.high = m.contains(QStringLiteral("high")) ? toFloat(m.value(QStringLiteral("high"))) : 1.0;
    return mapping;
}

Chain chainFromJson(const QJsonValue& value) {
    const QJsonObject c = asObject(value);
    Chain chain;
    chain.id = toStr(need(c, QStringLiteral("id")));
    chain.name = c.contains(QStringLiteral("name")) ? toStr(c.value(QStringLiteral("name"))) : QStringLiteral("Chain");
    for (const QJsonValue& d : listOr(c, QStringLiteral("devices"))) chain.devices.push_back(deviceFromJson(d));
    chain.volumeDb = clamped(c.contains(QStringLiteral("volume_db")) ? toFloat(c.value(QStringLiteral("volume_db"))) : 0.0,
                             automation::kMinVolumeDb, automation::kMaxVolumeDb);
    chain.pan = clamped(c.contains(QStringLiteral("pan")) ? toFloat(c.value(QStringLiteral("pan"))) : 0.0, -1.0, 1.0);
    chain.mute = truthy(c.value(QStringLiteral("mute")));
    chain.solo = truthy(c.value(QStringLiteral("solo")));
    return chain;
}

// Envelopes as saved; targets this version doesn't know are dropped.
EnvelopeMap automationFromJson(const QJsonValue& value) {
    EnvelopeMap envelopes;
    const QJsonObject data = objectOrEmpty(value);
    for (auto it = data.constBegin(); it != data.constEnd(); ++it) {
        if (!automation::isKey(it.key())) continue;
        if (!it->isArray()) damaged(QStringLiteral("%1 isn't a list of points").arg(quoted(it.key())));
        Envelope points;
        for (const QJsonValue& p : it->toArray()) {
            if (!p.isArray()) damaged(QStringLiteral("a point of %1 isn't a list").arg(quoted(it.key())));
            const QJsonArray point = p.toArray();
            if (point.size() < 2) damaged(QStringLiteral("list index out of range"));
            points.push_back({toFloat(point.at(0)), toFloat(point.at(1)), point.size() > 2 ? toFloat(point.at(2)) : 0.0});
        }
        Envelope envelope = automation::normalize(points);
        if (!envelope.empty()) envelopes.insert(it.key(), std::move(envelope));
    }
    return envelopes;
}

AutomationView viewFromJson(const QJsonValue& value) {
    const QJsonObject data = objectOrEmpty(value);
    AutomationView view;
    view.shown = truthy(data.value(QStringLiteral("shown")));
    const QJsonValue key = data.value(QStringLiteral("key"));
    if (key.isString() && !key.toString().isEmpty() && automation::isKey(key.toString())) view.key = key.toString();
    for (const QJsonValue& lane : listOr(data, QStringLiteral("lanes"))) {
        if (lane.isString() && automation::isKey(lane.toString())) view.lanes.append(lane.toString());
    }
    return view;
}

SendMap sendsFromJson(const QJsonValue& value) {
    SendMap sends;
    const QJsonObject data = objectOrEmpty(value);
    for (auto it = data.constBegin(); it != data.constEnd(); ++it) {
        const QJsonObject send = asObject(*it);
        Send s;
        s.levelDb = clamped(send.contains(QStringLiteral("level_db")) ? toFloat(send.value(QStringLiteral("level_db"))) : 0.0,
                            automation::kMinVolumeDb, automation::kMaxVolumeDb);
        s.preFader = truthy(send.value(QStringLiteral("pre_fader")));
        sends.insert(it.key(), s);
    }
    return sends;
}

std::optional<Freeze> freezeFromJson(const QJsonValue& value, const QString& base) {
    if (!value.isObject()) return std::nullopt;
    const QJsonObject data = value.toObject();
    if (!truthy(data.value(QStringLiteral("path")))) return std::nullopt;
    double duration = 0.0;
    double tempo = 0.0;
    try {
        duration = toFloat(need(data, QStringLiteral("duration_sec")));
        tempo = toFloat(need(data, QStringLiteral("tempo")));
    } catch (const Damaged&) {
        return std::nullopt;  // an incomplete one loads unfrozen
    }
    if (duration <= 0 || tempo <= 0) return std::nullopt;
    Freeze freeze{resolveClipPath(data, base), duration, tempo};
    // What of it plays (none, as files from before version 16 have it: all of
    // it). A damaged segment is left out.
    const QJsonValue segments = data.value(QStringLiteral("segments"));
    if (segments.isArray()) {
        std::vector<Clip> clips;
        for (const QJsonValue& value : segments.toArray()) {
            try {
                const QJsonObject segment = asObject(value);
                const double length = toFloat(need(segment, QStringLiteral("duration_sec")));
                const double start = toFloat(need(segment, QStringLiteral("start_beat")));
                const QJsonValue at = segment.value(QStringLiteral("offset_sec"));
                const double offset = at.isUndefined() ? 0.0 : toFloat(at);
                if (!(length > 0) || !std::isfinite(length + start + offset)) continue;
                clips.push_back(freeze.segment(toStr(need(segment, QStringLiteral("id"))), std::max(0.0, start),
                                               std::max(0.0, offset), length));
            } catch (const Damaged&) {
                continue;
            }
        }
        std::stable_sort(clips.begin(), clips.end(),
                         [](const Clip& a, const Clip& b) { return a.startBeat < b.startBeat; });
        freeze.segments = std::move(clips);
    }
    return freeze;
}

QString warpModeOf(const QJsonValue& value) {
    if (!value.isString()) return kDefaultWarpMode;
    const QString name = legacyWarpMode(value.toString());
    return kWarpModes.contains(name) ? name : kDefaultWarpMode;
}

double floatOr(const QJsonObject& data, const QString& key, double fallback) {
    return data.contains(key) ? toFloat(data.value(key)) : fallback;
}

Clip audioClipFromJson(const QJsonValue& value, const QString& base) {
    const QJsonObject c = asObject(value);
    Clip clip;
    clip.kind = Clip::Kind::Audio;
    clip.id = toStr(need(c, QStringLiteral("id")));
    clip.path = resolveClipPath(c, base);
    clip.name = c.contains(QStringLiteral("name")) ? toStr(c.value(QStringLiteral("name")))
                                                   : stemOf(toStr(need(c, QStringLiteral("path"))));
    clip.startBeat = toFloat(need(c, QStringLiteral("start_beat")));
    clip.muted = truthy(c.value(QStringLiteral("muted")));
    clip.durationSec = toFloat(need(c, QStringLiteral("duration_sec")));
    clip.offsetSec = floatOr(c, QStringLiteral("offset_sec"), 0.0);
    clip.sourceDurationSec = floatOr(c, QStringLiteral("source_duration_sec"), 0.0);
    clip.gainDb = floatOr(c, QStringLiteral("gain_db"), 0.0);
    clip.warp = truthy(c.value(QStringLiteral("warp")));
    clip.warpMode = warpModeOf(c.value(QStringLiteral("warp_mode")));
    clip.segmentBpm = floatOr(c, QStringLiteral("segment_bpm"), 0.0);
    clip.transpose = c.contains(QStringLiteral("transpose")) ? toSmallInt(c.value(QStringLiteral("transpose"))) : 0;
    clip.detune = floatOr(c, QStringLiteral("detune"), 0.0);
    clip.pan = floatOr(c, QStringLiteral("pan"), 0.0);
    clip.fadeInSec = floatOr(c, QStringLiteral("fade_in_sec"), 0.0);
    clip.fadeInCurve = floatOr(c, QStringLiteral("fade_in_curve"), 0.0);
    clip.fadeOutSec = floatOr(c, QStringLiteral("fade_out_sec"), 0.0);
    clip.fadeOutCurve = floatOr(c, QStringLiteral("fade_out_curve"), 0.0);
    clip.fitFades();
    if (truthy(c.value(QStringLiteral("reversed_from")))) {
        clip.reversedFrom =
            resolveClipPath(c, base, QStringLiteral("reversed_from"), QStringLiteral("reversed_from_relative"));
    }
    return clip;
}

Clip midiClipFromJson(const QJsonValue& value) {
    const QJsonObject c = asObject(value);
    std::vector<Note> notes;
    for (const QJsonValue& entry : listOr(c, QStringLiteral("notes"))) {
        if (!entry.isArray() || entry.toArray().size() < 4 || entry.toArray().size() > 5) {
            damaged(QStringLiteral("a note isn't [pitch, start, length, velocity] (and deactivated)"));
        }
        const QJsonArray n = entry.toArray();
        const double length = toFloat(n.at(2));
        if (length > 0) {
            notes.push_back(Note{static_cast<int>(std::clamp<long long>(toInt(n.at(0)), 0, 127)),
                                 std::max(0.0, toFloat(n.at(1))), length,
                                 static_cast<int>(std::clamp<long long>(toInt(n.at(3)), 1, 127)),
                                 n.size() > 4 && truthy(n.at(4))});
        }
    }
    Clip clip;
    clip.kind = Clip::Kind::Midi;
    clip.id = toStr(need(c, QStringLiteral("id")));
    clip.startBeat = toFloat(need(c, QStringLiteral("start_beat")));  // (no name: one saved earlier is dropped)
    clip.muted = truthy(c.value(QStringLiteral("muted")));
    clip.durationBeats = toFloat(need(c, QStringLiteral("duration_beats")));
    clip.offsetBeats = floatOr(c, QStringLiteral("offset_beats"), 0.0);
    clip.notes = notes::normalize(notes);
    return clip;
}

std::optional<MidiInput> midiInputFromJson(const QJsonValue& value) {
    if (value.isNull()) return std::nullopt;
    const QJsonObject data = value.isUndefined() ? QJsonObject() : asObject(value);
    MidiInput input;
    input.device = data.contains(QStringLiteral("device")) ? toStr(data.value(QStringLiteral("device"))) : QString();
    const int channel = data.contains(QStringLiteral("channel")) ? toSmallInt(data.value(QStringLiteral("channel"))) : 0;
    input.channel = (channel >= 0 && channel <= 16) ? channel : 0;
    return input;
}

std::vector<int> inputFromJson(const QJsonValue& value) {
    std::vector<int> channels;
    if (truthy(value)) {
        if (!value.isArray()) damaged(QStringLiteral("an input isn't a list of channels"));
        for (const QJsonValue& c : value.toArray()) {
            const int channel = toSmallInt(c);
            if (channel >= 0) channels.push_back(channel);
        }
    }
    return channels.size() <= 2 ? channels : std::vector<int>{};
}

std::vector<Device> devicesFromJson(const QJsonObject& data) {
    std::vector<Device> devices;
    for (const QJsonValue& d : listOr(data, QStringLiteral("devices"))) devices.push_back(deviceFromJson(d));
    return devices;
}

Track masterFromJson(const QJsonObject& data) {
    Track master = newMaster();
    master.volumeDb = floatOr(data, QStringLiteral("volume_db"), 0.0);
    master.pan = clamped(floatOr(data, QStringLiteral("pan"), 0.0), -1.0, 1.0);
    master.devices = devicesFromJson(data);  // none before version 5
    master.automation = automationFromJson(data.value(QStringLiteral("automation")));
    master.automationView = viewFromJson(data.value(QStringLiteral("automation_view")));
    return master;
}

Track returnFromJson(const QJsonValue& value, const QString& base) {
    const QJsonObject t = asObject(value);
    Track track;
    track.id = toStr(need(t, QStringLiteral("id")));
    track.name = toStr(need(t, QStringLiteral("name")));
    track.color = toStr(need(t, QStringLiteral("color")));
    track.kind = kReturnKind;
    track.volumeDb = floatOr(t, QStringLiteral("volume_db"), 0.0);
    track.pan = floatOr(t, QStringLiteral("pan"), 0.0);
    track.mute = truthy(t.value(QStringLiteral("mute")));
    track.solo = truthy(t.value(QStringLiteral("solo")));
    track.height = t.contains(QStringLiteral("height")) ? toSmallInt(t.value(QStringLiteral("height"))) : kDefaultTrackHeight;
    track.devices = devicesFromJson(t);
    track.automation = automationFromJson(t.value(QStringLiteral("automation")));
    track.automationView = viewFromJson(t.value(QStringLiteral("automation_view")));
    track.sends = sendsFromJson(t.value(QStringLiteral("sends")));
    track.output = outputFromJson(t.value(QStringLiteral("output")));
    track.frozen = freezeFromJson(t.value(QStringLiteral("frozen")), base);
    return track;
}

QByteArray readFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        throw ProjectFileError(
            QStringLiteral("Could not read %1: %2").arg(QFileInfo(path).fileName(), file.errorString()));
    }
    return file.readAll();
}

QJsonValue parseJson(const QByteArray& bytes, const QString& path) {
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError) {
        throw ProjectFileError(
            QStringLiteral("Could not read %1: %2").arg(QFileInfo(path).fileName(), error.errorString()));
    }
    if (document.isObject()) return document.object();
    if (document.isArray()) return document.array();
    return QJsonValue();
}

// Writes `data` to `path` without ever leaving a half-written file behind.
void writeJson(const QJsonObject& data, const QString& path) {
    QString error;
    if (!writeJsonFile(path, data, &error)) {
        throw ProjectFileError(QStringLiteral("Could not save %1: %2").arg(QFileInfo(path).fileName(), error));
    }
}

}  // namespace

// --- Devices ---

QJsonObject deviceToJson(const Device& device) {
    QJsonObject params;
    for (auto it = device.params.constBegin(); it != device.params.constEnd(); ++it) params[it.key()] = it.value();
    QJsonObject data{{QStringLiteral("id"), device.id},
                     {QStringLiteral("kind"), device.kind},
                     {QStringLiteral("enabled"), device.enabled},
                     {QStringLiteral("params"), params}};
    if (device.plugin) {
        const PluginRef& p = *device.plugin;
        data[QStringLiteral("plugin")] = QJsonObject{{QStringLiteral("format"), p.format},
                                                     {QStringLiteral("uid"), p.uid},
                                                     {QStringLiteral("name"), p.name},
                                                     {QStringLiteral("vendor"), p.vendor},
                                                     {QStringLiteral("path"), p.path},
                                                     {QStringLiteral("instrument"), p.instrument}};
        data[QStringLiteral("state")] = optionalString(device.state);
    } else if (device.state) {
        data[QStringLiteral("state")] = *device.state;
    }
    if (device.sidechain) {
        data[QStringLiteral("sidechain")] = QJsonObject{{QStringLiteral("track"), device.sidechain->trackId},
                                                        {QStringLiteral("tap"), device.sidechain->tap}};
    }
    if (device.isRack()) {
        QJsonArray chains;
        for (const Chain& c : device.chains) {
            chains.append(QJsonObject{{QStringLiteral("id"), c.id},
                                      {QStringLiteral("name"), c.name},
                                      {QStringLiteral("volume_db"), c.volumeDb},
                                      {QStringLiteral("pan"), c.pan},
                                      {QStringLiteral("mute"), c.mute},
                                      {QStringLiteral("solo"), c.solo},
                                      {QStringLiteral("devices"), devicesToJson(c.devices)}});
        }
        data[QStringLiteral("chains")] = chains;
        QJsonArray macros;
        for (const MacroMapping& m : device.macros) {
            macros.append(QJsonObject{{QStringLiteral("macro"), m.macro},
                                      {QStringLiteral("device"), m.deviceId},
                                      {QStringLiteral("param"), m.paramId},
                                      {QStringLiteral("low"), m.low},
                                      {QStringLiteral("high"), m.high}});
        }
        data[QStringLiteral("macros")] = macros;
        data[QStringLiteral("macro_names")] = QJsonArray::fromStringList(
            QStringList(device.macroNames.begin(), device.macroNames.end()));
        if (device.name && !device.name->isEmpty()) data[QStringLiteral("name")] = *device.name;
    }
    return data;
}

Device deviceFromJson(const QJsonValue& value) {
    const QJsonObject d = asObject(value);
    Device device;
    if (d.contains(QStringLiteral("plugin"))) {
        const QJsonObject p = asObject(d.value(QStringLiteral("plugin")));
        PluginRef plugin;
        plugin.format = p.contains(QStringLiteral("format")) ? toStr(p.value(QStringLiteral("format"))) : QStringLiteral("VST3");
        plugin.uid = toStr(need(p, QStringLiteral("uid")));
        plugin.name = p.contains(QStringLiteral("name")) ? toStr(p.value(QStringLiteral("name"))) : QStringLiteral("Plug-in");
        plugin.vendor = p.contains(QStringLiteral("vendor")) ? toStr(p.value(QStringLiteral("vendor"))) : QString();
        plugin.path = p.contains(QStringLiteral("path")) ? toStr(p.value(QStringLiteral("path"))) : QString();
        plugin.instrument = truthy(p.value(QStringLiteral("instrument")));
        device.plugin = plugin;
    }
    device.id = toStr(need(d, QStringLiteral("id")));
    device.kind = toStr(need(d, QStringLiteral("kind")));
    device.enabled = d.contains(QStringLiteral("enabled")) ? truthy(d.value(QStringLiteral("enabled"))) : true;
    const QJsonObject params = objectOr(d, QStringLiteral("params"));
    for (auto it = params.constBegin(); it != params.constEnd(); ++it) device.params.insert(it.key(), toFloat(*it));
    device.state = stringOrNone(d.value(QStringLiteral("state")));
    device.sidechain = sidechainFromJson(d.value(QStringLiteral("sidechain")));
    if (device.isRack()) {
        for (const QJsonValue& c : listOr(d, QStringLiteral("chains"))) device.chains.push_back(chainFromJson(c));
        QSet<QString> inside;
        for (const Chain& c : device.chains) {
            for (const Device* inner : iterDevices(c.devices)) inside.insert(inner->id);
        }
        inside.remove(device.id);
        std::vector<MacroMapping> mappings;
        for (const QJsonValue& m : listOr(d, QStringLiteral("macros"))) {
            const QJsonObject mapping = asObject(m);
            const QString target = toStr(mapping.contains(QStringLiteral("device")) ? mapping.value(QStringLiteral("device"))
                                                                                   : QJsonValue(QJsonValue::Null));
            if (!inside.contains(target)) continue;
            const long long macro =
                mapping.contains(QStringLiteral("macro")) ? toInt(mapping.value(QStringLiteral("macro"))) : -1;
            if (macro >= 0 && macro < kMaxMacroCount) mappings.push_back(macroFromJson(mapping));
        }
        // Its macros, named; before version 18 a rack had eight: as many as it
        // uses (mapped, or turned), and at least the default.
        if (d.contains(QStringLiteral("macro_names"))) {
            for (const QJsonValue& name : listOr(d, QStringLiteral("macro_names"))) {
                device.macroNames.push_back(name.isString() ? name.toString().trimmed() : QString());
            }
        } else {
            int used = kDefaultMacroCount;
            for (const MacroMapping& mapping : mappings) used = std::max(used, mapping.macro + 1);
            for (auto it = device.params.constBegin(); it != device.params.constEnd(); ++it) {
                const auto macro = macroIndex(it.key());
                if (macro && it.value() != 0.0) used = std::max(used, *macro + 1);
            }
            device.macroNames.resize(static_cast<size_t>(used));
        }
        if (device.macroNames.empty()) device.macroNames.resize(kDefaultMacroCount);
        if (device.macroNames.size() > static_cast<size_t>(kMaxMacroCount)) device.macroNames.resize(kMaxMacroCount);
        const int count = macroCount(device);
        for (const MacroMapping& mapping : mappings) {
            if (mapping.macro < count) device.macros.push_back(mapping);
        }
        // A value for each macro (0: as a new one), none for those it hasn't.
        for (const QString& param : device.params.keys()) {
            const auto macro = macroIndex(param);
            if (macro && *macro >= count) device.params.remove(param);
        }
        for (int i = 0; i < count; ++i) {
            if (!device.params.contains(macroParam(i))) device.params.insert(macroParam(i), 0.0);
        }
        const QJsonValue name = d.value(QStringLiteral("name"));
        if (name.isString() && !name.toString().trimmed().isEmpty()) device.name = name.toString();  // (none before version 13)
    }
    return device;
}

// --- Projects ---

void repairRouting(std::vector<Track>& tracks, std::vector<Track>& returns, Track* master) {
    QSet<QString> ids;
    for (const Track& r : returns) ids.insert(r.id);
    for (Track& track : tracks) {
        SendMap kept;
        for (auto it = track.sends.constBegin(); it != track.sends.constEnd(); ++it) {
            if (ids.contains(it.key())) kept.insert(it.key(), it.value());
        }
        track.sends = kept;
    }
    std::vector<SendMap> saved;
    for (const Track& ret : returns) saved.push_back(ret.sends);
    std::vector<std::optional<QString>> inputs;
    for (const Track& t : tracks) inputs.push_back(t.inputTrack);
    // Made again in order: each checked against those before it.
    for (Track& t : tracks) t.inputTrack.reset();
    for (Track& t : returns) t.inputTrack.reset();
    for (Track& ret : returns) ret.sends.clear();
    for (size_t i = 0; i < returns.size(); ++i) {
        for (auto it = saved[i].constBegin(); it != saved[i].constEnd(); ++it) {
            if (ids.contains(it.key()) && !wouldCycle(tracks, returns, returns[i].id, it.key())) {
                returns[i].sends.insert(it.key(), it.value());
            }
        }
    }
    // Outputs into the master, or nowhere, are kept; into a track or a device's
    // sidechain if it is there (the track an audio track, not the one's own
    // group), one by one unless they close a cycle with those before them.
    std::vector<Output> outputs;
    for (auto* list : {&tracks, &returns}) {
        for (Track& t : *list) {
            outputs.push_back(t.output);
            t.output = {};
        }
    }
    QSet<QString> audioTracks, deviceIds;
    for (const Track& t : tracks) {
        if (t.isAudio()) audioTracks.insert(t.id);
    }
    for (const auto* list : {&tracks, &returns}) {
        for (const Track& t : *list) {
            for (const Device* d : iterDevices(t.devices)) deviceIds.insert(d->id);
        }
    }
    if (master != nullptr) {
        for (const Device* d : iterDevices(master->devices)) deviceIds.insert(d->id);
    }
    {
        size_t i = 0;
        for (auto* list : {&tracks, &returns}) {
            for (Track& t : *list) {
                Output output = outputs[i++];
                if (output.to == Output::To::Master && !t.parent) output = {};  // (the same)
                if (output.to == Output::To::Track && t.parent == output.id) output = {};
                const bool there = output.to == Output::To::Track       ? audioTracks.contains(output.id) && output.id != t.id
                                   : output.to == Output::To::Sidechain ? deviceIds.contains(output.id)
                                                                        : true;
                if (there && !outputWouldCycle(tracks, returns, t.id, output)) t.output = output;
            }
        }
    }
    QSet<QString> sources = ids;
    for (const Track& t : tracks) sources.insert(t.id);
    sources.insert(kMaster);
    for (size_t i = 0; i < tracks.size(); ++i) {
        const auto& source = inputs[i];
        if (source && sources.contains(*source) && tracks[i].isAudio() &&
            (*source == kMaster || !feeds(routingGraph(tracks, returns), tracks[i].id, *source))) {
            tracks[i].inputTrack = source;
            tracks[i].input.clear();
        }
    }
    struct Sidechained {
        QString owner;
        Device* device;
        Sidechain sidechain;
    };
    std::vector<Sidechained> sidechained;
    auto collect = [&](Track& owner, const QString& ownerId) {
        for (Device* device : iterDevices(owner.devices)) {
            if (device->sidechain) sidechained.push_back({ownerId, device, *device->sidechain});
        }
    };
    for (Track& t : tracks) collect(t, t.id);
    for (Track& t : returns) collect(t, t.id);
    if (master != nullptr) collect(*master, kMaster);
    for (const Sidechained& entry : sidechained) entry.device->sidechain.reset();
    for (const Sidechained& entry : sidechained) {
        const QString& source = entry.sidechain.trackId;
        if (source != kMaster && sources.contains(source) &&
            !sidechainWouldCycle(tracks, returns, entry.owner, source)) {
            entry.device->sidechain = entry.sidechain;
        }
    }
}

QJsonObject projectToJson(const Project& project, const QString& projectFile) {
    const QString base = baseOf(projectFile);
    QSet<QString> deviceIds;
    for (const Track* t : project.allTracks()) {
        for (const Device* d : iterDevices(t->devices)) deviceIds.insert(d->id);
    }
    // View state of the devices there are, sorted.
    const auto present = [&](const QSet<QString>& ids) {
        QStringList kept;
        for (const QString& id : ids) {
            if (deviceIds.contains(id)) kept.append(id);
        }
        kept.sort();
        return QJsonArray::fromStringList(kept);
    };
    QJsonArray tracks;
    for (const Track& t : project.tracks()) tracks.append(trackToJson(t, base));
    QJsonArray returns;
    for (const Track& t : project.returns()) returns.append(returnToJson(t, base));
    const auto& key = project.key();
    return {{QStringLiteral("format"), kProjectFormat},
            {QStringLiteral("version"), kProjectVersion},
            {QStringLiteral("tempo"), project.tempo()},
            {QStringLiteral("key"), key ? QJsonValue(key->name()) : QJsonValue(QJsonValue::Null)},
            {QStringLiteral("time_signature"),
             QJsonArray{project.timeSignature().numerator, project.timeSignature().denominator}},
            {QStringLiteral("loop"), QJsonObject{{QStringLiteral("enabled"), project.loopEnabled()},
                                                 {QStringLiteral("start"), project.loopStart()},
                                                 {QStringLiteral("end"), project.loopEnd()}}},
            {QStringLiteral("automation_locked"), project.automationLocked()},
            {QStringLiteral("master"), masterToJson(project.master())},
            {QStringLiteral("folded_devices"), present(project.foldedDevices())},
            {QStringLiteral("chain_lists_shown"), present(project.shownChainLists())},
            {QStringLiteral("rack_devices_hidden"), present(project.hiddenRackDevices())},
            {QStringLiteral("tracks"), tracks},
            {QStringLiteral("returns"), returns}};
}

std::vector<Track> tracksFromJson(const QJsonObject& data, const QString& projectFile) {
    const QString base = baseOf(projectFile);
    const QJsonValue version = data.value(QStringLiteral("version"));
    const bool plainNames = (version.isUndefined() ? 0 : toInt(version)) < kNameTemplatesVersion;
    std::vector<Track> tracks;
    for (const QJsonValue& value : listOr(data, QStringLiteral("tracks"))) {
        const QJsonObject t = asObject(value);
        const QString kind = t.contains(QStringLiteral("kind")) ? toStr(t.value(QStringLiteral("kind"))) : kAudioKind;
        if (!kTrackKinds.contains(kind)) damaged(QStringLiteral("unknown track kind %1").arg(quoted(kind)));
        Track track;
        track.id = toStr(need(t, QStringLiteral("id")));
        track.name = toStr(need(t, QStringLiteral("name")));
        track.color = toStr(need(t, QStringLiteral("color")));
        track.volumeDb = floatOr(t, QStringLiteral("volume_db"), 0.0);
        track.pan = floatOr(t, QStringLiteral("pan"), 0.0);
        track.mute = truthy(t.value(QStringLiteral("mute")));
        track.solo = truthy(t.value(QStringLiteral("solo")));
        const int defaultHeight = kind == kGroupKind ? kDefaultGroupHeight : kDefaultTrackHeight;
        track.height =
            t.contains(QStringLiteral("height")) ? toSmallInt(t.value(QStringLiteral("height"))) : defaultHeight;
        track.devices = devicesFromJson(t);
        if (kind != kGroupKind) {
            for (const QJsonValue& c : listOr(t, QStringLiteral("clips"))) {
                track.clips.push_back(kind == kMidiKind ? midiClipFromJson(c) : audioClipFromJson(c, base));
            }
            std::stable_sort(track.clips.begin(), track.clips.end(),
                             [](const Clip& a, const Clip& b) { return a.startBeat < b.startBeat; });
        }
        track.kind = kind;
        track.automation = automationFromJson(t.value(QStringLiteral("automation")));
        track.automationView = viewFromJson(t.value(QStringLiteral("automation_view")));
        track.input = inputFromJson(t.value(QStringLiteral("input")));
        track.inputTrack = stringOrNone(t.value(QStringLiteral("input_track")));
        track.inputTap = inputTapFromJson(t.value(QStringLiteral("input_tap")));
        // MIDI tracks saved before there was MIDI input hear every input.
        track.midiInput = kind == kMidiKind ? midiInputFromJson(t.value(QStringLiteral("midi_input"))) : MidiInput{};
        const QJsonValue monitor = t.value(QStringLiteral("monitor"));
        track.monitor = monitor.isString() && kMonitorModes.contains(monitor.toString()) ? monitor.toString()
                                                                                        : QStringLiteral("auto");
        track.armed = truthy(t.value(QStringLiteral("armed"))) && kind != kGroupKind;
        track.parent = stringOrNone(t.value(QStringLiteral("parent")));
        track.folded = truthy(t.value(QStringLiteral("folded")));
        track.sends = sendsFromJson(t.value(QStringLiteral("sends")));
        track.output = outputFromJson(t.value(QStringLiteral("output")));
        track.frozen = freezeFromJson(t.value(QStringLiteral("frozen")), base);
        // Saved before tracks were numbered by their place, as new ones were named ("3 Audio"): so now.
        // (Since then, such a name is one typed: it stays.)
        if (plainNames && hasPlainName(track)) track.nameTemplate = plainNameTemplate(track);
        tracks.push_back(std::move(track));
    }
    repairTree(tracks);
    return tracks;
}

std::vector<Track> returnsFromJson(const QJsonObject& data, const QString& projectFile) {
    const QString base = baseOf(projectFile);
    std::vector<Track> returns;
    for (const QJsonValue& t : listOr(data, QStringLiteral("returns"))) returns.push_back(returnFromJson(t, base));
    return returns;
}

namespace {

ProjectContents contentsFromJson(const QJsonObject& data, const QString& projectFile) {
    if (data.value(QStringLiteral("format")) != QJsonValue(kProjectFormat)) {
        throw ProjectFileError(QStringLiteral("Not a SUBstation project"));
    }
    const QJsonValue version = data.value(QStringLiteral("version"));
    if ((version.isUndefined() ? 0 : toInt(version)) > kProjectVersion) {
        throw ProjectFileError(QStringLiteral("This project was saved by a newer version of SUBstation"));
    }
    TimeSignature ts;
    if (data.contains(QStringLiteral("time_signature"))) {
        const QJsonValue value = data.value(QStringLiteral("time_signature"));
        if (!value.isArray() || value.toArray().size() != 2) damaged(QStringLiteral("a time signature is two numbers"));
        ts = {toSmallInt(value.toArray().at(0)), toSmallInt(value.toArray().at(1))};
    }
    const QJsonObject loop = objectOr(data, QStringLiteral("loop"));
    ProjectContents contents;
    contents.tracks = tracksFromJson(data, projectFile);
    contents.returns = returnsFromJson(data, projectFile);
    Track master = masterFromJson(objectOr(data, QStringLiteral("master")));
    repairRouting(contents.tracks, contents.returns, &master);
    contents.master = std::move(master);
    contents.tempo = floatOr(data, QStringLiteral("tempo"), 120.0);
    contents.timeSignature = ts;
    contents.loopEnabled = truthy(loop.value(QStringLiteral("enabled")));
    contents.loopStart = floatOr(loop, QStringLiteral("start"), 0.0);
    contents.loopEnd = floatOr(loop, QStringLiteral("end"), 16.0);
    contents.automationLocked = truthy(data.value(QStringLiteral("automation_locked")));
    const QJsonValue key = data.value(QStringLiteral("key"));
    contents.key = key.isString() ? keyFromName(key.toString()) : std::nullopt;
    for (const QJsonValue& id : listOr(data, QStringLiteral("folded_devices"))) contents.foldedDevices.insert(toStr(id));
    for (const QJsonValue& id : listOr(data, QStringLiteral("chain_lists_shown"))) contents.shownChainLists.insert(toStr(id));
    for (const QJsonValue& id : listOr(data, QStringLiteral("rack_devices_hidden"))) {
        contents.hiddenRackDevices.insert(toStr(id));
    }
    contents.path = projectFile;
    return contents;
}

}  // namespace

void loadInto(Project& project, const QJsonObject& data, const QString& projectFile) {
    ProjectContents contents;
    try {
        contents = contentsFromJson(data, projectFile);
    } catch (const Damaged& error) {
        throw ProjectFileError(QStringLiteral("The project is damaged: %1").arg(error.detail()));
    }
    project.replaceContents(std::move(contents));
}

void saveProject(Project& project, const QString& path) {
    writeJson(projectToJson(project, path), path);
    project.setPath(path);
}

void loadProject(Project& project, const QString& path) {
    const QJsonValue data = parseJson(readFile(path), path);
    if (!data.isObject()) throw ProjectFileError(QStringLiteral("Not a SUBstation project"));
    ProjectContents contents;
    try {
        contents = contentsFromJson(data.toObject(), path);
    } catch (const Damaged& error) {
        throw ProjectFileError(QStringLiteral("%1 is damaged: %2").arg(QFileInfo(path).fileName(), error.detail()));
    }
    project.replaceContents(std::move(contents));
}

void saveTemplate(const Project& project, const QString& path) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    writeJson(projectToJson(project), path);  // (its files by their absolute paths only: it moves nowhere)
}

void loadTemplate(Project& project, const QString& path) {
    const QJsonValue data = parseJson(readFile(path), path);
    if (!data.isObject()) throw ProjectFileError(QStringLiteral("The template is not a SUBstation project"));
    ProjectContents contents;
    try {
        contents = contentsFromJson(data.toObject(), QString());  // untitled: no file of its own
    } catch (const Damaged& error) {
        throw ProjectFileError(QStringLiteral("The template is damaged: %1").arg(error.detail()));
    }
    project.replaceContents(std::move(contents));
}

// --- Presets ---

QJsonObject deviceToPreset(const Device& device) {
    std::function<void(QJsonObject&)> strip = [&](QJsonObject& d) {
        d.remove(QStringLiteral("sidechain"));
        if (!d.contains(QStringLiteral("chains"))) return;
        QJsonArray chains = d.value(QStringLiteral("chains")).toArray();
        for (qsizetype c = 0; c < chains.size(); ++c) {
            QJsonObject chain = chains.at(c).toObject();
            QJsonArray devices = chain.value(QStringLiteral("devices")).toArray();
            for (qsizetype i = 0; i < devices.size(); ++i) {
                QJsonObject inner = devices.at(i).toObject();
                strip(inner);
                devices.replace(i, inner);
            }
            chain[QStringLiteral("devices")] = devices;
            chains.replace(c, chain);
        }
        d[QStringLiteral("chains")] = chains;
    };
    QJsonObject data = deviceToJson(device);
    strip(data);
    return {{QStringLiteral("format"), kPresetFormat},
            {QStringLiteral("version"), kPresetVersion},
            {QStringLiteral("device"), data}};
}

Device presetDevice(const QJsonValue& data) {
    if (!data.isObject() || data.toObject().value(QStringLiteral("format")) != QJsonValue(kPresetFormat)) {
        throw ProjectFileError(QStringLiteral("Not a SUBstation preset"));
    }
    const QJsonObject preset = data.toObject();
    Device device;
    try {
        const QJsonValue version = preset.value(QStringLiteral("version"));
        if ((version.isUndefined() ? 0 : toInt(version)) > kPresetVersion) {
            throw ProjectFileError(QStringLiteral("This preset was saved by a newer version of SUBstation"));
        }
        device = deviceFromJson(need(preset, QStringLiteral("device")));
    } catch (const Damaged& error) {
        throw ProjectFileError(QStringLiteral("The preset is damaged: %1").arg(error.detail()));
    }
    if (rackHeight(device) > kMaxRackDepth) throw ProjectFileError(QStringLiteral("The preset nests racks too deep"));
    std::vector<Device> holder;
    holder.push_back(std::move(device));
    for (Device* inner : iterDevices(holder)) inner->sidechain.reset();
    refreshIds(holder.front());
    return std::move(holder.front());
}

void savePreset(const Device& device, const QString& path) { writeJson(deviceToPreset(device), path); }

Device loadPreset(const QString& path) {
    Device device = presetDevice(parseJson(readFile(path), path));
    if (device.isRack()) device.name = stemOf(path);  // (the preset's name, renamed or not)
    return device;
}

}  // namespace sub::app
