#pragma once
// The project model: the single source of truth for the UI, undo and saving.
//
// The audio engine mirrors this model (the engine bridge). Mutating methods
// here are called only by undo commands (Commands.h), which keeps every edit
// undoable and every change signalled. (View state, like track heights and
// which automation shows, changes directly: it is saved but not undone.)
//
// Automation belongs to an owner: a track (its id) or the master (kMaster); see
// Automation.h.
//
// The master is a Track too (kind "master", id kMaster), with devices, a mixer
// and automation, but no clips. It is master(), not one of tracks() (the
// arrangement's); track(kMaster) finds it, so whatever works on a track's
// devices, mixer or automation works on the master's.
//
// Group tracks (kind "group") hold other tracks: what is in a group goes into
// it, through its devices and mixer, and on to the master (or the group it is
// in). A group has no clips. The hierarchy lives here, as each track's `parent`
// (the group it is in, or none); tracks() stays flat, with the invariant that a
// group's descendants follow it, together (treeProblem). The engine sees only
// where each track's output goes.
//
// Any track can be folded (view state): a folded track shows as a thin row, its
// automation hidden; a folded group keeps its row but hides its tracks (and its
// automation). So can any device (foldedDevices(), by id: view state too, so
// undoing a change to the devices doesn't unfold them): the device view shows it
// as a narrow strip with its name, and a folded rack hides its chains. A rack's
// chain list shows only when asked for (shownChainLists()), and the devices of
// the chain it shows, beside it, unless hidden (hiddenRackDevices()): view state
// as well.
//
// Return tracks (kind "return") are fed by sends: any track, group or return can
// send its signal to a return, at a level, after its fader or before it
// (Track::sends, by return id). A return has no clips; it goes to the master,
// through its devices and mixer, and may send on into another return, but never
// back into one that feeds it (wouldCycle). Returns are returns(), apart from
// the arrangement's tracks (and the master); track() finds them too, as it does
// the master, so whatever works on a track's devices, mixer or automation works
// on a return's. They are named by letter, in order (A, B...).
//
// An audio track's input is some of the audio device's channels (Track::input),
// or another track's output, after its fader (Track::inputTrack: a track, a
// group or a return), or the master's (kMaster: resampling). Taking a track's
// output is an edge of the routing graph, like a send: a track can't take the
// output of one it feeds (routingGraph, feeds). The master is no part of that
// graph: everything reaches it, and a track recording it never plays it back
// into it (it can't monitor it).
//
// A device with a sidechain (aux) input can hear a track's (a group's, a
// return's) signal there (Device::sidechain): after its fader, before it, or
// after one of its devices. That is an edge of the routing graph too, from the
// source to the track the device is on (sidechainWouldCycle); the master's
// devices can take any track's. Should the source go, the sidechain goes (in the
// same undo step); should the device it taps after leave the source, it taps
// before the fader until the device comes back.
//
// Racks: see Device.h.
//
// A track (a group, a return) can be frozen (Track::frozen: a Freeze): its
// signal before its fader, after its devices (a group's: its bus, with what is
// in it), is rendered from the timeline's start into a file, and the track plays
// that instead: its devices are unloaded (their state kept for unfreezing), its
// clips, notes and device automation are baked in, its mixer, sends and output
// stay live. What goes into a frozen group or return is in its frozen audio, so
// nothing in a frozen group can be changed (isFrozen) until it is unfrozen. A
// tempo change plays the frozen audio warped. A track whose signal another
// track's sidechain takes after one of its devices (or before them) can't be
// frozen: that tap isn't in the frozen audio (freezeProblem). Flattening a
// frozen audio or MIDI track makes it an audio track playing its frozen audio as
// a clip, without its devices (one undo step).
//
// Queries hand out references into the project: they stay valid until the
// project changes (a track inserted or removed moves the others). Asking for a
// track, device, chain or clip that isn't there throws std::out_of_range (as a
// missing key would); the find* queries return null instead.

#include "model/Automation.h"
#include "model/Clip.h"
#include "model/Device.h"
#include "model/Keys.h"
#include "model/OrderedMap.h"
#include "model/Routing.h"
#include "model/Timebase.h"
#include "model/Track.h"

#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace sub::app {

// A parameter of one of a track's devices: (device id, param id).
using DeviceParam = std::pair<QString, QString>;
// An automation lane: (owner, target key).
using LaneRef = std::pair<QString, QString>;

