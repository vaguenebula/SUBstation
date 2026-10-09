// Ableton Live Set -> SUBstation project (see LiveImport.h for what comes
// across). Two passes over the set's tracks: the first gives every track (and
// every Drum Rack pad that will be a track) its id, so routing to tracks further
// down resolves; the second translates them. Automation comes last: every
// parameter translated says which of Live's automation targets it is
// (`targets_`), and each envelope goes to the track holding its target.

#include "io/LiveImport.h"

#include "io/Json.h"
#include "io/LiveSet.h"
#include "io/Serialization.h"
#include "model/Automation.h"
#include "model/Clip.h"
#include "model/Device.h"
#include "model/DeviceState.h"
#include "model/Devices.h"
#include "model/Ids.h"
#include "model/Keys.h"
#include "model/ParamSpec.h"
#include "model/Track.h"
#include "plugins/Vst3Ids.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <utility>

namespace sub::app::live {

namespace {

// Live's 70 track and clip colours: a Color is an index into them.
constexpr const char* kLiveColors[] = {
    "#ff94a6", "#ffa529", "#cc9927", "#f7f47c", "#bffb00", "#1aff2f", "#25ffa8", "#5cffe8", "#8bc5ff", "#5480e4",
    "#92a7ff", "#d86ce4", "#e553a0", "#ffffff", "#ff3636", "#f66c03", "#99724b", "#fff034", "#87ff67", "#3dc300",
    "#00bfaf", "#19e9ff", "#10a4ee", "#007dc0", "#886ce4", "#b677c6", "#ff39d4", "#d0d0d0", "#e2675a", "#ffa374",
    "#d3ad71", "#edffae", "#d2e498", "#bad074", "#9bc48d", "#d4fde1", "#cdf1f8", "#b9c1e3", "#cdbbe4", "#ae98e5",
    "#e5dce1", "#a9a9a9", "#c6928b", "#b78256", "#99836a", "#bfba69", "#a6be00", "#7db04d", "#88c2ba", "#9bb3c4",
    "#85a5c2", "#8393cc", "#a595b5", "#bf9fbe", "#bc7196", "#7b7b7b", "#af3333", "#a95131", "#724f41", "#dbc300",
    "#85961f", "#539f31", "#0a9c8e", "#236384", "#1a2f96", "#2f52a2", "#624bad", "#a34bad", "#cc2e6e", "#3c3c3c"};

// An element's colour index: Color (Live 11 and later) or ColorIndex (Live 10).
int colorOf(const Element& element) { return element.integer(u"Color", element.integer(u"ColorIndex", 0)); }

QString liveColor(int index) {
    constexpr int count = static_cast<int>(sizeof(kLiveColors) / sizeof(kLiveColors[0]));
    return index >= 0 && index < count ? QString::fromLatin1(kLiveColors[index]) : kTrackColors.front();
}

// How Live's own devices are called, by their names in the set (for the notes).
QString liveDeviceName(const QString& tag) {
    static const QHash<QString, QString> names{
        {QStringLiteral("AutoFilter2"), QStringLiteral("Auto Filter")},
        {QStringLiteral("AutoFilter"), QStringLiteral("Auto Filter")},
        {QStringLiteral("AutoPan"), QStringLiteral("Auto Pan")},
        {QStringLiteral("AutoPan2"), QStringLiteral("Auto Pan-Tremolo")},
        {QStringLiteral("MultibandDynamics"), QStringLiteral("Multiband Dynamics")},
        {QStringLiteral("Redux2"), QStringLiteral("Redux")},
        {QStringLiteral("Chorus2"), QStringLiteral("Chorus-Ensemble")},
        {QStringLiteral("Reverb"), QStringLiteral("Reverb")},
        {QStringLiteral("Hybrid"), QStringLiteral("Hybrid Reverb")},
        {QStringLiteral("GlueCompressor"), QStringLiteral("Glue Compressor")},
        {QStringLiteral("Tuner"), QStringLiteral("Tuner")},
        {QStringLiteral("Echo"), QStringLiteral("Echo")},
        {QStringLiteral("Saturator"), QStringLiteral("Saturator")},
        {QStringLiteral("Vocoder"), QStringLiteral("Vocoder")},
        {QStringLiteral("Limiter"), QStringLiteral("Limiter")},
        {QStringLiteral("Gate"), QStringLiteral("Gate")},
        {QStringLiteral("DrumBuss"), QStringLiteral("Drum Buss")},
        {QStringLiteral("Roar"), QStringLiteral("Roar")},
        {QStringLiteral("Overdrive"), QStringLiteral("Overdrive")},
        {QStringLiteral("Phaser"), QStringLiteral("Phaser")},
        {QStringLiteral("PhaserNew"), QStringLiteral("Phaser-Flanger")},
        {QStringLiteral("Flanger"), QStringLiteral("Flanger")},
        {QStringLiteral("Erosion"), QStringLiteral("Erosion")},
        {QStringLiteral("Amp"), QStringLiteral("Amp")},
        {QStringLiteral("Cabinet"), QStringLiteral("Cabinet")},
        {QStringLiteral("Pedal"), QStringLiteral("Pedal")},
        {QStringLiteral("Corpus"), QStringLiteral("Corpus")},
        {QStringLiteral("FilterEQ3"), QStringLiteral("EQ Three")},
        {QStringLiteral("Spectrum"), QStringLiteral("Spectrum")},
        {QStringLiteral("Looper"), QStringLiteral("Looper")},
        {QStringLiteral("BeatRepeat"), QStringLiteral("Beat Repeat")},
        {QStringLiteral("GrainDelay"), QStringLiteral("Grain Delay")},
        {QStringLiteral("FilterDelay"), QStringLiteral("Filter Delay")},
        {QStringLiteral("PingPongDelay"), QStringLiteral("Ping Pong Delay")},
        {QStringLiteral("CrossDelay"), QStringLiteral("Simple Delay")},
        {QStringLiteral("Resonator"), QStringLiteral("Resonators")},
        {QStringLiteral("FrequencyShifter"), QStringLiteral("Frequency Shifter")},
        {QStringLiteral("Shifter"), QStringLiteral("Shifter")},
        {QStringLiteral("Transmute"), QStringLiteral("Spectral Resonator")},
        {QStringLiteral("SpectralTime"), QStringLiteral("Spectral Time")},
        {QStringLiteral("ChannelEq"), QStringLiteral("Channel EQ")},
        {QStringLiteral("InstrumentVector"), QStringLiteral("Wavetable")},
        {QStringLiteral("UltraAnalog"), QStringLiteral("Analog")},
        {QStringLiteral("Operator"), QStringLiteral("Operator")},
        {QStringLiteral("Drift"), QStringLiteral("Drift")},
        {QStringLiteral("InstrumentMeld"), QStringLiteral("Meld")},
        {QStringLiteral("Collision"), QStringLiteral("Collision")},
        {QStringLiteral("LoungeLizard"), QStringLiteral("Electric")},
        {QStringLiteral("StringStudio"), QStringLiteral("Tension")},
        {QStringLiteral("InstrumentImpulse"), QStringLiteral("Impulse")},
        {QStringLiteral("MxDeviceAudioEffect"), QStringLiteral("Max for Live audio effect")},
        {QStringLiteral("MxDeviceInstrument"), QStringLiteral("Max for Live instrument")},
        {QStringLiteral("MxDeviceMidiEffect"), QStringLiteral("Max for Live MIDI effect")},
        {QStringLiteral("MidiArpeggiator"), QStringLiteral("Arpeggiator")},
        {QStringLiteral("MidiChord"), QStringLiteral("Chord")},
        {QStringLiteral("MidiNoteLength"), QStringLiteral("Note Length")},
        {QStringLiteral("MidiPitcher"), QStringLiteral("Pitch")},
        {QStringLiteral("MidiRandom"), QStringLiteral("Random")},
        {QStringLiteral("MidiScale"), QStringLiteral("Scale")},
        {QStringLiteral("MidiVelocity"), QStringLiteral("Velocity")},
        {QStringLiteral("MidiEffectGroupDevice"), QStringLiteral("MIDI Effect Rack")},
    };
    return names.value(tag, tag);
}

constexpr double kMinGain = 1e-7;

double gainToDb(double gain) { return gain <= kMinGain ? automation::kMinVolumeDb : 20.0 * std::log10(gain); }

// A fader's dB from Live's linear gain, in SUBstation's faders' range.
double faderDb(double gain) { return std::clamp(gainToDb(gain), automation::kMinVolumeDb, automation::kMaxVolumeDb); }

double volumeNormalized(double gain) { return automation::volumeToNormalized(faderDb(gain)); }

double panNormalized(double pan) { return automation::panToNormalized(std::clamp(pan, -1.0, 1.0)); }

double switchNormalized(double on) { return on >= 0.5 ? 1.0 : 0.0; }

double plainNormalized(double value) { return std::clamp(value, 0.0, 1.0); }

// A built-in device's parameter: a plain value (in its own units) to automation's 0..1.
std::function<double(double)> builtinNormalized(const QString& kind, const QString& paramId,
                                                std::function<double(double)> toPlain) {
    const BuiltinDevice* device = builtinDevice(kind);
    if (!device || !device->info) return {};
    for (const sub::ParamInfo& info : device->info->params) {
        if (QString::fromStdString(info.id) != paramId) continue;
        const ParamSpec spec = ParamSpec::fromInfo(info, QString(), QString());
        return [spec, toPlain](double value) {
            return spec.toNormalized(std::clamp(toPlain(value), spec.minimum, spec.maximum));
        };
    }
    return {};
}

// A 32-bit word Live writes (a class id's, a VST3 ParamID, a VST2's id), as a
// signed or an unsigned number; none if it isn't one.
std::optional<quint32> liveWord(const Element& element, QStringView path) {
    const double value = element.number(path, std::nan(""));
    if (!(value >= -2147483648.0 && value < 4294967296.0) || value != std::floor(value)) return std::nullopt;
    return static_cast<quint32>(static_cast<qint64>(value));
}

QByteArray hexBytes(const QString& text) {
    QString digits;
    digits.reserve(text.size());
    for (const QChar c : text) {
        if (!c.isSpace()) digits.append(c);
    }
    return QByteArray::fromHex(digits.toLatin1());
}

void appendLe32(QByteArray& bytes, quint32 value) {
    char data[4];
    qToLittleEndian(value, data);
    bytes.append(data, 4);
}

void appendLe64(QByteArray& bytes, quint64 value) {
    char data[8];
    qToLittleEndian(value, data);
    bytes.append(data, 8);
}

void appendBe32(QByteArray& bytes, quint32 value) {
    char data[4];
    qToBigEndian(value, data);
    bytes.append(data, 4);
}

// A name for matching a VST2 plug-in with its VST3: lower case, letters and
// digits only, without the "x64" of a 64-bit build ("ValhallaVintageVerb_x64").
QString plainPluginName(QString name) {
    name = name.toLower();
    static const QRegularExpression bits(QStringLiteral("[ _\\-(]*x64\\)?$|[ _\\-(]*64[ _\\-]?bit\\)?$"));
    name.remove(bits);
    QString plain;
    for (const QChar c : name) {
        if (c.isLetterOrNumber()) plain.append(c);
    }
    return plain;
}

// What a clip plays over its length, in its own time (beats if it is warped or
// MIDI, else seconds): from its start marker, round its loop if it loops. Each
// stretch [from, to) is heard `at` into the clip.
struct Span {
    double from = 0.0;
    double to = 0.0;
    double at = 0.0;
};

constexpr int kMostSpans = 4096;
const QString kTooManyLoops =
    QStringLiteral("Some looping clips loop too often to write out: they stop after %1 loops.").arg(kMostSpans);

std::vector<Span> clipSpans(const Element& clip, double length, bool* cut = nullptr) {
    const double loopStart = clip.number(u"Loop/LoopStart");
    const double loopEnd = clip.number(u"Loop/LoopEnd");
    const double start = loopStart + clip.number(u"Loop/StartRelative");
    std::vector<Span> spans;
    if (length <= 0) return spans;
    if (!clip.flag(u"Loop/LoopOn") || loopEnd - loopStart <= 1e-9) {
        spans.push_back({start, start + length, 0.0});
        return spans;
    }
    double from = start < loopEnd ? start : loopStart;
    double at = 0.0;
    while (at < length - 1e-9) {
        if (static_cast<int>(spans.size()) >= kMostSpans) {
            if (cut) *cut = true;
            break;
        }
        const double n = std::min(loopEnd - from, length - at);
        if (n <= 1e-12) break;
        spans.push_back({from, from + n, at});
        at += n;
        from = loopStart;
    }
    return spans;
}

// Content beats of a warped clip -> seconds of its file, as its warp markers
// say (straight lines between them; before the first and after the last, the
// first's and the last stretch's tempo).
class WarpMap {
public:
    explicit WarpMap(const Element& clip, double fallbackTempo) {
        if (const Element* markers = clip.child(u"WarpMarkers")) {
            for (const Element* m : markers->all(u"WarpMarker")) {
                markers_.push_back({m->numberAttribute(u"SecTime"), m->numberAttribute(u"BeatTime")});
            }
        }
        std::sort(markers_.begin(), markers_.end(), [](const Marker& a, const Marker& b) { return a.beat < b.beat; });
        // Markers at one beat: the first.
        markers_.erase(std::unique(markers_.begin(), markers_.end(),
                                   [](const Marker& a, const Marker& b) { return std::abs(a.beat - b.beat) < 1e-9; }),
                       markers_.end());
        if (markers_.empty()) markers_.push_back({0.0, 0.0});
        if (markers_.size() == 1) markers_.push_back({markers_[0].sec + 60.0 / fallbackTempo, markers_[0].beat + 1.0});
    }

