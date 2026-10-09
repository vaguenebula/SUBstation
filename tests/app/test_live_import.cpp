// Importing Ableton Live Sets (io/LiveSet.h, io/LiveImport.h,
// Session::importLiveSet): sets written here as Live writes them (gzip or plain
// XML), and what each part of one becomes: the song's settings; tracks, groups,
// returns, the main track, their mixers and routing; MIDI clips (loops written
// out); audio clips (found where the set moved, warped as their markers say);
// devices (VST3 plug-ins with their settings, VST2 ones as their VST3, Live's
// own that SUBstation has, racks); Drum Racks as a track a pad; automation; and
// the notes on what didn't come across.

#include "BridgeTestSupport.h"
#include "SessionFixture.h"
#include "TestSupport.h"

#include "io/LiveImport.h"
#include "io/LiveSet.h"
#include "io/Serialization.h"
#include "model/Automation.h"
#include "model/DeviceState.h"
#include "model/Devices.h"
#include "model/Errors.h"
#include "model/ParamSpec.h"
#include "model/Project.h"
#include "plugins/Vst3Ids.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QSettings>
#include <QTest>
#include <QtEndian>

#include <array>
#include <cmath>

using namespace sub::app;
using sub::app::test::SessionFixture;
using sub::app::test::TempDir;