// A rack chain's settings Project::updateChain changes.
enum class ChainField {
    Name,      // QString
    VolumeDb,  // double
    Pan,       // double
    Mute,      // bool
    Solo,      // bool
};
using ChainValue = std::variant<bool, double, QString>;
QString chainFieldName(ChainField field);  // "volume_db"
std::optional<ChainField> chainFieldFromName(const QString& name);
ChainValue chainValue(const Chain& chain, ChainField field);

// The project's settings Project::updateSettings changes.
enum class SettingsField {
    Tempo,             // double
    TimeSignature,     // TimeSignature
    Key,               // std::optional<Key>
    LoopEnabled,       // bool
    LoopStart,         // double
    LoopEnd,           // double
    AutomationLocked,  // bool
};
using SettingsValue = std::variant<bool, double, TimeSignature, std::optional<Key>>;
using SettingsValues = QMap<SettingsField, SettingsValue>;
QString settingsFieldName(SettingsField field);  // "loop_start"

// Everything a project holds, replaced at once (a project opened).
struct ProjectContents {
    double tempo = 120.0;
    TimeSignature timeSignature;
    std::optional<Key> key;
    bool loopEnabled = false;
    double loopStart = 0.0;
    double loopEnd = 16.0;
    bool automationLocked = false;
    std::optional<Track> master;  // none: a new master
    std::vector<Track> tracks;
    std::vector<Track> returns;
    QSet<QString> foldedDevices;
    QSet<QString> shownChainLists;
    QSet<QString> hiddenRackDevices;
    QString path;  // the file it was opened from ("": none)
};

class Project : public QObject {
    Q_OBJECT
    Q_PROPERTY(double tempo READ tempo NOTIFY settingsChanged)
    Q_PROPERTY(bool loopEnabled READ loopEnabled NOTIFY settingsChanged)
    Q_PROPERTY(double loopStart READ loopStart NOTIFY settingsChanged)
    Q_PROPERTY(double loopEnd READ loopEnd NOTIFY settingsChanged)
    Q_PROPERTY(bool automationLocked READ automationLocked NOTIFY settingsChanged)
    Q_PROPERTY(QString timeSignatureText READ timeSignatureText NOTIFY settingsChanged)
    Q_PROPERTY(QString keyName READ keyName NOTIFY settingsChanged)
    Q_PROPERTY(QString path READ path NOTIFY pathChanged)

public:
    explicit Project(QObject* parent = nullptr);

    // --- Settings ---
    double tempo() const { return tempo_; }
    const TimeSignature& timeSignature() const { return timeSignature_; }
    // The project's key: audio added with a key in its file name is transposed to it.
    const std::optional<Key>& key() const { return key_; }
    bool loopEnabled() const { return loopEnabled_; }
    double loopStart() const { return loopStart_; }
    double loopEnd() const { return loopEnd_; }
    // Locked: automation stays where it is when clips move. Unlocked, the
    // automation under clips moves (or is copied) with them, as in Ableton.
    bool automationLocked() const { return automationLocked_; }
    QString timeSignatureText() const { return timeSignature_.toString(); }
    QString keyName() const { return key_ ? key_->name() : QString(); }
    SettingsValue setting(SettingsField field) const;
    // The file it was saved to or opened from ("": none yet).
    QString path() const { return path_; }

    // --- Queries ---
    const Track& master() const { return master_; }
    const std::vector<Track>& tracks() const { return tracks_; }
    // Return tracks, in order (A, B, ...).
    const std::vector<Track>& returns() const { return returns_; }
    // A track of the arrangement, a return track, or the master (kMaster).
    const Track& track(const QString& trackId) const;
    const Track* findTrack(const QString& trackId) const;
    int trackIndex(const QString& trackId) const;
    // Whether this is a track of the arrangement (not the master: see hasOwner).
    bool hasTrack(const QString& trackId) const;
    const Clip& clip(const QString& trackId, const QString& clipId) const;
    const Clip* findClip(const QString& trackId, const QString& clipId) const;
    // End of the last clip (0 for an empty arrangement).
    double endBeat() const;
    // Whether this is a track, a return or the master: something with devices, a mixer and automation.
    bool hasOwner(const QString& owner) const;
    // The arrangement's tracks, the returns, then the master.
    std::vector<const Track*> allTracks() const;
    // An owner's envelopes by target key (read only: change them through commands).
    const EnvelopeMap& automation(const QString& owner) const;
    // An owner's envelope for a target (empty: none).
    Envelope envelope(const QString& owner, const QString& key) const;
    const AutomationView& automationView(const QString& owner) const;
    // Everything that has automation: the tracks, the returns, then the master.
    QStringList owners() const;