    double secondsAt(double beat) const {
        size_t i = 0;
        while (i + 2 < markers_.size() && beat >= markers_[i + 1].beat) ++i;
        const Marker& a = markers_[i];
        const Marker& b = markers_[i + 1];
        return a.sec + (beat - a.beat) * (b.sec - a.sec) / (b.beat - a.beat);
    }

    // The markers' beats strictly between two beats.
    std::vector<double> beatsBetween(double from, double to) const {
        std::vector<double> beats;
        for (const Marker& m : markers_) {
            if (m.beat > from + 1e-9 && m.beat < to - 1e-9) beats.push_back(m.beat);
        }
        return beats;
    }

private:
    struct Marker {
        double sec;
        double beat;
    };
    std::vector<Marker> markers_;
};

// Live's warp modes as SUBstation's.
QString warpMode(int liveMode) {
    switch (liveMode) {
    case 0: return QStringLiteral("Transients");  // Beats
    case 2: return QStringLiteral("Smooth");      // Texture
    case 3: return QStringLiteral("Re-Pitch");
    case 5: return QStringLiteral("Transients");  // REX
    case 6: return QStringLiteral("Formants");    // Complex Pro
    default: return QStringLiteral("Standard");   // Tones, Complex
    }
}

// What the notes say, a line each: plain lines once, lists with how many of each.
class Notes {
public:
    void line(const QString& text) {
        if (!lines_.contains(text)) lines_.append(text);
    }
    void listed(const QString& heading, const QString& item) {
        if (!lists_.contains(heading)) order_.append(heading);
        ++lists_[heading][item];
    }
    QStringList all() const {
        QStringList out = lines_;
        for (const QString& heading : order_) {
            QStringList items;
            const QMap<QString, int>& counts = lists_[heading];
            for (auto it = counts.constBegin(); it != counts.constEnd(); ++it) {
                items.append(it.value() > 1 ? QStringLiteral("%1 (%2)").arg(it.key()).arg(it.value()) : it.key());
            }
            out.append(heading + QStringLiteral(": ") + items.join(QStringLiteral(", ")) + u'.');
        }
        return out;
    }

private:
    QStringList lines_;
    QStringList order_;
    QHash<QString, QMap<QString, int>> lists_;
};

const QString kNoDevice = QStringLiteral("Left out (SUBstation has no device like them)");
const QString kMidiEffects = QStringLiteral("Left out (SUBstation has no MIDI effects)");
const QString kVst2Defaults =
    QStringLiteral("VST2 plug-ins became their installed VST3, at its default settings (set them up again)");
const QString kVst2Missing = QStringLiteral("Left out (VST2 plug-ins, and no VST3 of them is installed)");
const QString kNotInstalled = QStringLiteral("Not installed here (they load once installed: rescan the plug-ins)");

class Importer {
public:
    Importer(const Element& root, const QString& setPath, const ImportOptions& options)
        : set_(*root.child(u"LiveSet")), setDir_(QFileInfo(setPath).absoluteDir()), options_(options) {}

    ImportResult run();

private:
    // A Live track's pads that become tracks: (branch index, receiving note, sending note, id).
    struct Pad {
        int branch = 0;
        int note = 0;
        int sends = 60;
        QString id;
        const Element* element = nullptr;
    };
    // An automation target of Live's: whose automation it goes in, as which key, and its values' mapping.
    struct Target {
        QString owner;
        QString key;
        std::function<double(double)> normalized;
    };

    void readSettings();
    void assignIds();
    QJsonObject translateTrack(const Element& track, const QString& id, const std::optional<QString>& parent);
    void translateDrumTrack(const Element& track, const Element& drums, const QString& id,
                            const std::optional<QString>& parent);
    QJsonObject translateReturn(const Element& track, const QString& id);
    QJsonObject translateMaster(const Element& track);
    void translateMixer(const Element& mixer, const QString& owner, QJsonObject& data, bool master = false);
    void addSends(const Element& mixer, const QString& owner, QJsonObject& data);
    void addRouting(const Element& track, QJsonObject& data, bool inGroup);
    QJsonArray translateDevices(const Element* devices, const QString& owner);
    std::optional<QJsonObject> translateDevice(const Element& device, const QString& owner);
    std::optional<QJsonObject> plugin(const Element& device, const QString& owner, const QString& id);
    std::optional<QJsonObject> rack(const Element& device, const QString& owner, const QString& id);
    static QJsonObject builtin(const QString& kind, const QString& id, const QJsonObject& params);
    std::optional<QJsonObject> utility(const Element& device, const QString& owner, const QString& id);
    std::optional<QJsonObject> eq(const Element& device, const QString& owner, const QString& id);
    std::optional<QJsonObject> compressor(const Element& device, const QString& owner, const QString& id);
    std::optional<QJsonObject> delay(const Element& device, const QString& owner, const QString& id);
    std::optional<QJsonObject> sampler(const Element& device, const QString& owner, const QString& id);
    std::optional<QJsonObject> sidechainOf(const Element& device);
    QJsonArray clips(const Element& track, bool midi, const std::function<std::optional<int>(int)>& pitch = {});
    std::optional<QJsonObject> midiClip(const Element& clip, const std::function<std::optional<int>(int)>& pitch);
    void audioClips(const Element& clip, QJsonArray& out);
    // Clip envelopes are left out: noted, a clip each.
    void noteClipEnvelopes(const Element& track);
    QString samplePath(const Element* sampleRef);
    QString resolveSample(const QString& path, const QString& relative);
    void addTarget(const Element* parameter, const QString& owner, const QString& key,
                   std::function<double(double)> normalized);
    void addBuiltinTarget(const Element* parameter, const QString& owner, const QString& kind, const QString& deviceId,
                          const QString& paramId, std::function<double(double)> toPlain);
    void readAutomation(const Element& track);
    std::optional<QString> routedTrack(const QString& target, QString* tap);
    QString trackName(const Element& track, const QString& fallback) const;
    // Its name as Live shows it ("4-Serum"), for the notes.
    static QString liveName(const Element& track);

