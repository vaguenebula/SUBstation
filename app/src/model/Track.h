#pragma once
// Tracks, and what a track has besides its clips and devices: its sends, its
// inputs, its frozen audio.
//
// Audio tracks hold audio clips; MIDI tracks hold MIDI clips and an instrument;
// group tracks hold other tracks (each track's `parent` is the group it is in).
// The master is a Track too (kind "master", id kMaster), with devices, a mixer
// and automation, but no clips; return tracks (kind "return") are fed by sends
// and have no clips either. See Project.h.

#include "model/Automation.h"
#include "model/Clip.h"
#include "model/Device.h"
#include "model/OrderedMap.h"

#include <QMap>
#include <QString>
#include <QStringList>

#include <optional>
#include <variant>
#include <vector>

namespace sub::app {

// Ableton-like clip/track colours.
inline const QStringList kTrackColors{
    QStringLiteral("#ff94a6"), QStringLiteral("#ffa529"), QStringLiteral("#cc9927"), QStringLiteral("#f7f47c"),
    QStringLiteral("#bffb00"), QStringLiteral("#1aff2f"), QStringLiteral("#25ffa8"), QStringLiteral("#5cffe8"),
    QStringLiteral("#8bc5ff"), QStringLiteral("#5480e4"), QStringLiteral("#92a7ff"), QStringLiteral("#d86ce4"),
    QStringLiteral("#e553a0"), QStringLiteral("#ffb3a0")};

inline const QString kAudioKind = QStringLiteral("audio");
inline const QString kMidiKind = QStringLiteral("midi");
inline const QString kGroupKind = QStringLiteral("group");
// The arrangement's kinds of track.
inline const QStringList kTrackKinds{kAudioKind, kMidiKind, kGroupKind};
inline const QString kMasterKind = QStringLiteral("master");  // the master's kind: no clips, effects only
inline const QString kReturnKind = QStringLiteral("return");  // a return track's: fed by sends, no clips
// Input monitoring: when a track hears its input instead of its clips. "auto":
// while armed, unless it plays back without recording (as in Ableton).
inline const QStringList kMonitorModes{QStringLiteral("off"), QStringLiteral("in"), QStringLiteral("auto")};
inline const QString kMasterColor = QStringLiteral("#a0a0a0");

inline constexpr int kDefaultTrackHeight = 80;
inline constexpr int kMinTrackHeight = 24;
inline constexpr int kMaxTrackHeight = 400;

// A frozen track's audio: its signal before its fader (after its devices),
// from the timeline's start, in the WAV file `path`, `durationSec` long,
// rendered at `tempo` (it plays warped to others, its length in beats fixed).
struct Freeze {
    QString path;
    double durationSec = 0.0;
    double tempo = 120.0;

    // The clip that plays it, from the timeline's start (warped from `tempo`: at
    // that tempo it plays its samples as they are).
    Clip clip(const QString& trackId, const QString& name) const;

    friend bool operator==(const Freeze&, const Freeze&) = default;
};

// Which MIDI input a MIDI track hears and records: every input ("") or one by
// name, on every channel (0) or one (1-16).
struct MidiInput {
    QString device;
    int channel = 0;

    bool allDevices() const { return device.isEmpty(); }

    friend bool operator==(const MidiInput&, const MidiInput&) = default;
};

// A track's send to a return track: its level (dB; kMinVolumeDb or below is
// silent) and where it taps the track's signal: after its fader (and pan), or
// before it. A muted track sends nothing either way.
struct Send {
    double levelDb = automation::kMinVolumeDb;
    bool preFader = false;

    friend bool operator==(const Send&, const Send&) = default;
};

// Return id -> its send. Replaced whole, never changed in place.
using SendMap = QMap<QString, Send>;
// Target key -> envelope (never an empty one), in the order they were first set.
using EnvelopeMap = OrderedMap<QString, Envelope>;

// The settings of a track that Project::updateTrack changes (its id, kind,
// clips, devices, automation, view, group and frozen audio change otherwise).
enum class TrackField {
    Name,        // QString
    Color,       // QString
    VolumeDb,    // double
    Pan,         // double
    Mute,        // bool
    Solo,        // bool
    Height,      // int
    Input,       // std::vector<int>
    InputTrack,  // std::optional<QString>
    MidiInput,   // std::optional<MidiInput>
    Monitor,     // QString
    Armed,       // bool
    Folded,      // bool
    Sends,       // SendMap
};
using TrackValue =
    std::variant<bool, int, double, QString, std::vector<int>, std::optional<QString>, std::optional<MidiInput>, SendMap>;
// Several settings of one track.
using TrackValues = QMap<TrackField, TrackValue>;

// The field's name as the project file has it ("volume_db"), and back.
QString trackFieldName(TrackField field);
std::optional<TrackField> trackFieldFromName(const QString& name);

struct Track {
    QString id;
    QString name;
    QString color;
    double volumeDb = 0.0;
    double pan = 0.0;
    bool mute = false;
    bool solo = false;
    int height = kDefaultTrackHeight;
    std::vector<Clip> clips;  // sorted by startBeat; MIDI clips on MIDI tracks
    std::vector<Device> devices;
    // One of kTrackKinds (or kMasterKind: the master; kReturnKind: a return);
    // fixed (flattening replaces the track).
    QString kind = kAudioKind;
    EnvelopeMap automation;  // target key -> envelope (never empty)
    AutomationView automationView;
    // Audio input: device channels (0-based): none, {c} mono, {l, r} a stereo pair.
    std::vector<int> input;
    // An audio track's input from another track's output instead (its id;
    // kMaster: the master's, resampling); `input` is empty then. None: the
    // device's channels.
    std::optional<QString> inputTrack;
    // MIDI input (MIDI tracks); none: none. New MIDI tracks hear every input, as in Ableton.
    std::optional<MidiInput> midiInput = MidiInput{};
    QString monitor = QStringLiteral("auto");  // one of kMonitorModes
    bool armed = false;  // records when recording starts (saved, not undone)
    std::optional<QString> parent;  // the group it is in (none: none); see treeProblem
    // A thin row, automation hidden; a group: its tracks hidden (saved, not undone).
    bool folded = false;
    SendMap sends;  // return id -> its send (replaced whole, never changed)
    std::optional<Freeze> frozen;  // its frozen audio (none: not frozen); see Freeze

    bool isMidi() const { return kind == kMidiKind; }
    bool isAudio() const { return kind == kAudioKind; }
    bool isGroup() const { return kind == kGroupKind; }
    bool isMaster() const { return kind == kMasterKind; }
    bool isReturn() const { return kind == kReturnKind; }
    // Whether it is a track of the arrangement that plays clips (audio or MIDI).
    bool hasClips() const { return isAudio() || isMidi(); }
    // Whether it has something to record: an audio input (or another track's
    // output), or a MIDI track's MIDI input.
    bool hasInput() const;

    // A setting, and setting it (a number of the other kind is taken as it would
    // be: an int for a double, a whole double for an int).
    TrackValue value(TrackField field) const;
    void setValue(TrackField field, const TrackValue& value);

    friend bool operator==(const Track&, const Track&) = default;
};

// The master, with these settings (its id, name, colour and kind are fixed).
Track newMaster();

// A, B, ... Z, AA, AB...: what a return (by its place among the returns) is called.
QString returnLetter(int index);

}  // namespace sub::app