    // --- Returns and sends ---
    bool hasReturn(const QString& trackId) const;
    int returnIndex(const QString& trackId) const;
    QString returnLetter(const QString& trackId) const;
    // Everything that can send: the tracks (groups too), then the returns.
    std::vector<const Track*> senders() const;
    // Whether a send from a track into a return would close a cycle (see sub::app::wouldCycle).
    bool wouldCycle(const QString& trackId, const QString& returnId) const;

    // --- Inputs ---
    // Whether a track taking its input from another's output (or a return's)
    // would close a cycle (see sub::app::inputWouldCycle).
    bool inputWouldCycle(const QString& trackId, const QString& sourceId) const;
    // Whether a device on a track (or the master) taking another's signal as its
    // sidechain would close a cycle (see sub::app::sidechainWouldCycle).
    bool sidechainWouldCycle(const QString& trackId, const QString& sourceId) const;
    // The tracks (groups too) and returns a device on a track (or the master)
    // could take as its sidechain, but its own, in order (some would close a
    // cycle: sidechainWouldCycle).
    std::vector<const Track*> sidechainSources(const QString& trackId) const;
    // The MIDI tracks whose notes a device on a track (or a return, or the
    // master) could take instead of its own track's, but that track itself, in
    // order. (A MIDI input carries no audio: none closes a cycle.)
    std::vector<const Track*> midiSources(const QString& trackId) const;
    // The tracks (groups too) and returns whose output a track could take as its
    // input, but itself, in order (some would close a cycle: inputWouldCycle).
    // The master's (resampling) can be taken too.
    std::vector<const Track*> inputSources(const QString& trackId) const;
    // What an input from a track's output is called: the track's name, or "Resampling" (the master's).
    QString inputName(const QString& sourceId) const;
    // The returns a track (or return) can send to: all but those that would close a cycle.
    std::vector<const Track*> sendTargets(const QString& trackId) const;

    // --- Outputs ---
    // Whether a track's (or a return's) output going there would close a cycle
    // (see sub::app::outputWouldCycle).
    bool outputWouldCycle(const QString& trackId, const Output& output) const;
    // The track (or return) a track's output goes into (see sub::app::outputTarget):
    // its group, a track, or the one a device whose sidechain it goes into is on.
    // None: the master, or nowhere.
    std::optional<QString> outputTarget(const QString& trackId) const;

    // --- Groups ---
    // The index just past the last descendant of the track at `index` (index + 1 if it has none).
    int subtreeEnd(int index) const;
    // What is in a group (the tracks in groups in it too), in order.
    std::vector<const Track*> descendants(const QString& trackId) const;
    // These tracks and what is in those that are groups, in the arrangement's order.
    QStringList withContents(const QStringList& trackIds) const;
    std::vector<const Track*> children(const QString& trackId) const;
    // The groups a track is in, the nearest first.
    QStringList ancestors(const QString& trackId) const;
    bool isDescendant(const QString& trackId, const QString& groupId) const;
    int depth(const QString& trackId) const;
    // Whether a group it is in is folded (the arrangement doesn't show it).
    bool isHidden(const QString& trackId) const;
    // The group a track inserted at `index` goes into: that of the track it goes
    // before (amid a group's tracks it has to be in that group).
    std::optional<QString> parentAt(int index) const;
    TrackTree tree() const;
    QString nextColor() const;
    QString uniqueTrackName(const QString& base) const;

    // --- Freezing ---
    // The frozen track that holds a track's audio: itself, or the outermost
    // frozen group it is in; none: neither (it plays live).
    std::optional<QString> frozenBy(const QString& trackId) const;
    // Whether a track is frozen, or in a frozen group: its clips, devices and
    // device automation can't change.
    bool isFrozen(const QString& trackId) const;
    // Why a track can't be frozen (none: it can).
    std::optional<QString> freezeProblem(const QString& trackId) const;
    // Why a track can't be flattened (none: it can): only frozen audio and MIDI tracks can.
    std::optional<QString> flattenProblem(const QString& trackId) const;