    const Element& set_;
    QDir setDir_;
    const ImportOptions& options_;
    Notes notes_;
    double tempo_ = 120.0;
    int numerator_ = 4;
    int denominator_ = 4;
    QHash<QString, QString> ids_;             // a Live track's Id -> its id here
    QHash<QString, std::vector<Pad>> pads_;   // a Live track's Id -> its Drum Rack's pads that are tracks
    QStringList returnIds_;                   // the return tracks' ids, in order
    QHash<QString, Target> targets_;          // Live's AutomationTarget Id -> where its automation goes
    QSet<QString> settingTargets_;            // the tempo's and the time signature's (readSettings)
    QHash<QString, QJsonObject> automation_;  // owner id -> its automation (key -> points)
    std::vector<QJsonObject> tracks_;
    QJsonArray returns_;
    int clipCount_ = 0;
    QHash<QString, QString> resolvedPaths_;  // a file as the set names it (path, relative) -> where it is
    QSet<QString> missingPaths_;
};

// --- The song --------------------------------------------------------------------------------

const Element* masterTrack(const Element& set) {
    if (const Element* main = set.child(u"MainTrack")) return main;  // (Live 12)
    return set.child(u"MasterTrack");
}

void Importer::readSettings() {
    const Element* master = masterTrack(set_);
    const Element* mixer = master ? master->at(u"DeviceChain/Mixer") : nullptr;
    if (mixer) {
        tempo_ = mixer->number(u"Tempo/Manual", 120.0);
        const int signature = mixer->integer(u"TimeSignature/Manual", 201);
        numerator_ = std::clamp(signature % 99 + 1, 1, 32);
        denominator_ = 1 << std::clamp(signature / 99, 0, 5);
    }
    // An automated tempo or time signature: its value at the start; changes are noted.
    if (master && mixer) {
        const auto targetId = [mixer](QStringView path) {
            const Element* target = mixer->at(path);
            return target ? target->attribute(u"Id") : QString();
        };
        const QString tempoTarget = targetId(u"Tempo/AutomationTarget");
        const QString signatureTarget = targetId(u"TimeSignature/AutomationTarget");
        for (const QString& target : {tempoTarget, signatureTarget}) {
            if (!target.isEmpty()) settingTargets_ << target;
        }
        if (const Element* envelopes = master->at(u"AutomationEnvelopes/Envelopes")) {
            for (const Element* envelope : envelopes->all(u"AutomationEnvelope")) {
                const QString target = envelope->value(u"EnvelopeTarget/PointeeId").value_or(QString());
                const Element* events = envelope->at(u"Automation/Events");
                if (!events || target.isEmpty() || (target != tempoTarget && target != signatureTarget)) continue;
                std::optional<double> first;
                QSet<QString> values;
                for (const Element* event : events->all()) {
                    const double time = event->numberAttribute(u"Time");
                    values.insert(event->attribute(u"Value"));
                    if (time <= 0 || !first) first = event->numberAttribute(u"Value");
                }
                if (!first) continue;
                if (target == tempoTarget) {
                    tempo_ = std::clamp(*first, 20.0, 999.0);  // (as the project has it, before the note says it)
                    if (values.size() > 1) {
                        notes_.line(
                            QStringLiteral("The tempo changes during the song; SUBstation has one tempo: %1 BPM.")
                                .arg(QString::number(tempo_, 'g', 6)));
                    }
                } else {
                    const int signature = static_cast<int>(*first);
                    numerator_ = std::clamp(signature % 99 + 1, 1, 32);
                    denominator_ = 1 << std::clamp(signature / 99, 0, 5);
                    if (values.size() > 1) {
                        notes_.line(
                            QStringLiteral("The time signature changes during the song; SUBstation has one: %1/%2.")
                                .arg(numerator_)
                                .arg(denominator_));
                    }
                }
            }
        }
    }
    tempo_ = std::clamp(tempo_, 20.0, 999.0);
}

QString Importer::liveName(const Element& track) {
    const QString user = track.value(u"Name/UserName").value_or(QString()).trimmed();
    return !user.isEmpty() ? user : track.value(u"Name/EffectiveName").value_or(QStringLiteral("Track"));
}

// A return's: Live's default ones are lettered ("A-Reverb"); SUBstation letters them itself.
QString returnName(const Element& track) {
    const QString user = track.value(u"Name/UserName").value_or(QString()).trimmed();
    if (!user.isEmpty()) return user;
    QString name = track.value(u"Name/EffectiveName").value_or(QString()).trimmed();
    static const QRegularExpression letter(QStringLiteral("^[A-Z]{1,2}-\\s*"));
    name.remove(letter);
    return name.isEmpty() ? QStringLiteral("Return") : name;
}

QString Importer::trackName(const Element& track, const QString& fallback) const {
    const QString user = track.value(u"Name/UserName").value_or(QString()).trimmed();
    if (!user.isEmpty()) return user;
    // Live's default names are numbered ("4-Serum"): SUBstation's are too ("# Serum").
    QString name = track.value(u"Name/EffectiveName").value_or(QString()).trimmed();
    static const QRegularExpression number(QStringLiteral("^\\d+[- ]\\s*"));
    name.remove(number);
    return QStringLiteral("# ") + (name.isEmpty() ? fallback : name);
}

// --- Ids, before anything else ------------------------------------------------------------------

const Element* drumRackOf(const Element& track) {
    if (track.tag != u"MidiTrack") return nullptr;
    const Element* devices = track.at(u"DeviceChain/DeviceChain/Devices");
    if (!devices) return nullptr;
    for (const Element* device : devices->all()) {
        if (device->tag == u"DrumGroupDevice") return device;
    }
    return nullptr;
}

// The notes the arrangement's clips play on a MIDI track.
QSet<int> playedKeys(const Element& track) {
    QSet<int> keys;
    if (const Element* events = track.at(u"DeviceChain/MainSequencer/ClipTimeable/ArrangerAutomation/Events")) {
        for (const Element* clip : events->all(u"MidiClip")) {
            if (const Element* keyTracks = clip->at(u"Notes/KeyTracks")) {
                for (const Element* keyTrack : keyTracks->all(u"KeyTrack")) {
                    const Element* notes = keyTrack->child(u"Notes");
                    if (notes && !notes->children.empty()) keys.insert(keyTrack->integer(u"MidiKey"));
                }
            }
        }
    }
    return keys;
}

void Importer::assignIds() {
    const Element* tracks = set_.child(u"Tracks");
    if (!tracks) return;
    for (const Element* track : tracks->all()) {
        const QString liveId = track->attribute(u"Id");
        const QString id = newId();
        if (track->tag == u"ReturnTrack") {
            returnIds_.append(id);
            ids_.insert(liveId, id);
            continue;
        }
        ids_.insert(liveId, id);
        if (const Element* drums = drumRackOf(*track)) {
            // Each pad the clips play becomes a track (the lowest note first).
            const QSet<int> played = playedKeys(*track);
            std::vector<Pad> pads;
            int branchIndex = 0;
            if (const Element* branches = drums->child(u"Branches")) {
                for (const Element* branch : branches->all(u"DrumBranch")) {
                    const int receiving = branch->integer(u"BranchInfo/ReceivingNote", -1);
                    const int note = 128 - receiving;  // (Live keeps it upside down)
                    if (receiving > 0 && played.contains(note)) {
                        pads.push_back(
                            {branchIndex, note, branch->integer(u"BranchInfo/SendingNote", 60), newId(), branch});
                    }
                    ++branchIndex;
                }
            }
            std::stable_sort(pads.begin(), pads.end(), [](const Pad& a, const Pad& b) { return a.note < b.note; });
            pads_.insert(liveId, pads);
        }
    }
}

// --- Tracks ---------------------------------------------------------------------------------------

ImportResult Importer::run() {
    readSettings();
    assignIds();
    const Element* tracks = set_.child(u"Tracks");
    QHash<QString, QString> groupIds;  // a Live group's Id -> its id
    if (tracks) {
        for (const Element* track : tracks->all()) {
            const QString liveId = track->attribute(u"Id");
            const QString id = ids_.value(liveId);
            if (track->tag == u"ReturnTrack") {
                returns_.append(translateReturn(*track, id));
                continue;
            }
            if (track->tag != u"AudioTrack" && track->tag != u"MidiTrack" && track->tag != u"GroupTrack") continue;
            const QString groupId = track->value(u"TrackGroupId").value_or(QStringLiteral("-1"));
            const std::optional<QString> parent =
                groupIds.contains(groupId) ? std::optional<QString>(groupIds.value(groupId)) : std::nullopt;
            if (track->tag == u"GroupTrack") groupIds.insert(liveId, id);
            if (track->flag(u"Freeze"))
                notes_.listed(QStringLiteral("Frozen in Live, imported unfrozen"), liveName(*track));
            noteClipEnvelopes(*track);
            if (const Element* drums = drumRackOf(*track)) {
                translateDrumTrack(*track, *drums, id, parent);
            } else {
                tracks_.push_back(translateTrack(*track, id, parent));
            }
            readAutomation(*track);
        }
        for (const Element* track : tracks->all(u"ReturnTrack")) readAutomation(*track);
    }
    QJsonObject master;
    if (const Element* main = masterTrack(set_)) {
        master = translateMaster(*main);
        readAutomation(*main);
    }

    // The automation, each envelope in its owner's.
    QHash<QString, int> trackIndex;
    for (size_t i = 0; i < tracks_.size(); ++i)
        trackIndex.insert(tracks_[i].value(QStringLiteral("id")).toString(), int(i));
    for (auto it = automation_.constBegin(); it != automation_.constEnd(); ++it) {
        if (it.key() == kMaster) {
            master[QStringLiteral("automation")] = it.value();
        } else if (trackIndex.contains(it.key())) {
            tracks_[size_t(trackIndex.value(it.key()))][QStringLiteral("automation")] = it.value();
        } else {
            for (qsizetype r = 0; r < returns_.size(); ++r) {
                QJsonObject ret = returns_.at(r).toObject();
                if (ret.value(QStringLiteral("id")).toString() != it.key()) continue;
                ret[QStringLiteral("automation")] = it.value();
                returns_[r] = ret;
            }
        }
    }

    QJsonArray trackList;
    for (const QJsonObject& track : tracks_) trackList.append(track);
    // The key: Live 12's scale, if major or minor. C major is Live's own until
    // one is chosen: none then (SUBstation finds the key the MIDI is in).
    std::optional<Key> key;
    if (set_.flag(u"InKey", true) && set_.child(u"ScaleInformation")) {
        const int root = set_.integer(u"ScaleInformation/Root", -1);
        const QString scale = set_.value(u"ScaleInformation/Name").value_or(QString());
        const bool major = scale == u"0" || scale.compare(u"Major", Qt::CaseInsensitive) == 0;
        const bool minor = scale == u"1" || scale.compare(u"Minor", Qt::CaseInsensitive) == 0;
        if (root >= 0 && root < 12 && (minor || (major && root != 0))) key = Key{root, minor};
    }
    const Element* transport = set_.child(u"Transport");
    const double loopStart = transport ? std::max(0.0, transport->number(u"LoopStart")) : 0.0;
    const double loopLength = transport ? transport->number(u"LoopLength", 16.0) : 16.0;

    if (!missingPaths_.isEmpty()) {
        notes_.line(
            missingPaths_.size() == 1
                ? QStringLiteral("1 audio file isn't where the set says (moved, or on another computer): the "
                                 "File Manager finds it.")
                : QStringLiteral("%1 audio files aren't where the set says (moved, or on another computer): the "
                                 "File Manager finds them.")
                      .arg(missingPaths_.size()));
    }

    ImportResult result;
    result.project = QJsonObject{
        {QStringLiteral("format"), kProjectFormat},
        {QStringLiteral("version"), kProjectVersion},
        {QStringLiteral("tempo"), tempo_},
        {QStringLiteral("key"), key ? QJsonValue(key->name()) : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("time_signature"), QJsonArray{numerator_, denominator_}},
        {QStringLiteral("loop"), QJsonObject{{QStringLiteral("enabled"), transport && transport->flag(u"LoopOn")},
                                             {QStringLiteral("start"), loopStart},
                                             {QStringLiteral("end"), loopStart + std::max(loopLength, 1.0)}}},
        {QStringLiteral("automation_locked"), false},
        {QStringLiteral("master"), master},
        {QStringLiteral("tracks"), trackList},
        {QStringLiteral("returns"), returns_}};
    result.notes = notes_.all();
    result.tracks = static_cast<int>(tracks_.size());
    result.clips = clipCount_;
    return result;
}

QJsonObject trackBase(const QString& id, const QString& kind, const QString& name, const QString& color) {
    return QJsonObject{{QStringLiteral("id"), id},
                       {QStringLiteral("kind"), kind},
                       {QStringLiteral("name"), name},
                       {QStringLiteral("color"), color},
                       {QStringLiteral("volume_db"), 0.0},
                       {QStringLiteral("pan"), 0.0},
                       {QStringLiteral("mute"), false},
                       {QStringLiteral("solo"), false},
                       {QStringLiteral("height"), kind == kGroupKind ? kDefaultGroupHeight : kDefaultTrackHeight},
                       {QStringLiteral("devices"), QJsonArray()},
                       {QStringLiteral("clips"), QJsonArray()},
                       {QStringLiteral("sends"), QJsonObject()},
                       {QStringLiteral("monitor"), QStringLiteral("auto")},
                       {QStringLiteral("armed"), false},
                       {QStringLiteral("folded"), false}};
}

QJsonObject Importer::translateTrack(const Element& track, const QString& id, const std::optional<QString>& parent) {
    const bool midi = track.tag == u"MidiTrack";
    const bool group = track.tag == u"GroupTrack";
    const QString kind = group ? kGroupKind : midi ? kMidiKind : kAudioKind;
    QJsonObject data = trackBase(id, kind,
                                 trackName(track, group  ? QStringLiteral("Group")
                                                  : midi ? QStringLiteral("MIDI")
                                                         : QStringLiteral("Audio")),
                                 liveColor(colorOf(track)));
    data[QStringLiteral("parent")] = optionalString(parent);
    data[QStringLiteral("folded")] = !track.flag(u"TrackUnfolded", true);
    if (const Element* mixer = track.at(u"DeviceChain/Mixer")) {
        translateMixer(*mixer, id, data);
        addSends(*mixer, id, data);
    }
    addRouting(track, data, parent.has_value());
    data[QStringLiteral("devices")] = translateDevices(track.at(u"DeviceChain/DeviceChain/Devices"), id);
    if (!group) data[QStringLiteral("clips")] = clips(track, midi);
    if (midi)
        data[QStringLiteral("midi_input")] =
            QJsonObject{{QStringLiteral("device"), QString()}, {QStringLiteral("channel"), 0}};
    switch (track.integer(u"DeviceChain/MainSequencer/MonitoringEnum", 1)) {
    case 0: data[QStringLiteral("monitor")] = QStringLiteral("in"); break;
    case 2: data[QStringLiteral("monitor")] = QStringLiteral("off"); break;
    default: break;
    }
    return data;
}

// A MIDI track playing a Drum Rack: a group (its mixer, its effects after the
// rack) with a MIDI track for each pad the clips play, each with that pad's notes.
void Importer::translateDrumTrack(const Element& track, const Element& drums, const QString& id,
                                  const std::optional<QString>& parent) {
    QJsonObject group = trackBase(id, kGroupKind, trackName(track, QStringLiteral("Drums")), liveColor(colorOf(track)));
    group[QStringLiteral("parent")] = optionalString(parent);
    group[QStringLiteral("folded")] = !track.flag(u"TrackUnfolded", true);
    if (const Element* mixer = track.at(u"DeviceChain/Mixer")) {
        translateMixer(*mixer, id, group);
        addSends(*mixer, id, group);
    }
    addRouting(track, group, parent.has_value());
    // Its devices after the rack are the group's; MIDI effects before it can't be.
    QJsonArray after;
    bool past = false;
    if (const Element* devices = track.at(u"DeviceChain/DeviceChain/Devices")) {
        for (const Element* device : devices->all()) {
            if (device == &drums) {
                past = true;
                continue;
            }
            if (!past) {
                notes_.listed(kMidiEffects, liveDeviceName(device->tag));
                continue;
            }
            if (auto translated = translateDevice(*device, id)) after.append(*translated);
        }
    }
    group[QStringLiteral("devices")] = after;
    tracks_.push_back(group);

    if (drums.child(u"ReturnBranches") && !drums.child(u"ReturnBranches")->children.empty()) {
        notes_.listed(QStringLiteral("Left out (Drum Racks' return chains)"), liveName(track));
    }
    for (const Pad& pad : pads_.value(track.attribute(u"Id"))) {
        // Its clips first: one whose notes are all outside the clips' windows makes
        // no track (nor notes on its devices; a sidechain from it goes, its automation is noted).
        const int note = pad.note;
        const int sends = std::clamp(pad.sends, 0, 127);
        const QJsonArray padClips = clips(
            track, true, [note, sends](int key) { return key == note ? std::optional<int>(sends) : std::nullopt; });
        if (padClips.isEmpty()) continue;
        const Element& branch = *pad.element;
        QString name = branch.value(u"Name/UserName").value_or(QString()).trimmed();
        if (name.isEmpty()) name = branch.value(u"Name/EffectiveName").value_or(QString()).trimmed();
        if (name.isEmpty()) name = branch.value(u"Name").value_or(QStringLiteral("Pad"));
        QJsonObject padTrack = trackBase(pad.id, kMidiKind, name, liveColor(colorOf(track)));
        padTrack[QStringLiteral("parent")] = id;
        padTrack[QStringLiteral("folded")] = true;
        padTrack[QStringLiteral("midi_input")] =
            QJsonObject{{QStringLiteral("device"), QString()}, {QStringLiteral("channel"), 0}};
        if (const Element* mixer = branch.child(u"MixerDevice")) {
            padTrack[QStringLiteral("volume_db")] = faderDb(mixer->number(u"Volume/Manual", 1.0));
            padTrack[QStringLiteral("pan")] = std::clamp(mixer->number(u"Panorama/Manual"), -1.0, 1.0);
            padTrack[QStringLiteral("mute")] = !mixer->flag(u"Speaker/Manual", true);
            addTarget(mixer->child(u"Volume"), pad.id, automation::kMixerVolume, volumeNormalized);
            addTarget(mixer->child(u"Panorama"), pad.id, automation::kMixerPan, panNormalized);
            addTarget(mixer->child(u"Speaker"), pad.id, automation::kMixerOn, switchNormalized);
        }
        padTrack[QStringLiteral("solo")] = branch.flag(u"IsSoloed");
        padTrack[QStringLiteral("devices")] =
            translateDevices(branch.at(u"DeviceChain/MidiToAudioDeviceChain/Devices"), pad.id);
        padTrack[QStringLiteral("clips")] = padClips;
        tracks_.push_back(padTrack);
    }
}

QJsonObject Importer::translateReturn(const Element& track, const QString& id) {
    QJsonObject data{{QStringLiteral("id"), id},
                     {QStringLiteral("kind"), kReturnKind},
                     {QStringLiteral("name"), returnName(track)},
                     {QStringLiteral("color"), liveColor(colorOf(track))},
                     {QStringLiteral("volume_db"), 0.0},
                     {QStringLiteral("pan"), 0.0},
                     {QStringLiteral("mute"), false},
                     {QStringLiteral("solo"), false},
                     {QStringLiteral("height"), kDefaultTrackHeight},
                     {QStringLiteral("sends"), QJsonObject()}};
    if (const Element* mixer = track.at(u"DeviceChain/Mixer")) {
        translateMixer(*mixer, id, data);
        addSends(*mixer, id, data);
    }
    addRouting(track, data, false);
    data[QStringLiteral("devices")] = translateDevices(track.at(u"DeviceChain/DeviceChain/Devices"), id);
    return data;
}

QJsonObject Importer::translateMaster(const Element& track) {
    QJsonObject data{{QStringLiteral("volume_db"), 0.0}, {QStringLiteral("pan"), 0.0}};
    if (const Element* mixer = track.at(u"DeviceChain/Mixer")) translateMixer(*mixer, kMaster, data, true);
    data[QStringLiteral("devices")] = translateDevices(track.at(u"DeviceChain/DeviceChain/Devices"), kMaster);
    return data;
}

void Importer::translateMixer(const Element& mixer, const QString& owner, QJsonObject& data, bool master) {
    data[QStringLiteral("volume_db")] = faderDb(mixer.number(u"Volume/Manual", 1.0));
    data[QStringLiteral("pan")] = std::clamp(mixer.number(u"Pan/Manual"), -1.0, 1.0);
    addTarget(mixer.child(u"Volume"), owner, automation::kMixerVolume, volumeNormalized);
    addTarget(mixer.child(u"Pan"), owner, automation::kMixerPan, panNormalized);
    if (master) return;
    data[QStringLiteral("mute")] = !mixer.flag(u"Speaker/Manual", true);
    data[QStringLiteral("solo")] = mixer.flag(u"SoloSink");
    addTarget(mixer.child(u"Speaker"), owner, automation::kMixerOn, switchNormalized);
}

void Importer::addSends(const Element& mixer, const QString& owner, QJsonObject& data) {
    const Element* sends = mixer.child(u"Sends");
    if (!sends) return;
    const std::vector<const Element*> pre =
        set_.child(u"SendsPre") ? set_.child(u"SendsPre")->all() : std::vector<const Element*>{};
    QJsonObject out;
    int index = 0;
    for (const Element* holder : sends->all(u"TrackSendHolder")) {
        const int i = index++;
        if (i >= returnIds_.size()) break;
        const Element* send = holder->child(u"Send");
        if (!send) continue;
        const QString returnId = returnIds_.at(i);
        addTarget(send, owner, automation::sendKey(returnId), volumeNormalized);
        const double db = faderDb(send->number(u"Manual", 0.0));
        if (db <= automation::kMinVolumeDb) continue;  // (no send)
        const bool preFader = i < static_cast<int>(pre.size()) && pre[size_t(i)]->attribute(u"Value") == u"true";
        out[returnId] = QJsonObject{{QStringLiteral("level_db"), db}, {QStringLiteral("pre_fader"), preFader}};
    }
    data[QStringLiteral("sends")] = out;
}

// A routing target naming a track ("AudioIn/Track.40/PostFxOut", "AudioOut/Track.40/TrackIn",
// a Drum Rack pad's "AudioIn/Track.38/DeviceOut.0.B2,PreFxOut"): the track here, and
// where in it ("post", "pre", "pre-fx").
std::optional<QString> Importer::routedTrack(const QString& target, QString* tap) {
    static const QRegularExpression track(QStringLiteral("^Audio(?:In|Out)/Track\\.(\\d+)(?:/(.*))?$"));
    const QRegularExpressionMatch match = track.match(target);
    if (!match.hasMatch()) return std::nullopt;
    const QString liveId = match.captured(1);
    const QString where = match.captured(2);
    if (tap) *tap = where.contains(u"PreFx") ? kPreFx : where.contains(u"PostFx") ? kPreFader : kPostFader;
    static const QRegularExpression pad(QStringLiteral("DeviceOut\\.\\d+\\.B(\\d+)"));
    if (const QRegularExpressionMatch padMatch = pad.match(where); padMatch.hasMatch()) {
        const int branch = padMatch.captured(1).toInt();
        for (const Pad& p : pads_.value(liveId)) {
            if (p.branch == branch) {
                if (tap) *tap = kPreFader;  // (the pad's chain, after its devices)
                return p.id;
            }
        }
        return std::nullopt;
    }
    if (!ids_.contains(liveId)) return std::nullopt;
    return ids_.value(liveId);
}

void Importer::addRouting(const Element& track, QJsonObject& data, bool inGroup) {
    const QString output = track.value(u"DeviceChain/AudioOutputRouting/Target").value_or(QString());
    if (output == u"AudioOut/None") {
        data[QStringLiteral("output")] = QJsonObject{{QStringLiteral("to"), QStringLiteral("none")}};
    } else if ((output == u"AudioOut/Main" || output == u"AudioOut/Master") && inGroup) {
        data[QStringLiteral("output")] = QJsonObject{{QStringLiteral("to"), QStringLiteral("master")}};
    } else if (const auto to = routedTrack(output, nullptr)) {
        data[QStringLiteral("output")] =
            QJsonObject{{QStringLiteral("to"), QStringLiteral("track")}, {QStringLiteral("track"), *to}};
    } else if (output.startsWith(u"AudioOut/External")) {
        notes_.listed(QStringLiteral("Played into the main track instead of the audio device's outputs"),
                      liveName(track));
    }
    if (track.tag != u"AudioTrack") return;
    const QString input = track.value(u"DeviceChain/AudioInputRouting/Target").value_or(QString());
    static const QRegularExpression external(QStringLiteral("^AudioIn/External/([SM])(\\d+)$"));
    if (const QRegularExpressionMatch match = external.match(input); match.hasMatch()) {
        const int n = match.captured(2).toInt();
        data[QStringLiteral("input")] = match.captured(1) == u"S" ? QJsonArray{2 * n, 2 * n + 1} : QJsonArray{n};
    } else if (input == u"AudioIn/Main" || input == u"AudioIn/Master") {
        // Resampling: only where it is heard (tracks Live made with it, monitoring Off, play their clips).
        const bool armed =
            track.flag(u"DeviceChain/Mixer/Arm/Manual") || track.flag(u"DeviceChain/MainSequencer/Recorder/IsArmed");
        if (armed || track.integer(u"DeviceChain/MainSequencer/MonitoringEnum", 1) == 0) {
            data[QStringLiteral("input_track")] = kMaster;
        }
    } else {
        QString tap;
        if (const auto from = routedTrack(input, &tap)) {
            data[QStringLiteral("input_track")] = *from;
            if (tap != kPostFader) data[QStringLiteral("input_tap")] = tap;
        }
    }
}

// --- Devices ------------------------------------------------------------------------------------

QJsonArray Importer::translateDevices(const Element* devices, const QString& owner) {
    QJsonArray out;
    if (!devices) return out;
    for (const Element* device : devices->all()) {
        if (auto translated = translateDevice(*device, owner)) out.append(*translated);
    }
    return out;
}

void Importer::addTarget(const Element* parameter, const QString& owner, const QString& key,
                         std::function<double(double)> normalized) {
    if (!parameter || !normalized) return;
    const Element* target = parameter->child(u"AutomationTarget");
    if (target && !target->attribute(u"Id").isEmpty()) {
        targets_.insert(target->attribute(u"Id"), Target{owner, key, std::move(normalized)});
    }
}

void Importer::addBuiltinTarget(const Element* parameter, const QString& owner, const QString& kind,
                                const QString& deviceId, const QString& paramId,
                                std::function<double(double)> toPlain) {
    addTarget(parameter, owner, automation::deviceKey(deviceId, paramId),
              builtinNormalized(kind, paramId, std::move(toPlain)));
}

std::optional<QJsonObject> Importer::translateDevice(const Element& device, const QString& owner) {
    const QString& tag = device.tag;
    const QString id = newId();
    std::optional<QJsonObject> translated;
    if (tag == u"PluginDevice") {
        translated = plugin(device, owner, id);
    } else if (tag == u"AudioEffectGroupDevice" || tag == u"InstrumentGroupDevice") {
        translated = rack(device, owner, id);
    } else if (tag == u"StereoGain") {
        translated = utility(device, owner, id);
    } else if (tag == u"Eq8") {
        translated = eq(device, owner, id);
    } else if (tag == u"Compressor2") {
        translated = compressor(device, owner, id);
    } else if (tag == u"Delay") {
        translated = delay(device, owner, id);
    } else if (tag == u"OriginalSimpler" || tag == u"MultiSampler") {
        translated = sampler(device, owner, id);
    } else if (tag.startsWith(u"Midi") || tag == u"MxDeviceMidiEffect") {
        notes_.listed(kMidiEffects, liveDeviceName(tag));
        return std::nullopt;
    } else if (tag == u"DrumGroupDevice") {
        notes_.listed(kNoDevice, QStringLiteral("Drum Rack (inside a rack)"));
        return std::nullopt;
    } else {
        notes_.listed(kNoDevice, liveDeviceName(tag));
        return std::nullopt;
    }
    if (!translated) return std::nullopt;
    (*translated)[QStringLiteral("enabled")] = device.flag(u"On/Manual", true);
    addTarget(device.child(u"On"), owner, automation::deviceOnKey(id), switchNormalized);
    return translated;
}

QJsonObject Importer::builtin(const QString& kind, const QString& id, const QJsonObject& params) {
    return QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("kind"), kind}, {QStringLiteral("params"), params}};
}