namespace {

// --- Writing sets as Live does ------------------------------------------------------------------

QString value(const QString& tag, const QString& v) { return QStringLiteral("<%1 Value=\"%2\" />").arg(tag, v); }
QString value(const QString& tag, double v) { return value(tag, QString::number(v, 'g', 12)); }
QString value(const QString& tag, bool v) { return value(tag, v ? QStringLiteral("true") : QStringLiteral("false")); }
QString element(const QString& tag, const QString& inner, const QString& attributes = {}) {
    return QStringLiteral("<%1%2>%3</%1>").arg(tag, attributes.isEmpty() ? QString() : u' ' + attributes, inner);
}
// A parameter: <Tag><Manual Value="v" /><AutomationTarget Id="target" /></Tag>.
QString param(const QString& tag, const QString& v, int target = 0) {
    return element(tag, value(QStringLiteral("Manual"), v) +
                            (target ? QStringLiteral("<AutomationTarget Id=\"%1\" />").arg(target) : QString()));
}
QString param(const QString& tag, double v, int target = 0) { return param(tag, QString::number(v, 'g', 12), target); }
QString param(const QString& tag, bool v, int target = 0) {
    return param(tag, v ? QStringLiteral("true") : QStringLiteral("false"), target);
}

QString envelope(int target, const QString& events) {
    return element(
        QStringLiteral("AutomationEnvelope"),
        element(QStringLiteral("EnvelopeTarget"), value(QStringLiteral("PointeeId"), QString::number(target))) +
            element(QStringLiteral("Automation"), element(QStringLiteral("Events"), events)));
}
QString floatEvent(double time, double v) {
    return QStringLiteral("<FloatEvent Time=\"%1\" Value=\"%2\" />").arg(time).arg(v);
}

struct Mixer {
    double volume = 1.0;  // linear
    double pan = 0.0;
    bool on = true;
    bool solo = false;
    QString sends;  // TrackSendHolders
    int volumeTarget = 0;
};

QString mixer(const Mixer& m) {
    return element(QStringLiteral("Mixer"),
                   element(QStringLiteral("Sends"), m.sends) + param(QStringLiteral("Speaker"), m.on) +
                       value(QStringLiteral("SoloSink"), m.solo) + param(QStringLiteral("Pan"), m.pan) +
                       param(QStringLiteral("Volume"), m.volume, m.volumeTarget));
}

struct LiveTrack {
    QString kind = QStringLiteral("AudioTrack");
    int id = 1;
    QString name = QStringLiteral("1-Audio");  // EffectiveName
    QString userName;
    int color = 0;
    int group = -1;
    bool unfolded = true;
    Mixer mix;
    QString input = QStringLiteral("AudioIn/External/S0");
    QString output = QStringLiteral("AudioOut/GroupTrack");
    int monitoring = 1;
    QString clips;    // MidiClip / AudioClip elements
    QString devices;  // device elements
    QString envelopes;
};

QString track(const LiveTrack& t) {
    const bool midi = t.kind == u"MidiTrack";
    const QString events = element(QStringLiteral("ArrangerAutomation"), element(QStringLiteral("Events"), t.clips));
    const QString sequencer =
        element(QStringLiteral("MainSequencer"),
                (midi ? element(QStringLiteral("ClipTimeable"), events) : element(QStringLiteral("Sample"), events)) +
                    value(QStringLiteral("MonitoringEnum"), QString::number(t.monitoring)));
    return element(
        t.kind,
        element(QStringLiteral("Name"),
                value(QStringLiteral("EffectiveName"), t.name) + value(QStringLiteral("UserName"), t.userName)) +
            value(QStringLiteral("Color"), QString::number(t.color)) +
            element(QStringLiteral("AutomationEnvelopes"), element(QStringLiteral("Envelopes"), t.envelopes)) +
            value(QStringLiteral("TrackGroupId"), QString::number(t.group)) +
            value(QStringLiteral("TrackUnfolded"), t.unfolded) +
            element(QStringLiteral("DeviceChain"),
                    element(QStringLiteral("AudioInputRouting"), value(QStringLiteral("Target"), t.input)) +
                        element(QStringLiteral("AudioOutputRouting"), value(QStringLiteral("Target"), t.output)) +
                        mixer(t.mix) + sequencer +
                        element(QStringLiteral("DeviceChain"), element(QStringLiteral("Devices"), t.devices))),
        QStringLiteral("Id=\"%1\"").arg(t.id));
}

struct Song {
    double tempo = 120.0;
    int signature = 201;  // 4/4: (numerator - 1) + 99 * log2(denominator)
    QString tracks;
    QString masterDevices;
    QString masterEnvelopes;
    QString extra;               // more of LiveSet: Transport, ScaleInformation, SendsPre...
    bool settingTargets = true;  // the tempo's and time signature's AutomationTargets
};

QByteArray liveSet(const Song& song) {
    const QString master = element(
        QStringLiteral("MainTrack"),
        element(QStringLiteral("AutomationEnvelopes"), element(QStringLiteral("Envelopes"), song.masterEnvelopes)) +
            element(
                QStringLiteral("DeviceChain"),
                element(QStringLiteral("Mixer"),
                        param(QStringLiteral("Volume"), 1.0, 5) + param(QStringLiteral("Pan"), 0.0, 3) +
                            param(QStringLiteral("Tempo"), song.tempo, song.settingTargets ? 8 : 0) +
                            param(QStringLiteral("TimeSignature"), QString::number(song.signature),
                                  song.settingTargets ? 10 : 0)) +
                    element(QStringLiteral("DeviceChain"), element(QStringLiteral("Devices"), song.masterDevices))));
    const QString xml =
        QStringLiteral(
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<Ableton MajorVersion=\"5\" MinorVersion=\"12.0_12300\" Creator=\"Ableton Live 12.3.2\">") +
        element(QStringLiteral("LiveSet"), element(QStringLiteral("Tracks"), song.tracks) + master + song.extra) +
        QStringLiteral("</Ableton>");
    return xml.toUtf8();
}

uint32_t crc32(const QByteArray& bytes) {
    uint32_t crc = 0xFFFFFFFFu;
    for (const char byte : bytes) {
        crc ^= static_cast<unsigned char>(byte);
        for (int k = 0; k < 8; ++k) crc = (crc & 1) ? 0xEDB88320u ^ (crc >> 1) : crc >> 1;
    }
    return crc ^ 0xFFFFFFFFu;
}

// gzip, as Live compresses sets: zlib's deflate (qCompress's, without its
// length, header and Adler-32), between gzip's header and its CRC and length.
QByteArray gzip(const QByteArray& data) {
    const QByteArray z = qCompress(data, 9);
    QByteArray out("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\xff", 10);
    out += z.mid(4 + 2, z.size() - 4 - 2 - 4);
    char word[4];
    qToLittleEndian(crc32(data), word);
    out.append(word, 4);
    qToLittleEndian(static_cast<quint32>(data.size()), word);
    out.append(word, 4);
    return out;
}

QString writeSet(const TempDir& dir, const QString& name, const QByteArray& xml, bool compressed = true) {
    const QString path = dir.path(name);
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.write(compressed ? gzip(xml) : xml);
    return path;
}

live::ImportResult import(const QByteArray& xml, const QString& setPath = QStringLiteral("/nowhere/song.als"),
                          const live::ImportOptions& options = {}) {
    const auto root = live::parseLiveSet(xml, QStringLiteral("song.als"));
    return live::importLiveSet(*root, setPath, options);
}

// The project an import makes, loaded as a project file is.
struct Imported {
    Project project;
    QStringList notes;
};

std::unique_ptr<Imported> load(const QByteArray& xml, const QString& setPath = QStringLiteral("/nowhere/song.als"),
                               const live::ImportOptions& options = {}) {
    auto imported = std::make_unique<Imported>();
    const live::ImportResult result = import(xml, setPath, options);
    loadInto(imported->project, result.project);
    imported->notes = result.notes;
    return imported;
}

const Track* named(const Project& project, const QString& name) {
    for (const Track& t : project.tracks()) {
        if (t.name == name) return &t;
    }
    return nullptr;
}

bool noteSays(const QStringList& notes, const QString& part) {
    for (const QString& note : notes) {
        if (note.contains(part)) return true;
    }
    return false;
}

QString midiClip(double time, double length, const QString& keyTracks, const QString& loop = {},
                 bool disabled = false) {
    const QString loopXml =
        loop.isEmpty()
            ? element(QStringLiteral("Loop"),
                      value(QStringLiteral("LoopStart"), 0.0) + value(QStringLiteral("LoopEnd"), length) +
                          value(QStringLiteral("StartRelative"), 0.0) + value(QStringLiteral("LoopOn"), false))
            : loop;
    return element(QStringLiteral("MidiClip"),
                   value(QStringLiteral("CurrentStart"), time) + value(QStringLiteral("CurrentEnd"), time + length) +
                       loopXml + value(QStringLiteral("Name"), QString()) +
                       value(QStringLiteral("Disabled"), disabled) +
                       element(QStringLiteral("Notes"), element(QStringLiteral("KeyTracks"), keyTracks)),
                   QStringLiteral("Id=\"0\" Time=\"%1\"").arg(time));
}

QString keyTrack(int key, const QString& events) {
    return element(QStringLiteral("KeyTrack"),
                   element(QStringLiteral("Notes"), events) + value(QStringLiteral("MidiKey"), QString::number(key)));
}

QString note(double time, double duration, int velocity = 100, bool enabled = true) {
    return QStringLiteral(
               "<MidiNoteEvent Time=\"%1\" Duration=\"%2\" Velocity=\"%3\" OffVelocity=\"64\" NoteId=\"1\"%4 />")
        .arg(time)
        .arg(duration)
        .arg(velocity)
        .arg(enabled ? QString() : QStringLiteral(" IsEnabled=\"false\""));
}

QString loop(double start, double end, double startRelative, bool on) {
    return element(QStringLiteral("Loop"),
                   value(QStringLiteral("LoopStart"), start) + value(QStringLiteral("LoopEnd"), end) +
                       value(QStringLiteral("StartRelative"), startRelative) + value(QStringLiteral("LoopOn"), on));
}

QString sampleRef(const QString& path, const QString& relative, double frames, double rate) {
    return element(QStringLiteral("SampleRef"),
                   element(QStringLiteral("FileRef"), value(QStringLiteral("RelativePathType"), QStringLiteral("3")) +
                                                          value(QStringLiteral("RelativePath"), relative) +
                                                          value(QStringLiteral("Path"), path)) +
                       value(QStringLiteral("DefaultDuration"), frames) +
                       value(QStringLiteral("DefaultSampleRate"), rate));
}

struct AudioClipXml {
    double time = 0.0;
    double length = 4.0;  // beats on the timeline
    QString loopXml;
    QString sample;
    bool warped = true;
    QString markers;  // WarpMarker elements
    int warpMode = 4;
    int pitch = 0;
    double fine = 0.0;
    double volume = 1.0;
    double fadeIn = 0.0;
    double fadeOut = 0.0;
    QString name = QStringLiteral("Clip");
};

QString audioClip(const AudioClipXml& c) {
    return element(QStringLiteral("AudioClip"),
                   value(QStringLiteral("CurrentStart"), c.time) +
                       value(QStringLiteral("CurrentEnd"), c.time + c.length) + c.loopXml +
                       value(QStringLiteral("Name"), c.name) + value(QStringLiteral("Disabled"), false) +
                       value(QStringLiteral("IsWarped"), c.warped) + c.sample +
                       value(QStringLiteral("WarpMode"), QString::number(c.warpMode)) +
                       element(QStringLiteral("Fades"), value(QStringLiteral("FadeInLength"), c.fadeIn) +
                                                            value(QStringLiteral("FadeOutLength"), c.fadeOut)) +
                       value(QStringLiteral("PitchCoarse"), QString::number(c.pitch)) +
                       value(QStringLiteral("PitchFine"), c.fine) + value(QStringLiteral("SampleVolume"), c.volume) +
                       element(QStringLiteral("WarpMarkers"), c.markers),
                   QStringLiteral("Id=\"0\" Time=\"%1\"").arg(c.time));
}

QString marker(double sec, double beat) {
    return QStringLiteral("<WarpMarker Id=\"0\" SecTime=\"%1\" BeatTime=\"%2\" />")
        .arg(sec, 0, 'g', 12)
        .arg(beat, 0, 'g', 12);
}

QString hex(const QByteArray& bytes) { return QString::fromLatin1(bytes.toHex().toUpper()); }

QString vst3Device(const QString& name, const std::array<quint32, 4>& words, const QByteArray& processor,
                   const QByteArray& controller, int deviceType, const QString& parameters = {}) {
    QString uid;
    for (int i = 0; i < 4; ++i)
        uid += value(QStringLiteral("Fields.%1").arg(i), QString::number(static_cast<qint32>(words[size_t(i)])));
    return element(
        QStringLiteral("PluginDevice"),
        param(QStringLiteral("On"), true, 900) +
            element(QStringLiteral("PluginDesc"),
                    element(QStringLiteral("Vst3PluginInfo"),
                            element(QStringLiteral("Preset"),
                                    element(QStringLiteral("Vst3Preset"),
                                            element(QStringLiteral("ProcessorState"), hex(processor)) +
                                                element(QStringLiteral("ControllerState"), hex(controller)))) +
                                value(QStringLiteral("Name"), name) + element(QStringLiteral("Uid"), uid) +
                                value(QStringLiteral("DeviceType"), QString::number(deviceType)))) +
            element(QStringLiteral("ParameterList"), parameters),
        QStringLiteral("Id=\"0\""));
}

QString vst2Device(const QString& plugName, quint32 uniqueId, const QByteArray& chunk) {
    return element(
        QStringLiteral("PluginDevice"),
        param(QStringLiteral("On"), true) +
            element(QStringLiteral("PluginDesc"),
                    element(QStringLiteral("VstPluginInfo"),
                            value(QStringLiteral("PlugName"), plugName) +
                                value(QStringLiteral("UniqueId"), QString::number(static_cast<qint32>(uniqueId))) +
                                value(QStringLiteral("Version"), QStringLiteral("131329")) +
                                element(QStringLiteral("Preset"),
                                        element(QStringLiteral("VstPreset"),
                                                value(QStringLiteral("Type"), QStringLiteral("1178747752")) +
                                                    value(QStringLiteral("ProgramCount"), QStringLiteral("1")) +
                                                    element(QStringLiteral("Buffer"), hex(chunk)))))),
        QStringLiteral("Id=\"0\""));
}

// A .vstpreset's chunk ("Comp" or "Cont"); empty if it has none.
QByteArray presetChunk(const QByteArray& preset, const char* id) {
    if (preset.size() < 48 || !preset.startsWith("VST3")) return {};
    const qint64 list = qFromLittleEndian<qint64>(preset.constData() + 40);
    const int count = qFromLittleEndian<qint32>(preset.constData() + list + 4);
    for (int i = 0; i < count; ++i) {
        const char* entry = preset.constData() + list + 8 + i * 20;
        if (QByteArray(entry, 4) != id) continue;
        return preset.mid(qFromLittleEndian<qint64>(entry + 4), qFromLittleEndian<qint64>(entry + 12));
    }
    return {};
}

std::array<quint32, 4> wordsOf(const char (&bytes)[17]) {
    std::array<quint32, 4> words{};
    for (int i = 0; i < 4; ++i) words[size_t(i)] = qFromBigEndian<quint32>(bytes + 4 * i);
    return words;
}

QString classId(const std::array<quint32, 4>& w) {
    return QString::fromStdString(sub::vst3::classIdFromWords(w[0], w[1], w[2], w[3]));
}

}  // namespace