    // --- Devices ---
    // A device of a track (in a rack too).
    const Device& device(const QString& trackId, const QString& deviceId) const;
    const Device* findDevice(const QString& trackId, const QString& deviceId) const;
    bool hasDevice(const QString& trackId, const QString& deviceId) const;
    // The track (return, or kMaster) a device is on, in a rack or not; none: none.
    std::optional<QString> deviceOwner(const QString& deviceId) const;
    // A rack chain on a track, and its rack.
    const Chain& chain(const QString& trackId, const QString& chainId) const;
    const Device& chainRack(const QString& trackId, const QString& chainId) const;
    // Ids of the devices shown folded (view state).
    const QSet<QString>& foldedDevices() const { return foldedDevices_; }
    bool isDeviceFolded(const QString& deviceId) const { return foldedDevices_.contains(deviceId); }
    // Ids of the racks whose chain list shows (view state: hidden by default).
    const QSet<QString>& shownChainLists() const { return shownChainLists_; }
    bool isChainListShown(const QString& rackId) const { return shownChainLists_.contains(rackId); }
    // Ids of the racks that don't show their chain's devices beside them (view state: shown by default).
    const QSet<QString>& hiddenRackDevices() const { return hiddenRackDevices_; }
    bool areRackDevicesShown(const QString& rackId) const { return !hiddenRackDevices_.contains(rackId); }