std::optional<QJsonObject> Importer::plugin(const Element& device, const QString& owner, const QString& id) {
    const Element* desc = device.child(u"PluginDesc");
    if (!desc) return std::nullopt;
    if (const Element* vst3 = desc->child(u"Vst3PluginInfo")) {
        quint32 words[4] = {};
        for (int i = 0; i < 4; ++i) {
            words[i] = liveWord(*vst3, QStringLiteral("Uid/Fields.%1").arg(i)).value_or(0);
        }
        const QString uid = QString::fromStdString(sub::vst3::classIdFromWords(words[0], words[1], words[2], words[3]));
        const QString presetId =
            QString::fromStdString(sub::vst3::presetClassId(words[0], words[1], words[2], words[3]));
        QString name = vst3->value(u"Name").value_or(QStringLiteral("Plug-in"));
        QJsonObject ref{{QStringLiteral("format"), QStringLiteral("VST3")},
                        {QStringLiteral("uid"), uid},
                        {QStringLiteral("name"), name},
                        {QStringLiteral("vendor"), QString()},
                        {QStringLiteral("path"), QString()},
                        {QStringLiteral("instrument"), vst3->integer(u"DeviceType") == 1}};
        bool installed = false;
        for (const PluginInfo& info : options_.plugins) {
            if (info.uid.compare(uid, Qt::CaseInsensitive) != 0) continue;
            ref[QStringLiteral("uid")] = info.uid;
            ref[QStringLiteral("name")] = info.name;
            ref[QStringLiteral("vendor")] = info.vendor;
            ref[QStringLiteral("path")] = info.path;
            ref[QStringLiteral("instrument")] = info.instrument;
            installed = true;
            break;
        }
        if (!installed && !options_.plugins.empty()) notes_.listed(kNotInstalled, name);
        QJsonObject data{
            {QStringLiteral("id"), id}, {QStringLiteral("kind"), kPluginKind}, {QStringLiteral("plugin"), ref}};
        if (const Element* preset = vst3->at(u"Preset/Vst3Preset")) {
            const QByteArray processor =
                hexBytes(preset->child(u"ProcessorState") ? preset->child(u"ProcessorState")->text : QString());
            const QByteArray controller =
                hexBytes(preset->child(u"ControllerState") ? preset->child(u"ControllerState")->text : QString());
            if (!processor.isEmpty()) {
                data[QStringLiteral("state")] =
                    QString::fromLatin1(vstPreset(presetId, processor, controller).toBase64());
            }
        }
        if (const Element* parameters = device.child(u"ParameterList")) {
            for (const Element* parameter : parameters->all()) {
                // A VST3 ParamID (high-bit ids written negative); -1 is a slot with none (kNoParamId).
                const std::optional<quint32> paramId = liveWord(*parameter, u"ParameterId");
                if (!paramId || *paramId == 0xFFFFFFFFu) continue;
                addTarget(parameter->child(u"ParameterValue"), owner,
                          automation::deviceKey(id, QString::number(*paramId)), plainNormalized);
            }
        }
        return data;
    }
    if (const Element* vst2 = desc->child(u"VstPluginInfo")) {
        const QString name = vst2->value(u"PlugName").value_or(QStringLiteral("Plug-in"));
        const quint32 vst2Id = liveWord(*vst2, u"UniqueId").value_or(0);
        // The installed VST3 made to replace it (its class id "VST" + its id), else one of its name.
        const PluginInfo* replacement = nullptr;
        const PluginInfo* sameName = nullptr;
        const QString plain = plainPluginName(name);
        for (const PluginInfo& info : options_.plugins) {
            if (const auto words = sub::vst3::wordsFromClassId(info.uid.toStdString())) {
                const quint32 l1 = (*words)[0], l2 = (*words)[1];
                if ((l1 >> 8) == 0x565354u && (((l1 & 0xFFu) << 24) | (l2 >> 8)) == vst2Id) {
                    replacement = &info;
                    break;
                }
            }
            if (!sameName && !plain.isEmpty() && plainPluginName(info.name) == plain) sameName = &info;
        }
        const PluginInfo* chosen = replacement ? replacement : sameName;
        if (!chosen) {
            notes_.listed(kVst2Missing, name);
            return std::nullopt;
        }
        QJsonObject data{
            {QStringLiteral("id"), id},
            {QStringLiteral("kind"), kPluginKind},
            {QStringLiteral("plugin"),
             QJsonObject{{QStringLiteral("format"), chosen->format.isEmpty() ? QStringLiteral("VST3") : chosen->format},
                         {QStringLiteral("uid"), chosen->uid},
                         {QStringLiteral("name"), chosen->name},
                         {QStringLiteral("vendor"), chosen->vendor},
                         {QStringLiteral("path"), chosen->path},
                         {QStringLiteral("instrument"), chosen->instrument}}}};
        const Element* preset = vst2->at(u"Preset/VstPreset");
        const QByteArray chunk =
            preset && preset->child(u"Buffer") ? hexBytes(preset->child(u"Buffer")->text) : QByteArray();
        if (replacement && !chunk.isEmpty()) {
            const quint32 type = liveWord(*preset, u"Type").value_or(0);
            const bool bank = type == 0x46424368u;  // 'FBCh' ('FPCh': a program)
            const quint32 version = liveWord(*vst2, u"Version").value_or(0);
            const QByteArray state =
                vst2CompatibleState(chunk, vst2Id, version, bank, preset->integer(u"ProgramCount", 1));
            const auto words = sub::vst3::wordsFromClassId(replacement->uid.toStdString());
            const QString presetId =
                QString::fromStdString(sub::vst3::presetClassId((*words)[0], (*words)[1], (*words)[2], (*words)[3]));
            data[QStringLiteral("state")] = QString::fromLatin1(vstPreset(presetId, state, {}).toBase64());
        } else {
            notes_.listed(kVst2Defaults, QStringLiteral("%1 as %2").arg(name, chosen->name));
        }
        return data;
    }
    notes_.listed(kNoDevice, QStringLiteral("AU plug-ins"));
    return std::nullopt;
}