class TestLiveImport : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { test::prepareApplication(); }
    void init() { QSettings().clear(); }

    // Gzip (as Live writes them) or plain XML; what isn't a set, or is damaged, says so.
    void readingSets() {
        TempDir dir;
        Song song;
        song.tempo = 140.0;
        const QByteArray xml = liveSet(song);
        for (const bool compressed : {true, false}) {
            const auto root = live::readLiveSet(writeSet(dir, QStringLiteral("set.als"), xml, compressed));
            QCOMPARE(root->tag, QStringLiteral("Ableton"));
            QCOMPARE(root->number(u"LiveSet/MainTrack/DeviceChain/Mixer/Tempo/Manual"), 140.0);
        }
        QVERIFY(live::isGzip(gzip(xml)));
        QCOMPARE(live::gunzip(gzip(xml), QStringLiteral("x")), xml);

        QByteArray broken = gzip(xml);
        broken[broken.size() - 6] = char(broken[broken.size() - 6] ^ 0x5A);  // (its CRC)
        try {
            live::parseLiveSet(broken, QStringLiteral("broken.als"));
            QFAIL("a damaged set was read");
        } catch (const ProjectFileError& error) {
            QCOMPARE(error.message(), QStringLiteral("broken.als is damaged: its data doesn't check out"));
        }
        // A cut file's last bytes (its length, as gzip has it) aren't taken on trust: no gigabytes asked for.
        QByteArray cut = gzip(xml);
        for (int i = 1; i <= 4; ++i) cut[cut.size() - i] = char(0xF0);
        try {
            live::parseLiveSet(cut, QStringLiteral("cut.als"));
            QFAIL("a set with a broken length was read");
        } catch (const ProjectFileError& error) {
            QCOMPARE(error.message(), QStringLiteral("cut.als is damaged: its compressed data is broken"));
        }
        try {
            live::parseLiveSet(QByteArrayLiteral("<?xml version=\"1.0\"?><Project />"), QStringLiteral("other.als"));
            QFAIL("not a set was read");
        } catch (const ProjectFileError& error) {
            QCOMPARE(error.message(), QStringLiteral("other.als is not an Ableton Live Set"));
        }
        try {
            live::parseLiveSet(QByteArrayLiteral("<Ableton><LiveSet>"), QStringLiteral("cut.als"));
            QFAIL("a cut set was read");
        } catch (const ProjectFileError& error) {
            QVERIFY(error.message().startsWith(QStringLiteral("cut.als is damaged")));
        }
        QVERIFY_THROWS_EXCEPTION(ProjectFileError, live::readLiveSet(dir.path(QStringLiteral("none.als"))));

        // Values that aren't numbers, or are past what they're read as, are the fallback.
        const auto values = live::parseLiveSet(
            QByteArrayLiteral("<Ableton><LiveSet><A Value=\"nan\" /><B Value=\"-inf\" /><C Value=\"1e20\" />"
                              "<D Value=\"3.0\" X=\"inf\" Y=\"0.25\" /></LiveSet></Ableton>"),
            QStringLiteral("values.als"));
        const live::Element* set = values->child(u"LiveSet");
        QCOMPARE(set->number(u"A", 120.0), 120.0);
        QCOMPARE(set->number(u"B", 1.0), 1.0);
        QCOMPARE(set->integer(u"A", 7), 7);
        QCOMPARE(set->integer(u"C", 7), 7);
        QCOMPARE(set->integer(u"D", 7), 3);
        QCOMPARE(set->at(u"D")->numberAttribute(u"X", 2.0), 2.0);
        QCOMPARE(set->at(u"D")->numberAttribute(u"Y", 2.0), 0.25);
    }

    // Tempo, time signature, loop and key; a tempo that changes is noted.
    void theSongsSettings() {
        Song song;
        song.tempo = 174.0;
        song.signature = 5 + 99 * 3;  // 6/8
        song.extra =
            element(QStringLiteral("Transport"), value(QStringLiteral("LoopOn"), true) +
                                                     value(QStringLiteral("LoopStart"), 8.0) +
                                                     value(QStringLiteral("LoopLength"), 16.0)) +
            element(QStringLiteral("ScaleInformation"), value(QStringLiteral("Root"), QStringLiteral("9")) +
                                                            value(QStringLiteral("Name"), QStringLiteral("1"))) +
            value(QStringLiteral("InKey"), true);
        auto imported = load(liveSet(song));
        const Project& p = imported->project;
        QCOMPARE(p.tempo(), 174.0);
        QCOMPARE(p.timeSignature().numerator, 6);
        QCOMPARE(p.timeSignature().denominator, 8);
        QVERIFY(p.loopEnabled());
        QCOMPARE(p.loopStart(), 8.0);
        QCOMPARE(p.loopEnd(), 24.0);
        QCOMPARE(p.keyName(), QStringLiteral("Am"));
        QVERIFY(imported->notes.isEmpty());

        // C major is Live's own scale until one is chosen: no key. An automated
        // tempo starts at its first value; its changes are noted.
        song.extra =
            element(QStringLiteral("ScaleInformation"), value(QStringLiteral("Root"), QStringLiteral("0")) +
                                                            value(QStringLiteral("Name"), QStringLiteral("0"))) +
            value(QStringLiteral("InKey"), true);
        song.masterEnvelopes = envelope(8, floatEvent(-63072000, 150) + floatEvent(64, 160));
        imported = load(liveSet(song));
        QCOMPARE(imported->project.keyName(), QString());
        QCOMPARE(imported->project.tempo(), 150.0);
        QVERIFY(noteSays(imported->notes,
                         QStringLiteral("The tempo changes during the song; SUBstation has one tempo: 150 BPM.")));

        // Past SUBstation's tempos: the one it has is said.
        song.masterEnvelopes = envelope(8, floatEvent(-63072000, 1200) + floatEvent(64, 160));
        imported = load(liveSet(song));
        QCOMPARE(imported->project.tempo(), 999.0);
        QVERIFY(noteSays(imported->notes, QStringLiteral("SUBstation has one tempo: 999 BPM.")));

        // A tempo without an automation target, and an envelope without one: nothing is taken for the tempo.
        song.settingTargets = false;
        song.masterEnvelopes =
            element(QStringLiteral("AutomationEnvelope"),
                    QStringLiteral("<EnvelopeTarget />") +
                        element(QStringLiteral("Automation"), element(QStringLiteral("Events"), floatEvent(0, 0.5))));
        imported = load(liveSet(song));
        QCOMPARE(imported->project.tempo(), 174.0);
        QVERIFY(noteSays(imported->notes, QStringLiteral("Automation left out (of what didn't come across): Main.")));
    }

    // Tracks in groups (nested, folded), returns and their sends, names, colours,
    // mixers, and where their audio goes.
    void tracksGroupsAndMixers() {
        Song song;
        LiveTrack group{QStringLiteral("GroupTrack"), 10, QStringLiteral("Drums")};
        group.userName = QStringLiteral("Drums");
        group.color = 4;
        group.unfolded = false;
        LiveTrack audio{QStringLiteral("AudioTrack"), 11, QStringLiteral("2-Kick")};
        audio.group = 10;
        audio.color = 14;
        audio.mix =
            Mixer{0.5, -0.25, false, true,
                  element(QStringLiteral("TrackSendHolder"),
                          element(QStringLiteral("Send"), value(QStringLiteral("Manual"), 0.25))) +
                      element(QStringLiteral("TrackSendHolder"),
                              element(QStringLiteral("Send"), value(QStringLiteral("Manual"), 0.0003162277571)))};
        audio.output = QStringLiteral("AudioOut/Main");
        LiveTrack midi{QStringLiteral("MidiTrack"), 12, QStringLiteral("3-MIDI")};
        midi.output = QStringLiteral("AudioOut/Track.13/TrackIn");
        LiveTrack bus{QStringLiteral("AudioTrack"), 13, QStringLiteral("4-Audio")};
        bus.userName = QStringLiteral("Bus");
        bus.monitoring = 0;  // In
        bus.input = QStringLiteral("AudioIn/None");
        LiveTrack silent{QStringLiteral("AudioTrack"), 14, QStringLiteral("5-Audio")};
        silent.output = QStringLiteral("AudioOut/None");
        silent.input = QStringLiteral("AudioIn/Track.12/PostFxOut");
        LiveTrack resampled{QStringLiteral("AudioTrack"), 15, QStringLiteral("6-Audio")};
        resampled.input = QStringLiteral("AudioIn/Main");
        resampled.monitoring = 2;  // Off: it plays its clips
        LiveTrack reverb{QStringLiteral("ReturnTrack"), 20, QStringLiteral("A-Reverb")};
        LiveTrack delay{QStringLiteral("ReturnTrack"), 21, QStringLiteral("B-Delay")};
        song.tracks = track(group) + track(audio) + track(midi) + track(bus) + track(silent) + track(resampled) +
                      track(reverb) + track(delay);
        song.extra = element(QStringLiteral("SendsPre"),
                             value(QStringLiteral("SendPreBool"), true) + value(QStringLiteral("SendPreBool"), false));
        auto imported = load(liveSet(song));
        const Project& p = imported->project;
        QCOMPARE(p.tracks().size(), size_t{6});
        QCOMPARE(p.returns().size(), size_t{2});
        const Track& g = p.tracks()[0];
        QVERIFY(g.isGroup() && g.folded);
        QCOMPARE(g.name, QStringLiteral("Drums"));
        QCOMPARE(g.color, QStringLiteral("#bffb00"));
        const Track& kick = p.tracks()[1];
        QCOMPARE(kick.name, QStringLiteral("2 Kick"));  // Live's numbered name, numbered here
        QCOMPARE(kick.nameSource(), QStringLiteral("# Kick"));
        QCOMPARE(kick.parent, std::optional<QString>(g.id));
        QCOMPARE(kick.color, QStringLiteral("#ff3636"));
        QVERIFY(std::abs(kick.volumeDb - 20.0 * std::log10(0.5)) < 1e-9);
        QCOMPARE(kick.pan, -0.25);
        QVERIFY(kick.mute && kick.solo);
        QCOMPARE(kick.output, Output::master());  // past its group
        QCOMPARE(kick.sends.size(), 1);           // (the one at -70 dB is none)
        const Send send = kick.sends.value(p.returns()[0].id);
        QVERIFY(std::abs(send.levelDb - 20.0 * std::log10(0.25)) < 1e-9);
        QVERIFY(send.preFader);
        QCOMPARE(kick.input, (std::vector<int>{0, 1}));
        const Track& keys = p.tracks()[2];
        QVERIFY(keys.isMidi() && !keys.parent);
        QCOMPARE(keys.output, Output::track(p.tracks()[3].id));
        QCOMPARE(p.tracks()[3].name, QStringLiteral("Bus"));
        QCOMPARE(p.tracks()[3].monitor, QStringLiteral("in"));
        QCOMPARE(p.tracks()[4].output, Output::none());
        QCOMPARE(p.tracks()[4].inputTrack, std::optional<QString>(keys.id));
        QCOMPARE(p.tracks()[4].inputTap, kPreFader);
        QVERIFY(!p.tracks()[5].inputTrack);  // (resampling, not monitored: not taken)
        QCOMPARE(p.tracks()[5].monitor, QStringLiteral("off"));
        QCOMPARE(p.returns()[0].name, QStringLiteral("Reverb"));
    }

    // Live 10 keeps colours as ColorIndex.
    void live10Colours() {
        Song song;
        LiveTrack t{QStringLiteral("AudioTrack"), 1, QStringLiteral("1-Audio")};
        t.color = 14;
        song.tracks = track(t).replace(QStringLiteral("<Color Value"), QStringLiteral("<ColorIndex Value"));
        QVERIFY(song.tracks.contains(QStringLiteral("ColorIndex")));
        QCOMPARE(load(liveSet(song))->project.tracks()[0].color, QStringLiteral("#ff3636"));
    }

    // MIDI clips: notes in place from the start marker; a loop written out loop
    // after loop, notes cut at its end; deactivated notes and clips stay so.
    void midiClipsAndLoops() {
        Song song;
        LiveTrack keys{QStringLiteral("MidiTrack"), 1, QStringLiteral("1-Keys")};
        keys.clips =
            midiClip(8.0, 4.0,
                     keyTrack(60, note(0.0, 1.0, 90) + note(2.0, 1.0, 64, false)) + keyTrack(64, note(3.5, 2.0)), {},
                     true) +
            // From its start marker at 1, round a loop from 0 to 2: 1..2, 0..2, 0..2, 0..1.
            midiClip(16.0, 6.0, keyTrack(62, note(0.0, 0.5) + note(1.5, 1.0)), loop(0.0, 2.0, 1.0, true));
        song.tracks = track(keys);
        auto imported = load(liveSet(song));
        const Track& t = imported->project.tracks()[0];
        QCOMPARE(t.clips.size(), size_t{2});
        const Clip& first = t.clips[0];
        QCOMPARE(first.startBeat, 8.0);
        QCOMPARE(first.durationBeats, 4.0);
        QVERIFY(first.muted);
        QCOMPARE(first.notes.size(), size_t{3});
        QCOMPARE(first.notes[0], (Note{60, 0.0, 1.0, 90, false}));
        QCOMPARE(first.notes[1], (Note{60, 2.0, 1.0, 64, true}));
        QCOMPARE(first.notes[2], (Note{64, 3.5, 0.5, 100, false}));  // (cut at the clip's end)
        const Clip& looped = t.clips[1];
        QCOMPARE(looped.startBeat, 16.0);
        QCOMPARE(looped.durationBeats, 6.0);
        std::vector<std::pair<double, double>> heard;
        for (const Note& n : looped.notes) heard.emplace_back(n.start, n.length);
        // (Notes cut at the loop's end, as Live cuts them.)
        QCOMPARE(heard, (std::vector<std::pair<double, double>>{
                            {0.5, 0.5}, {1.0, 0.5}, {2.5, 0.5}, {3.0, 0.5}, {4.5, 0.5}, {5.0, 0.5}}));
    }

    // Audio clips: their files where the set moved; unwarped ones at their own
    // speed (markers in seconds); warped ones at the tempo their markers say, a
    // clip a stretch; loops written out; gain, pitch, fades, warp mode.
    void audioClipsAndWarping() {
        TempDir dir;
        QDir().mkpath(dir.path(QStringLiteral("Samples")));
        const QString wav =
            test::writeWav(dir.path(QStringLiteral("Samples/loop.wav")), std::vector<float>(2 * 48000 * 4, 0.1f), 2);
        const QString moved = QStringLiteral("F:/Elsewhere/Samples/loop.wav");  // where the set says it was
        const QString ref = sampleRef(moved, QStringLiteral("Samples/loop.wav"), 4 * 48000, 48000);
        Song song;
        song.tempo = 120.0;
        LiveTrack t{QStringLiteral("AudioTrack"), 1, QStringLiteral("1-Audio")};
        AudioClipXml plain;  // unwarped: 2 beats at 120 = 1 s, from 0.5 s into the file
        plain.time = 0.0;
        plain.length = 2.0;
        plain.loopXml = loop(0.5, 1.5, 0.0, false);
        plain.sample = ref;
        plain.warped = false;
        plain.pitch = -3;
        plain.fine = 12.0;
        plain.volume = 0.5;
        AudioClipXml warped;  // warped at 140 BPM (two markers), from beat 1 of its content, a fade in of half a beat
        warped.time = 8.0;
        warped.length = 3.0;
        warped.loopXml = loop(1.0, 7.0, 0.0, false);
        warped.sample = ref;
        warped.markers = marker(0.0, 0.0) + marker(60.0 / 140.0, 1.0);
        warped.fadeIn = 0.5;
        warped.warpMode = 6;
        AudioClipXml stretched;  // its first 2 beats in 1 s (120), the next 2 in 2 s (60): two clips
        stretched.time = 16.0;
        stretched.length = 4.0;
        stretched.loopXml = loop(0.0, 4.0, 0.0, false);
        stretched.sample = ref;
        stretched.markers = marker(0.0, 0.0) + marker(1.0, 2.0) + marker(3.0, 4.0);
        AudioClipXml looped;  // a warped loop of a beat at 120, played for 2.5 beats
        looped.time = 24.0;
        looped.length = 2.5;
        looped.loopXml = loop(0.0, 1.0, 0.0, true);
        looped.sample = ref;
        looped.markers = marker(0.0, 0.0) + marker(0.5, 1.0);
        AudioClipXml missing;
        missing.time = 32.0;
        missing.sample =
            sampleRef(QStringLiteral("F:/Gone/gone.wav"), QStringLiteral("Samples/gone.wav"), 48000, 48000);
        missing.markers = marker(0.0, 0.0) + marker(0.5, 1.0);
        t.clips = audioClip(plain) + audioClip(warped) + audioClip(stretched) + audioClip(looped) + audioClip(missing);
        song.tracks = track(t);
        auto imported = load(liveSet(song), dir.path(QStringLiteral("song.als")));
        const std::vector<Clip>& clips = imported->project.tracks()[0].clips;
        QCOMPARE(clips.size(), size_t{1 + 1 + 2 + 3 + 1});

        const Clip& a = clips[0];
        QCOMPARE(QFileInfo(a.path).canonicalFilePath(), QFileInfo(wav).canonicalFilePath());  // found beside the set
        QVERIFY(!a.warp);
        QCOMPARE(a.offsetSec, 0.5);
        QCOMPARE(a.durationSec, 1.0);
        QCOMPARE(a.sourceDurationSec, 4.0);
        QCOMPARE(a.transpose, -3);
        QCOMPARE(a.detune, 12.0);
        QVERIFY(std::abs(a.gainDb - 20.0 * std::log10(0.5)) < 1e-9);
        QCOMPARE(a.name, QStringLiteral("Clip"));

        const Clip& b = clips[1];
        QVERIFY(b.isWarped());
        QVERIFY(std::abs(b.segmentBpm - 140.0) < 1e-6);
        QCOMPARE(b.startBeat, 8.0);
        QVERIFY(std::abs(b.offsetSec - 60.0 / 140.0) < 1e-9);
        QVERIFY(std::abs(b.lengthBeats() - 3.0) < 1e-9);
        QVERIFY(std::abs(b.fadeInSec - 0.5 * 60.0 / 140.0) < 1e-9);
        QCOMPARE(b.warpMode, QStringLiteral("Formants"));  // (Complex Pro)

        QVERIFY(std::abs(clips[2].segmentBpm - 120.0) < 1e-6 && std::abs(clips[3].segmentBpm - 60.0) < 1e-6);
        QCOMPARE(clips[2].startBeat, 16.0);
        QCOMPARE(clips[2].durationSec, 1.0);
        QCOMPARE(clips[3].startBeat, 18.0);
        QCOMPARE(clips[3].offsetSec, 1.0);
        QCOMPARE(clips[3].durationSec, 2.0);

        for (int i = 0; i < 3; ++i) {
            const Clip& piece = clips[size_t(4 + i)];
            QCOMPARE(piece.startBeat, 24.0 + i);
            QCOMPARE(piece.offsetSec, 0.0);
            QCOMPARE(piece.durationSec, i < 2 ? 0.5 : 0.25);
        }
        QCOMPARE(clips[7].path, QStringLiteral("F:/Gone/gone.wav"));  // (the File Manager finds it)
        QVERIFY(noteSays(imported->notes, QStringLiteral("1 audio file isn't where the set says")));
    }

    // VST3 plug-ins with their settings (a .vstpreset of Live's states), found by
    // the plug-in index; VST2 ones as the VST3 replacing them (with their
    // settings) or of their name (at its defaults), else left out.
    void plugIns() {
        const std::array<quint32, 4> proQ = wordsOf("FabFilterProQ4!!");
        const std::array<quint32, 4> vintage =
            wordsOf("VSTVlvvvalhallav");  // Steinberg's id of the VST3 replacing VST2 'Vlvv'
        const std::array<quint32, 4> serum = wordsOf("XfsXSerumVST3abc");
        live::ImportOptions options;
        options.plugins = {
            PluginInfo{QStringLiteral("Pro-Q 4"), QStringLiteral("VST3"),
                       QStringLiteral("C:/VST3/FabFilter Pro-Q 4.vst3"), classId(proQ), QStringLiteral("FabFilter"),
                       QStringLiteral("4.0"), QStringLiteral("Fx|EQ"), false},
            PluginInfo{QStringLiteral("ValhallaVintageVerb"), QStringLiteral("VST3"),
                       QStringLiteral("C:/VST3/Vintage.vst3"), classId(vintage), QStringLiteral("Valhalla DSP"),
                       QStringLiteral("2.0"), QStringLiteral("Fx|Reverb"), false},
            PluginInfo{QStringLiteral("Serum"), QStringLiteral("VST3"), QStringLiteral("C:/VST3/Serum.vst3"),
                       classId(serum), QStringLiteral("Xfer"), QStringLiteral("1.3"),
                       QStringLiteral("Instrument|Synth"), true}};
        Song song;
        LiveTrack t{QStringLiteral("MidiTrack"), 1, QStringLiteral("1-Serum")};
        const QByteArray component("component state \x01\x02", 18);
        const QByteArray controller("controller", 10);
        const auto parameter = [](const QString& id, int target) {
            return element(QStringLiteral("PluginFloatParameter"),
                           value(QStringLiteral("ParameterId"), id) +
                               element(QStringLiteral("ParameterValue"),
                                       value(QStringLiteral("Manual"), 0.25) +
                                           QStringLiteral("<AutomationTarget Id=\"%1\" />").arg(target)),
                           QStringLiteral("Id=\"0\""));
        };
        // ParamID 7; 3000000000 (its high bit set: Live writes it as a signed number); a slot with none (-1).
        const QString parameters = parameter(QStringLiteral("7"), 700) + parameter(QStringLiteral("-1294967296"), 701) +
                                   parameter(QStringLiteral("-1"), 702);
        t.devices = vst2Device(QStringLiteral("Serum_x64"), 0x58667358, QByteArray("serum", 5)) +
                    vst3Device(QStringLiteral("Pro-Q 4"), proQ, component, controller, 2, parameters) +
                    vst2Device(QStringLiteral("ValhallaVintageVerb_x64"), 0x566C7676, QByteArray("VC2!vintage", 11)) +
                    vst2Device(QStringLiteral("Glitch2"), 0x476C7432, QByteArray("glitch", 6)) +
                    vst3Device(QStringLiteral("Unknown Thing"), wordsOf("NotInstalledHere"), component, {}, 2);
        t.envelopes = envelope(700, floatEvent(0, 0.25) + floatEvent(4, 0.75)) + envelope(701, floatEvent(0, 0.5)) +
                      envelope(702, floatEvent(0, 0.5));
        song.tracks = track(t);
        auto imported = load(liveSet(song), QStringLiteral("/nowhere/song.als"), options);
        const std::vector<Device>& devices = imported->project.tracks()[0].devices;
        QCOMPARE(devices.size(), size_t{4});

        // Serum (VST2) as Serum (VST3), at its defaults: no state.
        QCOMPARE(devices[0].plugin->name, QStringLiteral("Serum"));
        QCOMPARE(devices[0].plugin->uid, classId(serum));
        QVERIFY(devices[0].plugin->instrument);
        QVERIFY(!devices[0].state);
        QVERIFY(noteSays(imported->notes, QStringLiteral("Serum_x64 as Serum")));

        // Pro-Q 4 with its states, its path from the index, its automation.
        const Device& eq = devices[1];
        QCOMPARE(eq.plugin->uid, classId(proQ));
        QCOMPARE(eq.plugin->path, QStringLiteral("C:/VST3/FabFilter Pro-Q 4.vst3"));
        QCOMPARE(eq.plugin->vendor, QStringLiteral("FabFilter"));
        const QByteArray preset = QByteArray::fromBase64(eq.state->toLatin1());
        QVERIFY(preset.startsWith("VST3"));
        QCOMPARE(preset.mid(8, 32),
                 QByteArray::fromStdString(sub::vst3::presetClassId(proQ[0], proQ[1], proQ[2], proQ[3])));
        QCOMPARE(preset.mid(8, 32),
                 QByteArray("46616246696C74657250726F51342121"));  // ("FabFilterProQ4!!": its four words)
        QCOMPARE(presetChunk(preset, "Comp"), component);
        QCOMPARE(presetChunk(preset, "Cont"), controller);
        const Envelope points =
            imported->project.tracks()[0].automation.value(automation::deviceKey(eq.id, QStringLiteral("7")));
        QCOMPARE(points.size(), size_t{2});
        QCOMPARE(points[1].beat, 4.0);
        QCOMPARE(points[1].value, 0.75);
        QCOMPARE(imported->project.tracks()[0]
                     .automation.value(automation::deviceKey(eq.id, QStringLiteral("3000000000")))
                     .size(),
                 size_t{1});
        QVERIFY(
            noteSays(imported->notes, QStringLiteral("Automation left out (of what didn't come across): 1-Serum.")));

        // ValhallaVintageVerb (VST2) as the VST3 replacing it, with its settings in Steinberg's VST2 form.
        const Device& verb = devices[2];
        QCOMPARE(verb.plugin->uid, classId(vintage));
        const QByteArray state = presetChunk(QByteArray::fromBase64(verb.state->toLatin1()), "Comp");
        QVERIFY(state.startsWith(QByteArray("VstW\x00\x00\x00\x08\x00\x00\x00\x01\x00\x00\x00\x00", 16)));
        QCOMPARE(state.mid(16, 4), QByteArray("CcnK"));
        QCOMPARE(state.mid(24, 4), QByteArray("FBCh"));
        QCOMPARE(qFromBigEndian<quint32>(state.constData() + 32), 0x566C7676u);
        QVERIFY(state.endsWith("VC2!vintage"));

        // Not installed (a VST3: it stays, and loads once installed; a VST2 with no VST3: left out).
        QCOMPARE(devices[3].plugin->name, QStringLiteral("Unknown Thing"));
        QVERIFY(noteSays(imported->notes, QStringLiteral("Not installed here")));
        QVERIFY(noteSays(imported->notes, QStringLiteral("Glitch2")));
    }

    // A real VST3 plug-in's settings as Live keeps them (its processor's and
    // controller's states, its class id as four words) come back in it: the
    // .vstpreset made of them is one Steinberg's reader (the engine's) takes.
    void aPlugInsSettingsComeBackInIt() {
        const QString bundle = test::testPluginsBundle();
        if (!test::haveTestPlugins(bundle)) QSKIP("test plug-ins not built");
        const auto effect = test::testPlugin(bundle, QStringLiteral("SUB Test Effect"));
        QVERIFY(effect);
        // Its settings, as Live would have saved them: its states, from a .vstpreset of it.
        sub::Engine engine;
        const uint32_t engineTrack = engine.addTrack();
        const uint32_t plugin = engine.addPluginProcessor(engine.trackChain(engineTrack), "VST3",
                                                          effect->path.toStdString(), effect->uid.toStdString(), -1);
        engine.setProcessorParam(plugin, test::kEffectGain, 0.3f);
        const std::vector<uint8_t> saved = engine.processorState(plugin);
        const QByteArray preset(reinterpret_cast<const char*>(saved.data()), qsizetype(saved.size()));
        const QByteArray component = presetChunk(preset, "Comp");
        QVERIFY(!component.isEmpty());
        const auto words = sub::vst3::wordsFromClassId(effect->uid.toStdString());
        QVERIFY(words);
        QCOMPARE(classId(*words), effect->uid);  // (and back)
        // Its .vstpreset again, as the importer makes it: Steinberg's reader takes it.
        const QByteArray rebuilt = live::vstPreset(
            QString::fromStdString(sub::vst3::presetClassId((*words)[0], (*words)[1], (*words)[2], (*words)[3])),
            component, presetChunk(preset, "Cont"));
        const uint32_t other = engine.addPluginProcessor(engine.trackChain(engineTrack), "VST3",
                                                         effect->path.toStdString(), effect->uid.toStdString(), -1);
        engine.setProcessorState(other, std::vector<uint8_t>(rebuilt.begin(), rebuilt.end()));
        QVERIFY(std::abs(engine.processorParam(other, test::kEffectGain) - 0.3f) < 1e-6f);

        // And a set holding it: the device loads with those settings.
        SessionFixture f;
        f.bridge().setKnownPlugins(test::testPluginInfos(bundle));
        live::ImportOptions options;
        options.plugins = test::testPluginInfos(bundle);
        Song song;
        LiveTrack t{QStringLiteral("AudioTrack"), 1, QStringLiteral("1-Audio")};
        t.devices = vst3Device(QStringLiteral("SUB Test Effect"), *words, component, presetChunk(preset, "Cont"), 2);
        song.tracks = track(t);
        loadInto(f.project(), import(liveSet(song), QStringLiteral("/nowhere/song.als"), options).project);
        QTRY_VERIFY_WITH_TIMEOUT(f.bridge().pluginsPending() == 0, 5000);
        const Track& imported = f.project().tracks()[0];
        QCOMPARE(imported.devices.size(), size_t{1});
        QCOMPARE(imported.devices[0].plugin->path, bundle);
        const auto id = f.bridge().engineDeviceId(imported.id, imported.devices[0].id);
        QVERIFY(id);
        QVERIFY(std::abs(f.engine.processorParam(*id, test::kEffectGain) - 0.3f) < 1e-6f);
    }

    // Live's devices SUBstation has: Utility, EQ Eight, Compressor (and its
    // sidechain), Delay, Simpler; racks with their chains; the rest noted.
    void livesOwnDevices() {
        TempDir dir;
        const QString kick = test::writeWav(dir.path(QStringLiteral("kick.wav")), std::vector<float>(4800, 0.5f), 1);
        Song song;
        LiveTrack source{QStringLiteral("AudioTrack"), 1, QStringLiteral("1-Kick")};
        LiveTrack t{QStringLiteral("MidiTrack"), 2, QStringLiteral("2-Bass")};
        const QString utility =
            element(QStringLiteral("StereoGain"),
                    param(QStringLiteral("On"), true) + param(QStringLiteral("Gain"), 2.0) +
                        param(QStringLiteral("Balance"), 0.5) + param(QStringLiteral("StereoWidth"), 1.5) +
                        param(QStringLiteral("Mono"), false) + param(QStringLiteral("Mute"), false));
        QString bands;
        for (int b = 0; b < 8; ++b) {
            const QString a = param(QStringLiteral("IsOn"), b < 2) +
                              param(QStringLiteral("Mode"), QString::number(b == 0 ? 1 : 3)) +
                              param(QStringLiteral("Freq"), b == 0 ? 80.0 : 1000.0, b == 1 ? 801 : 0) +
                              param(QStringLiteral("Gain"), b == 1 ? -3.0 : 0.0) + param(QStringLiteral("Q"), 0.71);
            const QString s = param(QStringLiteral("IsOn"), b == 0) +
                              param(QStringLiteral("Mode"), QStringLiteral("7")) +
                              param(QStringLiteral("Freq"), 12000.0) + param(QStringLiteral("Gain"), 0.0) +
                              param(QStringLiteral("Q"), 0.71);
            bands += element(QStringLiteral("Bands.%1").arg(b),
                             element(QStringLiteral("ParameterA"), a) + element(QStringLiteral("ParameterB"), s));
        }
        const QString eq8 =
            element(QStringLiteral("Eq8"),
                    param(QStringLiteral("On"), false) + value(QStringLiteral("Mode"), QStringLiteral("2")) +
                        param(QStringLiteral("GlobalGain"), 1.5) + param(QStringLiteral("Scale"), 1.0) + bands);
        const QString compressor =
            element(QStringLiteral("Compressor2"),
                    param(QStringLiteral("On"), true) + param(QStringLiteral("Threshold"), 0.1) +
                        param(QStringLiteral("Ratio"), 1e30) + param(QStringLiteral("Attack"), 0.01) +
                        param(QStringLiteral("Release"), 50.0) + param(QStringLiteral("Gain"), 3.0) +
                        param(QStringLiteral("Knee"), 6.0) + param(QStringLiteral("DryWet"), 0.5) +
                        element(QStringLiteral("SideChain"),
                                param(QStringLiteral("OnOff"), true) +
                                    element(QStringLiteral("RoutedInput"),
                                            element(QStringLiteral("Routable"),
                                                    value(QStringLiteral("Target"),
                                                          QStringLiteral("AudioIn/Track.1/PostFxOut"))))));
        const QString delay =
            element(QStringLiteral("Delay"),
                    param(QStringLiteral("On"), true) + param(QStringLiteral("DelayLine_SyncL"), true) +
                        param(QStringLiteral("DelayLine_SyncedSixteenthL"), QStringLiteral("3")) +
                        param(QStringLiteral("DelayLine_TimeR"), 0.375) + param(QStringLiteral("Feedback"), 0.8) +
                        param(QStringLiteral("DryWet"), 0.25) +
                        param(QStringLiteral("DelayLine_SmoothingMode"), QStringLiteral("2")));
        const QString simpler = element(
            QStringLiteral("OriginalSimpler"),
            param(QStringLiteral("On"), true) +
                element(QStringLiteral("Player"),
                        element(QStringLiteral("MultiSampleMap"),
                                element(QStringLiteral("SampleParts"),
                                        element(QStringLiteral("MultiSamplePart"),
                                                value(QStringLiteral("RootKey"), QStringLiteral("48")) +
                                                    value(QStringLiteral("Detune"), 0.0) +
                                                    value(QStringLiteral("Volume"), 0.5) +
                                                    value(QStringLiteral("SampleStart"), 0.0) +
                                                    value(QStringLiteral("SampleEnd"), 2400.0) +
                                                    sampleRef(kick, QStringLiteral("kick.wav"), 4800, 48000))))) +
                element(QStringLiteral("Pitch"),
                        param(QStringLiteral("TransposeKey"), 2.0) + param(QStringLiteral("TransposeFine"), 10.0)) +
                element(QStringLiteral("VolumeAndPan"),
                        param(QStringLiteral("Volume"), -6.0, 820) +
                            element(QStringLiteral("Envelope"), param(QStringLiteral("AttackTime"), 5.0) +
                                                                    param(QStringLiteral("DecayTime"), 300.0) +
                                                                    param(QStringLiteral("SustainLevel"), 0.5) +
                                                                    param(QStringLiteral("ReleaseTime"), 60000.0))) +
                element(QStringLiteral("Globals"), value(QStringLiteral("PlaybackMode"), QStringLiteral("1"))));
        const QString rack = element(
            QStringLiteral("AudioEffectGroupDevice"),
            param(QStringLiteral("On"), true) + value(QStringLiteral("UserName"), QStringLiteral("Wide")) +
                element(
                    QStringLiteral("Branches"),
                    element(QStringLiteral("AudioEffectBranch"),
                            element(QStringLiteral("Name"),
                                    value(QStringLiteral("EffectiveName"), QStringLiteral("Left"))) +
                                element(QStringLiteral("DeviceChain"),
                                        element(QStringLiteral("AudioToAudioDeviceChain"),
                                                element(QStringLiteral("Devices"),
                                                        utility + element(QStringLiteral("Saturator"), QString())))) +
                                element(QStringLiteral("MixerDevice"), param(QStringLiteral("Volume"), 0.5) +
                                                                           param(QStringLiteral("Panorama"), -1.0) +
                                                                           param(QStringLiteral("Speaker"), true)))));
        t.devices = element(QStringLiteral("MidiArpeggiator"), QString()) + simpler + utility + eq8 + compressor +
                    delay + rack + element(QStringLiteral("Saturator"), QString());
        t.envelopes = envelope(801, floatEvent(0, 1000.0) + floatEvent(2, 2000.0)) + envelope(820, floatEvent(0, -6.0));
        song.tracks = track(source) + track(t);
        auto imported = load(liveSet(song), dir.path(QStringLiteral("song.als")));
        const Track& bass = imported->project.tracks()[1];
        QCOMPARE(bass.devices.size(), size_t{6});
        QVERIFY(noteSays(imported->notes, QStringLiteral("Left out (SUBstation has no MIDI effects): Arpeggiator.")));
        QVERIFY(noteSays(imported->notes, QStringLiteral("Saturator (2)")));

        const Device& sampler = bass.devices[0];
        QCOMPARE(sampler.kind, QStringLiteral("sampler"));
        QCOMPARE(QFileInfo(deviceState::fromModel(sampler.state).value(QStringLiteral("sample"))).canonicalFilePath(),
                 QFileInfo(kick).canonicalFilePath());
        QCOMPARE(sampler.params.value(QStringLiteral("root")), 48.0);
        QCOMPARE(sampler.params.value(QStringLiteral("mode")), 1.0);  // One-Shot
        QCOMPARE(sampler.params.value(QStringLiteral("tune")), 2.0);
        QCOMPARE(sampler.params.value(QStringLiteral("fine")), 10.0);
        QCOMPARE(sampler.params.value(QStringLiteral("end")), 50.0);
        // Its gain: the Simpler's volume and its sample's (-6 dB each); automating the
        // volume keeps the sample's on top: at -6 dB it is the gain it has.
        const double gainDb = -6.0 + 20.0 * std::log10(0.5);
        QVERIFY(std::abs(sampler.params.value(QStringLiteral("gain")) - gainDb) < 1e-9);
        const sub::ParamInfo* gainInfo = nullptr;
        for (const sub::ParamInfo& info : builtinDevice(QStringLiteral("sampler"))->info->params) {
            if (info.id == "gain") gainInfo = &info;
        }
        QVERIFY(gainInfo);
        const Envelope gainPoints = bass.automation.value(automation::deviceKey(sampler.id, QStringLiteral("gain")));
        QCOMPARE(gainPoints.size(), size_t{1});
        QVERIFY(std::abs(gainPoints[0].value - ParamSpec::fromInfo(*gainInfo, {}, {}).toNormalized(gainDb)) < 1e-9);
        QCOMPARE(sampler.params.value(QStringLiteral("sustain")), 50.0);
        QCOMPARE(sampler.params.value(QStringLiteral("release")), 10000.0);  // (its longest)

        const Device& gain = bass.devices[1];
        QCOMPARE(gain.kind, QStringLiteral("utility"));
        QVERIFY(std::abs(gain.params.value(QStringLiteral("gain")) - 20.0 * std::log10(2.0)) < 1e-9);
        QCOMPARE(gain.params.value(QStringLiteral("pan")), 0.5);
        QCOMPARE(gain.params.value(QStringLiteral("width")), 150.0);

        const Device& eq = bass.devices[2];
        QCOMPARE(eq.kind, QStringLiteral("eq"));
        QVERIFY(!eq.enabled);
        QCOMPARE(eq.params.value(QStringLiteral("output")), 1.5);
        QCOMPARE(eq.params.value(QStringLiteral("b1_type")), 2.0);   // a low cut...
        QCOMPARE(eq.params.value(QStringLiteral("b1_slope")), 1.0);  // ...of 12 dB/oct
        QCOMPARE(eq.params.value(QStringLiteral("b1_place")), 3.0);  // Mid (M/S)
        QCOMPARE(eq.params.value(QStringLiteral("b2_gain")), -3.0);
        QCOMPARE(eq.params.value(QStringLiteral("b3_on")), 0.0);
        QCOMPARE(eq.params.value(QStringLiteral("b9_type")), 4.0);   // the Side's first: a high cut...
        QCOMPARE(eq.params.value(QStringLiteral("b9_slope")), 6.0);  // ...of 48 dB/oct
        QCOMPARE(eq.params.value(QStringLiteral("b9_place")), 4.0);
        QCOMPARE(eq.params.value(QStringLiteral("b16_used")), 1.0);
        const Envelope freq = bass.automation.value(automation::deviceKey(eq.id, QStringLiteral("b2_freq")));
        QCOMPARE(freq.size(), size_t{2});
        QVERIFY(freq[0].value < freq[1].value &&
                freq[1].value < 1.0);  // (1 kHz and 2 kHz, normalized as the band's frequency)

        const Device& comp = bass.devices[3];
        QCOMPARE(comp.kind, QStringLiteral("compressor"));
        QCOMPARE(comp.params.value(QStringLiteral("threshold")), -20.0);
        QCOMPARE(comp.params.value(QStringLiteral("ratio")), 20.0);  // (Live's infinity: the most here)
        QCOMPARE(comp.params.value(QStringLiteral("attack")), 0.1);
        QCOMPARE(comp.params.value(QStringLiteral("mix")), 50.0);
        QVERIFY(comp.sidechain);
        QCOMPARE(comp.sidechain->trackId, imported->project.tracks()[0].id);
        QCOMPARE(comp.sidechain->tap, kPreFader);

        const Device& echo = bass.devices[4];
        QCOMPARE(echo.kind, QStringLiteral("delay"));
        QCOMPARE(echo.params.value(QStringLiteral("l_division")), 3.0);
        QCOMPARE(echo.params.value(QStringLiteral("r_time")), 375.0);
        QCOMPARE(echo.params.value(QStringLiteral("feedback")), 80.0);
        QCOMPARE(echo.params.value(QStringLiteral("mix")), 25.0);
        QCOMPARE(echo.params.value(QStringLiteral("mode")), 2.0);

        const Device& wide = bass.devices[5];
        QVERIFY(wide.isRack());
        QCOMPARE(wide.name, std::optional<QString>(QStringLiteral("Wide")));
        QCOMPARE(wide.chains.size(), size_t{1});
        QCOMPARE(wide.chains[0].name, QStringLiteral("Left"));
        QVERIFY(std::abs(wide.chains[0].volumeDb - 20.0 * std::log10(0.5)) < 1e-9);
        QCOMPARE(wide.chains[0].pan, -1.0);
        QCOMPARE(wide.chains[0].devices.size(), size_t{1});
        QCOMPARE(wide.chains[0].devices[0].kind, QStringLiteral("utility"));
    }

    // A Drum Rack: a group (its mixer, the effects after the rack) with a MIDI
    // track for each pad the clips play, its notes as the pad sends them on; a
    // sidechain from a pad comes from its track.
    void drumRacks() {
        TempDir dir;
        const QString kick = test::writeWav(dir.path(QStringLiteral("kick.wav")), std::vector<float>(4800, 0.5f), 1);
        const QString snare = test::writeWav(dir.path(QStringLiteral("snare.wav")), std::vector<float>(4800, 0.25f), 1);
        const auto pad = [&](const QString& name, int note, const QString& sample, double volume,
                             const QString& more = {}) {
            const QString simpler =
                element(QStringLiteral("OriginalSimpler"),
                        param(QStringLiteral("On"), true) +
                            element(QStringLiteral("Player"),
                                    element(QStringLiteral("MultiSampleMap"),
                                            element(QStringLiteral("SampleParts"),
                                                    element(QStringLiteral("MultiSamplePart"),
                                                            value(QStringLiteral("RootKey"), QStringLiteral("60")) +
                                                                sampleRef(sample, QString(), 4800, 48000))))));
            return element(
                QStringLiteral("DrumBranch"),
                element(QStringLiteral("Name"), value(QStringLiteral("EffectiveName"), name)) +
                    element(QStringLiteral("DeviceChain"),
                            element(QStringLiteral("MidiToAudioDeviceChain"),
                                    element(QStringLiteral("Devices"), simpler + more))) +
                    element(QStringLiteral("BranchInfo"),
                            value(QStringLiteral("ReceivingNote"), QString::number(128 - note)) +
                                value(QStringLiteral("SendingNote"), QStringLiteral("60"))) +
                    element(QStringLiteral("MixerDevice"), param(QStringLiteral("Volume"), volume, 950 + note) +
                                                               param(QStringLiteral("Panorama"), 0.0) +
                                                               param(QStringLiteral("Speaker"), true)));
        };
        Song song;
        LiveTrack drums{QStringLiteral("MidiTrack"), 5, QStringLiteral("5-Drum Rack")};
        drums.color = 9;
        drums.mix.volume = 0.5;
        drums.devices =
            element(
                QStringLiteral("DrumGroupDevice"),
                param(QStringLiteral("On"), true) +
                    element(QStringLiteral("Branches"),
                            pad(QStringLiteral("Kick"), 36, kick, 1.0) + pad(QStringLiteral("Snare"), 38, snare, 0.5) +
                                pad(QStringLiteral("Unplayed"), 40, kick, 1.0) +
                                pad(QStringLiteral("Outside"), 41, kick, 1.0,
                                    vst2Device(QStringLiteral("Glitch2"), 0x476C7432, QByteArray("glitch", 6))))) +
            element(QStringLiteral("StereoGain"),
                    param(QStringLiteral("On"), true) + param(QStringLiteral("Gain"), 1.0));
        // (A clip envelope, noted once for the clip, not once a pad; pad 41 plays only past the clip's end.)
        drums.clips =
            midiClip(0.0, 4.0,
                     keyTrack(36, note(0.0, 0.25) + note(2.0, 0.25)) + keyTrack(38, note(1.0, 0.25, 80)) +
                         keyTrack(41, note(6.0, 0.25)))
                .replace(
                    QStringLiteral("<Notes>"),
                    QStringLiteral("<Envelopes><Envelopes><ClipEnvelope Id=\"0\" /></Envelopes></Envelopes><Notes>"));
        drums.envelopes =
            envelope(950 + 38, floatEvent(0, 1.0) + floatEvent(4, 0.5)) + envelope(950 + 41, floatEvent(0, 0.5));
        LiveTrack bass{QStringLiteral("AudioTrack"), 6, QStringLiteral("6-Bass")};
        bass.devices = element(
            QStringLiteral("Compressor2"),
            param(QStringLiteral("On"), true) +
                element(QStringLiteral("SideChain"),
                        param(QStringLiteral("OnOff"), true) +
                            element(QStringLiteral("RoutedInput"),
                                    element(QStringLiteral("Routable"),
                                            value(QStringLiteral("Target"),
                                                  QStringLiteral("AudioIn/Track.5/DeviceOut.0.B0,PreFxOut"))))));
        song.tracks = track(drums) + track(bass);
        auto imported = load(liveSet(song), dir.path(QStringLiteral("song.als")));
        const Project& p = imported->project;
        QCOMPARE(p.tracks().size(), size_t{4});
        const Track& group = p.tracks()[0];
        QVERIFY(group.isGroup());
        QCOMPARE(group.name, QStringLiteral("1 Drum Rack"));
        QVERIFY(std::abs(group.volumeDb - 20.0 * std::log10(0.5)) < 1e-9);
        QCOMPARE(group.devices.size(), size_t{1});
        QCOMPARE(group.devices[0].kind, QStringLiteral("utility"));
        const Track& kickTrack = p.tracks()[1];
        const Track& snareTrack = p.tracks()[2];
        QCOMPARE(kickTrack.name, QStringLiteral("Kick"));
        QCOMPARE(snareTrack.name, QStringLiteral("Snare"));
        for (const Track* t : {&kickTrack, &snareTrack}) {
            QCOMPARE(t->parent, std::optional<QString>(group.id));
            QVERIFY(t->isMidi());
            QCOMPARE(t->devices.size(), size_t{1});
            QCOMPARE(t->devices[0].kind, QStringLiteral("sampler"));
            QCOMPARE(t->clips.size(), size_t{1});
        }
        QCOMPARE(kickTrack.clips[0].notes, (std::vector<Note>{{60, 0.0, 0.25, 100}, {60, 2.0, 0.25, 100}}));
        QCOMPARE(snareTrack.clips[0].notes, (std::vector<Note>{{60, 1.0, 0.25, 80}}));
        QCOMPARE(QFileInfo(deviceState::fromModel(snareTrack.devices[0].state).value(QStringLiteral("sample")))
                     .canonicalFilePath(),
                 QFileInfo(snare).canonicalFilePath());
        QVERIFY(std::abs(snareTrack.volumeDb - 20.0 * std::log10(0.5)) < 1e-9);
        QCOMPARE(snareTrack.automation.value(automation::kMixerVolume).size(),
                 size_t{2});  // (its pad's volume, automated)
        QVERIFY(!named(p, QStringLiteral("Unplayed")) && !named(p, QStringLiteral("Outside")));
        QVERIFY(noteSays(imported->notes, QStringLiteral("Left out (clip envelopes): 5-Drum Rack.")));
        QVERIFY(
            !noteSays(imported->notes, QStringLiteral("Glitch2")));  // (the pad made no track: nothing of it is said)
        QVERIFY(noteSays(imported->notes,
                         QStringLiteral("Automation left out (of what didn't come across): 5-Drum Rack.")));
        const Device& ducking = p.tracks()[3].devices[0];
        QVERIFY(ducking.sidechain);
        QCOMPARE(ducking.sidechain->trackId, kickTrack.id);
    }

    // Mixer automation: volume and pan as the faders map them, the activator as a switch.
    void mixerAutomation() {
        Song song;
        LiveTrack t{QStringLiteral("AudioTrack"), 1, QStringLiteral("1-Audio")};
        t.mix.volumeTarget = 77;
        t.envelopes = envelope(77, floatEvent(-63072000, 1.0) + floatEvent(4, 0.5)) + envelope(12345, floatEvent(0, 1));
        song.tracks = track(t);
        song.masterEnvelopes = envelope(5, floatEvent(0, 0.25));
        auto imported = load(liveSet(song));
        const Envelope volume = imported->project.tracks()[0].automation.value(automation::kMixerVolume);
        QCOMPARE(volume.size(), size_t{2});
        QCOMPARE(volume[0].beat, 0.0);
        QVERIFY(std::abs(volume[0].value - automation::volumeToNormalized(0.0)) < 1e-9);
        QCOMPARE(volume[1].beat, 4.0);
        QVERIFY(std::abs(volume[1].value - automation::volumeToNormalized(20.0 * std::log10(0.5))) < 1e-9);
        const Envelope master = imported->project.master().automation.value(automation::kMixerVolume);
        QCOMPARE(master.size(), size_t{1});
        QVERIFY(
            noteSays(imported->notes, QStringLiteral("Automation left out (of what didn't come across): 1-Audio.")));
    }

    // File › Import Ableton Live Set…: the set as a new project, named after it
    // (its title, where Save As starts), the notes said; one that can't be read
    // is a warning, and the project stays.
    void theSessionImportsASet() {
        SessionFixture f;
        Session& s = f.s();
        TempDir dir;
        Song song;
        song.tempo = 172.0;
        LiveTrack keys{QStringLiteral("MidiTrack"), 1, QStringLiteral("1-Keys")};
        keys.clips = midiClip(0.0, 4.0, keyTrack(60, note(0.0, 1.0)));
        keys.devices = element(QStringLiteral("Saturator"), QString());
        song.tracks = track(keys);
        const QString path = writeSet(dir, QStringLiteral("My Song.als"), liveSet(song));
        f.editor().addAudioTrack();
        QVERIFY(s.importLiveSet(path));
        QCOMPARE(f.project().tracks().size(), size_t{1});
        QCOMPARE(f.project().tempo(), 172.0);
        QCOMPARE(f.engine.tempo(), 172.0);
        QCOMPARE(f.project().path(), QString());
        QVERIFY(!s.clean() && f.stack().count() == 0);  // (unsaved: New, Open and Quit ask first)
        QCOMPARE(s.title(), QStringLiteral("My Song* - SUBstation"));
        QCOMPARE(QFileInfo(s.suggestedSavePath()).fileName(), QStringLiteral("My Song.gilproj"));
        QCOMPARE(QFileInfo(s.suggestedExportPath(QStringLiteral("mp3"))).fileName(), QStringLiteral("My Song.mp3"));
        QCOMPARE(f.lastMessage(), QStringLiteral("Imported My Song.als: 1 track, 1 clip"));
        QCOMPARE(f.informations.size(), 1);
        QVERIFY(f.informations.back().contains(QStringLiteral("Saturator")));
        QVERIFY(s.saveProjectAs(dir.path(QStringLiteral("My Song.gilproj"))));
        QVERIFY(s.clean());
        QCOMPARE(s.title(), QStringLiteral("My Song - SUBstation"));
        s.newProject();
        QCOMPARE(s.title(), QStringLiteral("Untitled - SUBstation"));

        f.editor().addAudioTrack();
        QFile broken(dir.path(QStringLiteral("broken.als")));
        QVERIFY(broken.open(QIODevice::WriteOnly));
        broken.write("\x1f\x8b\x08\x00 not really", 15);
        broken.close();
        QVERIFY(!s.importLiveSet(broken.fileName()));
        QCOMPARE(f.warnings.size(), 1);
        QVERIFY(f.warnings.back().startsWith(QStringLiteral("broken.als is damaged")));
        QCOMPARE(f.project().tracks().size(), size_t{1});  // (as it was)
        QCOMPARE(s.liveSetFilter(), QStringLiteral("Ableton Live Set (*.als)"));
    }
};

QTEST_GUILESS_MAIN(TestLiveImport)
#include "test_live_import.moc"