    // --- Mutations (call through undo commands) ---
    void insertTrack(Track track, int index);
    std::pair<Track, int> removeTrack(const QString& trackId);
    void insertReturn(Track track, int index);
    // Takes a return away (the sends into it go first: see the editor's deleteTracks).
    std::pair<Track, int> removeReturn(const QString& trackId);
    // Put the tracks in this order and these groups (`tree`: every track's id and
    // parent). Throws EditError, changing nothing, if that isn't a valid tree.
    void arrangeTracks(const TrackTree& tree);
    // A track's settings (the master's mixer too). Emits trackChanged once.
    void updateTrack(const QString& trackId, TrackField field, const TrackValue& value);
    void updateTrack(const QString& trackId, const TrackValues& values);
    // Puts `track` where the track of its id is (a track flattened: of another
    // kind), as if that one went and this one came; returns the one it replaces.
    Track replaceTrack(Track track);
    void setFrozen(const QString& trackId, const std::optional<Freeze>& freeze);
    // What of a frozen track's audio plays (Freeze::segments; sorted here by
    // start). Emits clipsChanged: what the track plays changed, not whether it
    // is frozen. Nothing for a track that isn't frozen.
    void setFrozenSegments(const QString& trackId, std::optional<std::vector<Clip>> segments);
    // A track's name is its nameTemplate numbered by its place (TrackNames.h):
    // a track coming, going or moving renumbers the tracks after it before
    // anyone hears of it (trackInserted, trackRemoved, tracksArranged), then
    // trackChanged for each one renamed. A track's clips (sorted here by start).
    // These, setDevices, setChains and setDeviceName rename a track named by
    // what it holds after it: trackChanged, after clipsChanged or devicesChanged.
    void setClips(const QString& trackId, std::vector<Clip> clips);
    void setDevices(const QString& trackId, std::vector<Device> devices);
    // Change several tracks' devices at once (a device moving between them): all
    // of them change before anyone hears of it (devicesChanged, in this order).
    void setChains(const OrderedMap<QString, std::vector<Device>>& chains);
    // A rack chain's name or mixer (volume, pan, mute, solo).
    void updateChain(const QString& trackId, const QString& chainId, ChainField field, const ChainValue& value);
    // A rack's macro mappings; a rack's name (none: named by its kind).
    void setDeviceMacros(const QString& trackId, const QString& deviceId, const std::vector<MacroMapping>& macros);
    void setDeviceName(const QString& trackId, const QString& deviceId, const std::optional<QString>& name);
    void setDeviceParam(const QString& trackId, const QString& deviceId, const QString& paramId, double value);
    // Several parameters at once ((device id, param id) -> value): a macro and what it moves.
    void setDeviceParams(const QString& trackId, const QMap<DeviceParam, double>& values);
    void setDeviceEnabled(const QString& trackId, const QString& deviceId, bool enabled);
    void setDeviceSidechain(const QString& trackId, const QString& deviceId, const std::optional<Sidechain>& sidechain);
    void setDeviceMidiFrom(const QString& trackId, const QString& deviceId, const QString& sourceId);
    void setDeviceState(const QString& trackId, const QString& deviceId, const std::optional<QString>& state);
    // A plug-in device's state as its plug-in has it now (base64), and where the
    // plug-in was found, kept in the model without a signal or an undo step: it
    // isn't an edit (the engine has that state already), only what saving or
    // copying the device needs (EngineBridge::storePluginStates).
    void storePluginState(const QString& trackId, const QString& deviceId, const QString& state,
                          const QString& pluginPath);
    // Fold or unfold devices of a track (view state: not undone).
    void setDevicesFolded(const QString& trackId, const QSet<QString>& deviceIds, bool folded);
    // Devices shown folded from now on, without a signal: devices about to be
    // added (pasted copies of folded ones), whose devicesChanged follows.
    void addFoldedDevices(const QSet<QString>& deviceIds);
    // Show or hide a rack's chain list, or its chain's devices beside it (view state: not undone).
    void setChainListShown(const QString& trackId, const QString& rackId, bool shown);
    void setRackDevicesShown(const QString& trackId, const QString& rackId, bool shown);
    void updateSettings(const SettingsValues& values);
    // Replace an envelope; an empty one removes the target's automation.
    void setEnvelope(const QString& owner, const QString& key, const Envelope& points);
    void setAutomationView(const QString& owner, const AutomationView& view);
    // Everything replaced (new, open): emits reset, then settingsChanged.
    void replaceContents(ProjectContents contents);
    void clear();
    // The file it was saved to (saving sets it).
    void setPath(const QString& path);

Q_SIGNALS:
    void trackInserted(const QString& trackId, int index);
    void trackRemoved(const QString& trackId, int index);
    void returnInserted(const QString& trackId, int index);  // its index in returns()
    void returnRemoved(const QString& trackId, int index);
    // Name, colour, mixer settings, sends, input, monitoring, arming, height or
    // folding (kMaster: the master's mixer).
    void trackChanged(const QString& trackId);
    // The tracks' order or groups changed (not which tracks there are).
    void tracksArranged();
    void clipsChanged(const QString& trackId);
    // Devices added/removed/moved/toggled (in racks too), a sidechain, a MIDI input, macros or a rack's name changed.
    void devicesChanged(const QString& trackId);
    // A rack chain's name or mixer.
    void chainChanged(const QString& trackId, const QString& chainId);
    void deviceParamChanged(const QString& trackId, const QString& deviceId, const QString& paramId);
    // A device's state was set (a preset, a sample).
    void deviceStateChanged(const QString& trackId, const QString& deviceId);
    // Devices on it were folded or unfolded.
    void devicesFolded(const QString& trackId);
    // A rack on it showed or hid its chain list, or its chain's devices.
    void rackViewChanged(const QString& trackId);
    // It was frozen or unfrozen.
    void freezeChanged(const QString& trackId);
    // Tempo, time signature, key, loop, automation lock (after reset too).
    void settingsChanged();
    // An envelope was set or removed (owner: a track id or kMaster).
    void automationChanged(const QString& owner, const QString& key);
    // What an owner's automation shows.
    void automationViewChanged(const QString& owner);
    // Everything replaced (new, open).
    void reset();
    // The file it is saved to changed.
    void pathChanged();

private:
    Track& trackRef(const QString& trackId);
    // After a change of what a track holds: renamed after it if it was named so before.
    void followContents(const QString& trackId, bool followed);
    // Names the tracks from `from` on (and the returns, with `returns`) from
    // their templates, quietly; the ids of the tracks renamed.
    QStringList renumber(int from, bool returns = false);
    void announceRenamed(const QStringList& trackIds);
    Device& deviceRef(const QString& trackId, const QString& deviceId);
    Chain& chainRef(const QString& trackId, const QString& chainId);

    double tempo_ = 120.0;
    TimeSignature timeSignature_;
    std::optional<Key> key_;
    bool loopEnabled_ = false;
    double loopStart_ = 0.0;
    double loopEnd_ = 16.0;
    bool automationLocked_ = false;
    Track master_;
    std::vector<Track> tracks_;
    std::vector<Track> returns_;
    QSet<QString> foldedDevices_;
    QSet<QString> shownChainLists_;
    QSet<QString> hiddenRackDevices_;
    QString path_;
};

}  // namespace sub::app