std::optional<QJsonObject> Importer::rack(const Element& device, const QString& owner, const QString& id) {
    const bool instrument = device.tag == u"InstrumentGroupDevice";
    const QString branchTag = instrument ? QStringLiteral("InstrumentBranch") : QStringLiteral("AudioEffectBranch");
    const QString chainPath = instrument ? QStringLiteral("DeviceChain/MidiToAudioDeviceChain/Devices")
                                         : QStringLiteral("DeviceChain/AudioToAudioDeviceChain/Devices");
    QJsonArray chains;
    if (const Element* branches = device.child(u"Branches")) {
        for (const Element* branch : branches->all(branchTag)) {
            const QString chainId = newId();
            QString name = branch->value(u"Name/UserName").value_or(QString()).trimmed();
            if (name.isEmpty()) name = branch->value(u"Name/EffectiveName").value_or(QString()).trimmed();
            QJsonObject chain{{QStringLiteral("id"), chainId},
                              {QStringLiteral("name"), name.isEmpty() ? QStringLiteral("Chain") : name},
                              {QStringLiteral("volume_db"), 0.0},
                              {QStringLiteral("pan"), 0.0},
                              {QStringLiteral("mute"), false},
                              {QStringLiteral("solo"), branch->flag(u"IsSoloed")}};
            if (const Element* mixer = branch->child(u"MixerDevice")) {
                chain[QStringLiteral("volume_db")] = faderDb(mixer->number(u"Volume/Manual", 1.0));
                chain[QStringLiteral("pan")] = std::clamp(mixer->number(u"Panorama/Manual"), -1.0, 1.0);
                chain[QStringLiteral("mute")] = !mixer->flag(u"Speaker/Manual", true);
                addTarget(mixer->child(u"Volume"), owner, automation::chainKey(id, chainId, automation::kChainVolume),
                          volumeNormalized);
                addTarget(mixer->child(u"Panorama"), owner, automation::chainKey(id, chainId, automation::kChainPan),
                          panNormalized);
            }
            chain[QStringLiteral("devices")] = translateDevices(branch->at(chainPath), owner);
            chains.append(chain);
        }
    }
    notes_.listed(QStringLiteral("Racks came without their macros"),
                  instrument ? QStringLiteral("Instrument Racks") : QStringLiteral("Audio Effect Racks"));
    QJsonObject data{
        {QStringLiteral("id"), id}, {QStringLiteral("kind"), kRackKind}, {QStringLiteral("chains"), chains}};
    const QString name = device.value(u"UserName").value_or(QString()).trimmed();
    if (!name.isEmpty()) data[QStringLiteral("name")] = name;
    return data;
}

std::optional<QJsonObject> Importer::utility(const Element& device, const QString& owner, const QString& id) {
    const QString kind = QStringLiteral("utility");
    // Gain is a linear amplitude (Live 11 and later; Live 10 kept dB in LegacyGain).
    const bool legacy = device.flag(u"LegacyMode");
    const auto gainDb = [legacy](double v) { return legacy ? v : gainToDb(v); };
    const double gain = legacy ? device.number(u"LegacyGain/Manual") : gainToDb(device.number(u"Gain/Manual", 1.0));
    QJsonObject params{
        {QStringLiteral("gain"), std::clamp(gain, -60.0, 24.0)},
        {QStringLiteral("pan"), std::clamp(device.number(u"Balance/Manual"), -1.0, 1.0)},
        {QStringLiteral("width"), std::clamp(100.0 * device.number(u"StereoWidth/Manual", 1.0), 0.0, 200.0)}};
    if (device.flag(u"Mono/Manual")) params[QStringLiteral("width")] = 0.0;
    if (device.flag(u"Mute/Manual")) params[QStringLiteral("gain")] = -60.0;
    addBuiltinTarget(device.child(legacy ? u"LegacyGain" : u"Gain"), owner, kind, id, QStringLiteral("gain"), gainDb);
    addBuiltinTarget(device.child(u"Balance"), owner, kind, id, QStringLiteral("pan"), [](double v) { return v; });
    addBuiltinTarget(device.child(u"StereoWidth"), owner, kind, id, QStringLiteral("width"),
                     [](double v) { return 100.0 * v; });
    return builtin(kind, id, params);
}

std::optional<QJsonObject> Importer::eq(const Element& device, const QString& owner, const QString& id) {
    const QString kind = QStringLiteral("eq");
    // Live's band modes: 48 dB low cut, 12 dB low cut, low shelf, bell, notch, high shelf, 12 dB and 48 dB high cut.
    struct Mode {
        int type;   // SUBstation's: Bell 0, Low Shelf 1, Low Cut 2, High Shelf 3, High Cut 4, Notch 5
        int slope;  // an index into its slopes: 12 dB/oct 1, 48 dB/oct 6
    };
    static const Mode kModes[] = {{2, 6}, {2, 1}, {1, 1}, {0, 1}, {5, 1}, {3, 1}, {4, 1}, {4, 6}};
    const int stereo = device.integer(u"Mode", 0);  // 0 stereo, 1 L/R, 2 M/S
    QJsonObject params{{QStringLiteral("output"), std::clamp(device.number(u"GlobalGain/Manual"), -36.0, 36.0)},
                       {QStringLiteral("scale"), std::clamp(100.0 * device.number(u"Scale/Manual", 1.0), 0.0, 200.0)}};
    addBuiltinTarget(device.child(u"GlobalGain"), owner, kind, id, QStringLiteral("output"),
                     [](double v) { return v; });
    addBuiltinTarget(device.child(u"Scale"), owner, kind, id, QStringLiteral("scale"),
                     [](double v) { return 100.0 * v; });
    int band = 0;
    for (const QString& side : {QStringLiteral("ParameterA"), QStringLiteral("ParameterB")}) {
        if (side == u"ParameterB" && stereo == 0) break;
        const int place = stereo == 0   ? 0
                          : stereo == 1 ? (side == u"ParameterA" ? 1 : 2)
                                        : (side == u"ParameterA" ? 3 : 4);
        for (int b = 0; b < 8; ++b) {
            const Element* p = device.at(QStringLiteral("Bands.%1/%2").arg(b).arg(side));
            if (!p) continue;
            const QString prefix = QStringLiteral("b%1_").arg(++band);
            const Mode mode = kModes[std::clamp(p->integer(u"Mode/Manual", 3), 0, 7)];
            params[prefix + QStringLiteral("used")] = 1.0;
            params[prefix + QStringLiteral("on")] = p->flag(u"IsOn/Manual", true) ? 1.0 : 0.0;
            params[prefix + QStringLiteral("type")] = mode.type;
            params[prefix + QStringLiteral("slope")] = mode.slope;
            params[prefix + QStringLiteral("freq")] = std::clamp(p->number(u"Freq/Manual", 1000.0), 10.0, 22000.0);
            params[prefix + QStringLiteral("gain")] = std::clamp(p->number(u"Gain/Manual"), -30.0, 30.0);
            params[prefix + QStringLiteral("q")] = std::clamp(p->number(u"Q/Manual", 0.71), 0.025, 40.0);
            params[prefix + QStringLiteral("place")] = place;
            const auto same = [](double v) { return v; };
            addBuiltinTarget(p->child(u"IsOn"), owner, kind, id, prefix + QStringLiteral("on"), same);
            addBuiltinTarget(p->child(u"Freq"), owner, kind, id, prefix + QStringLiteral("freq"), same);
            addBuiltinTarget(p->child(u"Gain"), owner, kind, id, prefix + QStringLiteral("gain"), same);
            addBuiltinTarget(p->child(u"Q"), owner, kind, id, prefix + QStringLiteral("q"), same);
        }
    }
    return builtin(kind, id, params);
}

std::optional<QJsonObject> Importer::sidechainOf(const Element& device) {
    const Element* sidechain = device.child(u"SideChain");
    if (!sidechain || !sidechain->flag(u"OnOff/Manual")) return std::nullopt;
    const QString target = sidechain->value(u"RoutedInput/Routable/Target").value_or(QString());
    if (target.isEmpty() || target == u"AudioIn/None") return std::nullopt;
    QString tap;
    if (const auto track = routedTrack(target, &tap)) {
        return QJsonObject{{QStringLiteral("track"), *track}, {QStringLiteral("tap"), tap}};
    }
    if (target == u"AudioIn/Main" || target == u"AudioIn/Master") {
        return QJsonObject{{QStringLiteral("track"), kMaster}, {QStringLiteral("tap"), kPostFader}};
    }
    notes_.line(
        QStringLiteral("Some sidechains come from what isn't a track here (a pad no clip plays, a device's output): "
                       "they were left out."));
    return std::nullopt;
}

std::optional<QJsonObject> Importer::compressor(const Element& device, const QString& owner, const QString& id) {
    const QString kind = QStringLiteral("compressor");
    const auto thresholdDb = [](double v) { return gainToDb(v); };
    const auto percent = [](double v) { return 100.0 * v; };
    const auto same = [](double v) { return v; };
    QJsonObject params{
        {QStringLiteral("threshold"), std::clamp(thresholdDb(device.number(u"Threshold/Manual", 1.0)), -60.0, 0.0)},
        {QStringLiteral("ratio"), std::clamp(device.number(u"Ratio/Manual", 4.0), 1.0, 20.0)},
        {QStringLiteral("attack"), std::clamp(device.number(u"Attack/Manual", 10.0), 0.1, 200.0)},
        {QStringLiteral("release"), std::clamp(device.number(u"Release/Manual", 150.0), 5.0, 2000.0)},
        {QStringLiteral("knee"), std::clamp(device.number(u"Knee/Manual", 6.0), 0.0, 24.0)},
        {QStringLiteral("makeup"), std::clamp(device.number(u"Gain/Manual"), -12.0, 24.0)},
        {QStringLiteral("mix"), std::clamp(percent(device.number(u"DryWet/Manual", 1.0)), 0.0, 100.0)}};
    addBuiltinTarget(device.child(u"Threshold"), owner, kind, id, QStringLiteral("threshold"), thresholdDb);
    addBuiltinTarget(device.child(u"Ratio"), owner, kind, id, QStringLiteral("ratio"), same);
    addBuiltinTarget(device.child(u"Attack"), owner, kind, id, QStringLiteral("attack"), same);
    addBuiltinTarget(device.child(u"Release"), owner, kind, id, QStringLiteral("release"), same);
    addBuiltinTarget(device.child(u"Gain"), owner, kind, id, QStringLiteral("makeup"), same);
    addBuiltinTarget(device.child(u"DryWet"), owner, kind, id, QStringLiteral("mix"), percent);
    QJsonObject data = builtin(kind, id, params);
    if (const auto sidechain = sidechainOf(device)) data[QStringLiteral("sidechain")] = *sidechain;
    return data;
}

std::optional<QJsonObject> Importer::delay(const Element& device, const QString& owner, const QString& id) {
    const QString kind = QStringLiteral("delay");
    const auto ms = [](double v) { return 1000.0 * v; };
    const auto percent = [](double v) { return 100.0 * v; };
    const auto same = [](double v) { return v; };
    const auto onOff = [&device](QStringView path) { return device.flag(path) ? 1.0 : 0.0; };
    QJsonObject params{
        {QStringLiteral("l_sync"), onOff(u"DelayLine_SyncL/Manual")},
        {QStringLiteral("l_division"), std::clamp(device.integer(u"DelayLine_SyncedSixteenthL/Manual", 2), 0, 7)},
        {QStringLiteral("l_time"), std::clamp(ms(device.number(u"DelayLine_TimeL/Manual", 0.25)), 1.0, 5000.0)},
        {QStringLiteral("l_offset"), std::clamp(percent(device.number(u"DelayLine_OffsetL/Manual")), -33.0, 33.0)},
        {QStringLiteral("r_sync"), onOff(u"DelayLine_SyncR/Manual")},
        {QStringLiteral("r_division"), std::clamp(device.integer(u"DelayLine_SyncedSixteenthR/Manual", 3), 0, 7)},
        {QStringLiteral("r_time"), std::clamp(ms(device.number(u"DelayLine_TimeR/Manual", 0.375)), 1.0, 5000.0)},
        {QStringLiteral("r_offset"), std::clamp(percent(device.number(u"DelayLine_OffsetR/Manual")), -33.0, 33.0)},
        {QStringLiteral("link"), onOff(u"DelayLine_Link/Manual")},
        {QStringLiteral("feedback"), std::clamp(percent(device.number(u"Feedback/Manual", 0.5)), 0.0, 95.0)},
        {QStringLiteral("freeze"), onOff(u"Freeze/Manual")},
        {QStringLiteral("filter"), onOff(u"Filter_On/Manual")},
        {QStringLiteral("freq"), std::clamp(device.number(u"Filter_Frequency/Manual", 1000.0), 50.0, 18000.0)},
        {QStringLiteral("width"), std::clamp(device.number(u"Filter_Bandwidth/Manual", 8.0), 0.5, 9.0)},
        {QStringLiteral("mode"), std::clamp(device.integer(u"DelayLine_SmoothingMode/Manual", 0), 0, 2)},
        {QStringLiteral("ping_pong"), onOff(u"DelayLine_PingPong/Manual")},
        {QStringLiteral("mix"), std::clamp(percent(device.number(u"DryWet/Manual", 0.5)), 0.0, 100.0)}};
    addBuiltinTarget(device.child(u"DelayLine_TimeL"), owner, kind, id, QStringLiteral("l_time"), ms);
    addBuiltinTarget(device.child(u"DelayLine_TimeR"), owner, kind, id, QStringLiteral("r_time"), ms);
    addBuiltinTarget(device.child(u"Feedback"), owner, kind, id, QStringLiteral("feedback"), percent);
    addBuiltinTarget(device.child(u"Filter_Frequency"), owner, kind, id, QStringLiteral("freq"), same);
    addBuiltinTarget(device.child(u"Filter_Bandwidth"), owner, kind, id, QStringLiteral("width"), same);
    addBuiltinTarget(device.child(u"DryWet"), owner, kind, id, QStringLiteral("mix"), percent);
    return builtin(kind, id, params);
}

std::optional<QJsonObject> Importer::sampler(const Element& device, const QString& owner, const QString& id) {
    const QString kind = QStringLiteral("sampler");
    const Element* parts = device.at(u"Player/MultiSampleMap/SampleParts");
    const std::vector<const Element*> samples = parts ? parts->all(u"MultiSamplePart") : std::vector<const Element*>{};
    if (samples.empty()) {
        notes_.listed(kNoDevice, liveDeviceName(device.tag) + QStringLiteral(" without a sample"));
        return std::nullopt;
    }
    if (samples.size() > 1)
        notes_.listed(QStringLiteral("Samplers of several samples became Samplers of their first"),
                      QStringLiteral("Sampler"));
    const Element& part = *samples.front();
    const QString path = samplePath(part.child(u"SampleRef"));
    const double frames = part.number(u"SampleRef/DefaultDuration", 0.0);
    const auto percentOf = [frames](double frame) {
        return frames > 0 ? std::clamp(100.0 * frame / frames, 0.0, 100.0) : 0.0;
    };
    // Its gain: the device's volume and its sample's (automation of the volume keeps the sample's on top).
    const double partDb = gainToDb(std::max(part.number(u"Volume", 1.0), kMinGain));
    const double gain = device.number(u"VolumeAndPan/Volume/Manual") + partDb;
    const bool simpler = device.tag == u"OriginalSimpler";
    const int mode = simpler ? std::clamp(device.integer(u"Globals/PlaybackMode", 0), 0, 2) : 0;
    QJsonObject params{
        {QStringLiteral("mode"), mode},
        {QStringLiteral("root"), std::clamp(part.integer(u"RootKey", 60), 0, 127)},
        {QStringLiteral("tune"), std::clamp(device.number(u"Pitch/TransposeKey/Manual"), -48.0, 48.0)},
        {QStringLiteral("fine"),
         std::clamp(device.number(u"Pitch/TransposeFine/Manual") + part.number(u"Detune"), -100.0, 100.0)},
        {QStringLiteral("start"), percentOf(part.number(u"SampleStart"))},
        {QStringLiteral("end"), frames > 0 ? percentOf(part.number(u"SampleEnd", frames)) : 100.0},
        {QStringLiteral("gain"), std::clamp(gain, -24.0, 24.0)},
        {QStringLiteral("attack"),
         std::clamp(device.number(u"VolumeAndPan/Envelope/AttackTime/Manual", 1.0), 0.1, 5000.0)},
        {QStringLiteral("decay"),
         std::clamp(device.number(u"VolumeAndPan/Envelope/DecayTime/Manual", 1000.0), 1.0, 10000.0)},
        {QStringLiteral("sustain"),
         std::clamp(100.0 * device.number(u"VolumeAndPan/Envelope/SustainLevel/Manual", 1.0), 0.0, 100.0)},
        {QStringLiteral("release"),
         std::clamp(device.number(u"VolumeAndPan/Envelope/ReleaseTime/Manual", 50.0), 1.0, 10000.0)}};
    if (mode == 1) {  // One-Shot's fades
        params[QStringLiteral("fade_in")] =
            std::clamp(device.number(u"VolumeAndPan/OneShotEnvelope/FadeInTime/Manual", 0.1), 0.1, 2000.0);
        params[QStringLiteral("fade_out")] =
            std::clamp(device.number(u"VolumeAndPan/OneShotEnvelope/FadeOutTime/Manual", 0.1), 0.1, 2000.0);
    }
    addBuiltinTarget(device.at(u"Pitch/TransposeKey"), owner, kind, id, QStringLiteral("tune"),
                     [](double v) { return v; });
    addBuiltinTarget(device.at(u"VolumeAndPan/Volume"), owner, kind, id, QStringLiteral("gain"),
                     [partDb](double v) { return v + partDb; });
    QJsonObject data = builtin(kind, id, params);
    if (!path.isEmpty()) {
        if (const auto state = deviceState::toModel({{QStringLiteral("sample"), path}}))
            data[QStringLiteral("state")] = *state;
    }
    return data;
}

// --- Clips ------------------------------------------------------------------------------------------

void Importer::noteClipEnvelopes(const Element& track) {
    for (const QStringView path : {u"DeviceChain/MainSequencer/ClipTimeable/ArrangerAutomation/Events",
                                   u"DeviceChain/MainSequencer/Sample/ArrangerAutomation/Events"}) {
        const Element* events = track.at(path);
        if (!events) continue;
        for (const Element* clip : events->all()) {
            const Element* envelopes = clip->at(u"Envelopes/Envelopes");
            if (envelopes && !envelopes->children.empty()) {
                notes_.listed(QStringLiteral("Left out (clip envelopes)"), liveName(track));
            }
        }
    }
}

QString Importer::samplePath(const Element* sampleRef) {
    if (!sampleRef) return {};
    const Element* file = sampleRef->child(u"FileRef");
    if (!file) return {};
    const QString path = QDir::fromNativeSeparators(file->value(u"Path").value_or(QString()));
    const QString relative = QDir::fromNativeSeparators(file->value(u"RelativePath").value_or(QString()));
    // Each file looked for once (many clips play one).
    const QString key = path + u'\n' + relative;
    if (const auto known = resolvedPaths_.constFind(key); known != resolvedPaths_.constEnd()) return *known;
    const QString resolved = resolveSample(path, relative);
    resolvedPaths_.insert(key, resolved);
    return resolved;
}

QString Importer::resolveSample(const QString& path, const QString& relative) {
    if (!path.isEmpty() && QFileInfo::exists(path)) return path;
    // Moved with the set: beside it (its project folder), or a folder up.
    if (!relative.isEmpty()) {
        for (const QString& base : {setDir_.absolutePath(), QFileInfo(setDir_.absolutePath()).absolutePath()}) {
            const QString candidate = QDir::cleanPath(QDir(base).filePath(relative));
            if (QFileInfo::exists(candidate)) return candidate;
        }
    }
    if (path.isEmpty() && relative.isEmpty()) return {};
    const QString kept = path.isEmpty() ? QDir::cleanPath(setDir_.filePath(relative)) : path;
    missingPaths_.insert(kept);
    return kept;
}

QJsonArray Importer::clips(const Element& track, bool midi, const std::function<std::optional<int>(int)>& pitch) {
    QJsonArray out;
    const Element* events = track.at(midi ? u"DeviceChain/MainSequencer/ClipTimeable/ArrangerAutomation/Events"
                                          : u"DeviceChain/MainSequencer/Sample/ArrangerAutomation/Events");
    if (!events) return out;
    for (const Element* clip : events->all(midi ? u"MidiClip" : u"AudioClip")) {
        if (midi) {
            if (auto translated = midiClip(*clip, pitch)) {
                out.append(*translated);
                ++clipCount_;
            }
        } else {
            audioClips(*clip, out);
        }
    }
    return out;
}

std::optional<QJsonObject> Importer::midiClip(const Element& clip,
                                              const std::function<std::optional<int>(int)>& pitch) {
    const double start = clip.number(u"CurrentStart", clip.numberAttribute(u"Time"));
    const double length = clip.number(u"CurrentEnd", start) - start;
    if (length <= 0) return std::nullopt;
    bool cut = false;
    const std::vector<Span> spans = clipSpans(clip, length, &cut);
    if (cut) notes_.line(kTooManyLoops);
    QJsonArray notes;
    if (const Element* keyTracks = clip.at(u"Notes/KeyTracks")) {
        for (const Element* keyTrack : keyTracks->all(u"KeyTrack")) {
            const int key = keyTrack->integer(u"MidiKey", -1);
            if (key < 0 || key > 127) continue;
            const std::optional<int> played = pitch ? pitch(key) : std::optional<int>(key);
            if (!played) continue;
            const Element* events = keyTrack->child(u"Notes");
            if (!events) continue;
            for (const Element* event : events->all(u"MidiNoteEvent")) {
                const double time = event->numberAttribute(u"Time");
                const double duration = event->numberAttribute(u"Duration");
                const int velocity =
                    std::clamp(static_cast<int>(std::lround(event->numberAttribute(u"Velocity", 100))), 1, 127);
                const bool muted = event->attribute(u"IsEnabled") == u"false";
                for (const Span& span : spans) {
                    if (time < span.from - 1e-9 || time >= span.to - 1e-9) continue;
                    const double noteLength = std::min(duration, span.to - time);
                    if (noteLength <= 0) continue;
                    QJsonArray note{*played, span.at + (time - span.from), noteLength, velocity};
                    if (muted) note.append(true);
                    notes.append(note);
                }
            }
        }
    }
    if (pitch && notes.isEmpty()) return std::nullopt;  // (a pad's track: the clips its pad plays in)
    QJsonObject data{{QStringLiteral("id"), newId()},       {QStringLiteral("name"), QString()},
                     {QStringLiteral("start_beat"), start}, {QStringLiteral("duration_beats"), length},
                     {QStringLiteral("offset_beats"), 0.0}, {QStringLiteral("notes"), notes}};
    if (clip.flag(u"Disabled")) data[QStringLiteral("muted")] = true;
    return data;
}

void Importer::audioClips(const Element& clip, QJsonArray& out) {
    const double start = clip.number(u"CurrentStart", clip.numberAttribute(u"Time"));
    const double length = clip.number(u"CurrentEnd", start) - start;
    if (length <= 0) return;
    const QString path = samplePath(clip.child(u"SampleRef"));
    if (path.isEmpty()) return;
    const double rate = clip.number(u"SampleRef/DefaultSampleRate", 0.0);
    const double sourceSec = rate > 0 ? clip.number(u"SampleRef/DefaultDuration") / rate : 0.0;
    const bool warped = clip.flag(u"IsWarped");
    QString name = clip.value(u"Name").value_or(QString()).trimmed();
    if (name.isEmpty()) name = QFileInfo(path).completeBaseName();

    // Each stretch it plays at a tempo of its own: (timeline beat, file offset, seconds, tempo; 0: unwarped).
    struct Piece {
        double beat;
        double offset;
        double seconds;
        double bpm;
    };
    std::vector<Piece> pieces;
    bool cut = false;
    bool clamped = false;
    if (warped) {
        const WarpMap map(clip, tempo_);
        for (const Span& span : clipSpans(clip, length, &cut)) {
            std::vector<double> edges{span.from};
            for (const double b : map.beatsBetween(span.from, span.to)) edges.push_back(b);
            edges.push_back(span.to);
            for (size_t i = 0; i + 1 < edges.size(); ++i) {
                double b0 = edges[i];
                double b1 = edges[i + 1];
                double s0 = map.secondsAt(b0);
                double s1 = map.secondsAt(b1);
                if (b1 - b0 <= 1e-9 || s1 - s0 <= 1e-9) continue;
                double bpm = 60.0 * (b1 - b0) / (s1 - s0);
                if (bpm < kMinSegmentBpm || bpm > kMaxSegmentBpm) {
                    bpm = std::clamp(bpm, kMinSegmentBpm, kMaxSegmentBpm);
                    clamped = true;
                }
                const double secondsPerBeat = 60.0 / bpm;
                // Not before the file's start, nor past its end.
                if (s0 < 0) {
                    b0 += -s0 / secondsPerBeat;
                    s0 = 0;
                }
                if (sourceSec > 0 && s1 > sourceSec) {
                    b1 -= (s1 - sourceSec) / secondsPerBeat;
                    s1 = sourceSec;
                }
                if (b1 - b0 <= 1e-9 || s1 - s0 <= 1e-9) continue;
                const double beat = start + span.at + (b0 - span.from);
                if (!pieces.empty()) {
                    Piece& last = pieces.back();
                    const double lastEnd = last.beat + last.seconds / (60.0 / last.bpm);
                    if (std::abs(last.bpm - bpm) <= 1e-3 * bpm && std::abs(last.offset + last.seconds - s0) < 1e-6 &&
                        std::abs(lastEnd - beat) < 1e-6) {
                        last.seconds += s1 - s0;
                        continue;
                    }
                }
                pieces.push_back({beat, s0, s1 - s0, bpm});
            }
        }
    } else {
        // Unwarped: its markers are in seconds, and it plays at its own speed.
        const double secondsPerBeat = 60.0 / tempo_;
        for (const Span& span : clipSpans(clip, length * secondsPerBeat, &cut)) {
            double from = span.from;
            double to = span.to;
            double at = span.at;
            if (from < 0) {
                at -= from;
                from = 0;
            }
            if (sourceSec > 0) to = std::min(to, sourceSec);
            if (to - from <= 1e-9) continue;
            pieces.push_back({start + at / secondsPerBeat, from, to - from, 0.0});
        }
    }
    if (cut) notes_.line(kTooManyLoops);
    if (clamped) {
        notes_.line(
            QStringLiteral("Some warped audio stretched past SUBstation's tempos (%1 to %2 BPM) plays at their limit.")
                .arg(kMinSegmentBpm)
                .arg(kMaxSegmentBpm));
    }
    if (pieces.empty()) return;

    const double gainDb = std::clamp(gainToDb(clip.number(u"SampleVolume", 1.0)), automation::kMinVolumeDb, 24.0);
    // Fades are in the clip's time: beats if it is warped, else seconds.
    const double fadeIn = clip.number(u"Fades/FadeInLength");
    const double fadeOut = clip.number(u"Fades/FadeOutLength");
    for (size_t i = 0; i < pieces.size(); ++i) {
        const Piece& piece = pieces[i];
        QJsonObject data{{QStringLiteral("id"), newId()},
                         {QStringLiteral("name"), name},
                         {QStringLiteral("path"), path},
                         {QStringLiteral("start_beat"), piece.beat},
                         {QStringLiteral("duration_sec"), piece.seconds},
                         {QStringLiteral("offset_sec"), piece.offset},
                         {QStringLiteral("source_duration_sec"), sourceSec},
                         {QStringLiteral("gain_db"), gainDb},
                         {QStringLiteral("warp"), warped},
                         {QStringLiteral("warp_mode"), warpMode(clip.integer(u"WarpMode", 4))},
                         {QStringLiteral("segment_bpm"), warped ? piece.bpm : 0.0},
                         {QStringLiteral("transpose"), std::clamp(clip.integer(u"PitchCoarse"), -48, 48)},
                         {QStringLiteral("detune"), std::clamp(clip.number(u"PitchFine"), -50.0, 50.0)},
                         {QStringLiteral("pan"), 0.0}};
        const double toSeconds = warped ? 60.0 / piece.bpm : 1.0;
        if (i == 0 && fadeIn > 0) {
            data[QStringLiteral("fade_in_sec")] = std::min(fadeIn * toSeconds, piece.seconds);
            data[QStringLiteral("fade_in_curve")] = std::clamp(clip.number(u"Fades/FadeInCurveSlope"), -1.0, 1.0);
        }
        if (i + 1 == pieces.size() && fadeOut > 0) {
            data[QStringLiteral("fade_out_sec")] = std::min(fadeOut * toSeconds, piece.seconds);
            data[QStringLiteral("fade_out_curve")] = std::clamp(clip.number(u"Fades/FadeOutCurveSlope"), -1.0, 1.0);
        }
        if (clip.flag(u"Disabled")) data[QStringLiteral("muted")] = true;
        out.append(data);
        ++clipCount_;
    }
}

// --- Automation -----------------------------------------------------------------------------------

void Importer::readAutomation(const Element& track) {
    const Element* envelopes = track.at(u"AutomationEnvelopes/Envelopes");
    if (!envelopes) return;
    int unknown = 0;
    for (const Element* envelope : envelopes->all(u"AutomationEnvelope")) {
        const QString pointee = envelope->value(u"EnvelopeTarget/PointeeId").value_or(QString());
        const Element* events = envelope->at(u"Automation/Events");
        if (!events || events->children.empty()) continue;
        const auto target = targets_.constFind(pointee);
        if (target == targets_.constEnd()) {
            if (!settingTargets_.contains(pointee)) ++unknown;  // (the tempo's: readSettings)
            continue;
        }
        QJsonArray points;
        for (const Element* event : events->all()) {
            const QString text = event->attribute(u"Value");
            const double value = text == u"true" ? 1.0 : text == u"false" ? 0.0 : event->numberAttribute(u"Value");
            const double beat = std::max(0.0, event->numberAttribute(u"Time"));
            points.append(QJsonArray{beat, std::clamp(target->normalized(value), 0.0, 1.0), 0.0});
        }
        QJsonObject owned = automation_.value(target->owner);
        owned[target->key] = points;
        automation_.insert(target->owner, owned);
    }
    if (unknown > 0) {
        notes_.listed(
            QStringLiteral("Automation left out (of what didn't come across)"),
            track.tag == u"MainTrack" || track.tag == u"MasterTrack" ? QStringLiteral("Main") : liveName(track));
    }
}

}  // namespace

QByteArray vstPreset(const QString& classId, const QByteArray& processorState, const QByteArray& controllerState) {
    // Steinberg's .vstpreset: a header ("VST3", version 1, the class id as 32
    // ASCII hex digits, where the chunk list is), the chunks, then the list
    // ("List", how many, then id, offset and size each). Little-endian.
    constexpr int kHeaderSize = 4 + 4 + 32 + 8;
    QByteArray bytes("VST3");
    appendLe32(bytes, 1);
    bytes.append(classId.toLatin1().leftJustified(32, '0', true));
    const quint64 componentOffset = kHeaderSize;
    const quint64 controllerOffset = componentOffset + quint64(processorState.size());
    const quint64 listOffset = controllerOffset + quint64(controllerState.size());
    appendLe64(bytes, listOffset);
    bytes.append(processorState);
    bytes.append(controllerState);
    bytes.append("List");
    appendLe32(bytes, controllerState.isEmpty() ? 1 : 2);
    bytes.append("Comp");
    appendLe64(bytes, componentOffset);
    appendLe64(bytes, quint64(processorState.size()));
    if (!controllerState.isEmpty()) {
        bytes.append("Cont");
        appendLe64(bytes, controllerOffset);
        appendLe64(bytes, quint64(controllerState.size()));
    }
    return bytes;
}

QByteArray vst2CompatibleState(const QByteArray& chunk, quint32 vst2Id, quint32 version, bool bank, int programs) {
    // "VstW" (its header's size, 8; version 1; not bypassed), then the VST2's fxb
    // or fxp with its opaque chunk ("CcnK", its size, "FBCh" or "FPCh", ...). Big-endian.
    QByteArray block;
    block.append(bank ? "FBCh" : "FPCh");
    appendBe32(block, bank ? 2 : 1);  // fx version
    appendBe32(block, vst2Id);
    appendBe32(block, version);
    appendBe32(block, quint32(std::max(1, programs)));  // programs (a bank's) or parameters (a program's)
    block.append(QByteArray(bank ? 128 : 28, '\0'));    // a bank's current program and future; a program's name
    appendBe32(block, quint32(chunk.size()));
    block.append(chunk);
    QByteArray bytes("VstW");
    appendBe32(bytes, 8);
    appendBe32(bytes, 1);
    appendBe32(bytes, 0);
    bytes.append("CcnK");
    appendBe32(bytes, quint32(block.size()));
    bytes.append(block);
    return bytes;
}

ImportResult importLiveSet(const Element& root, const QString& setPath, const ImportOptions& options) {
    if (!root.child(u"LiveSet")) return {};
    Importer importer(root, setPath, options);
    return importer.run();
}

}  // namespace sub::app::live
