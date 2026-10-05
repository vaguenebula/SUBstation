#pragma once
// High-level, undoable edit operations used by the UI: ProjectEditor.
//
// Every user action becomes undo commands (model/Commands.h), pushed onto the
// undo stack through push(), which refuses those changing what frozen audio
// holds (saying why on `refused`). Its operations are grouped by what they
// edit, one source file per area: EditorTracks.cpp (tracks, returns and sends,
// groups, inputs, recordings), EditorSettings.cpp (tempo, time signature, key,
// loop), EditorClips.cpp (clips and time selections), EditorDeviceChains.cpp
// (devices in chains), EditorRacks.cpp (racks, their chains and macros),
// EditorDeviceSettings.cpp (a device's parameters, state, presets, switch and
// sidechain), EditorAutomation.cpp (envelopes and the lanes shown) and
// EditorFreezing.cpp (freezing, flattening, and what frozen tracks refuse).
// Kinds of devices and new devices are in model/Devices.h.
//
// Some changes are view state, saved with the project but not undone (as in
// Ableton): track heights, folding, arming, solo (tracks' and rack chains'),
// which devices are folded, which automation shows, and Lock Envelopes. They
// change the project directly and push nothing.
//
// Conventions:
// - Ids are QStrings; an empty one is "none" (no chain: a track's own; no
//   group; no track: a new one). An index below 0 is "last".
// - Operations that make something return its id ("" if nothing was made).
// - Continuous gestures pass a merge key (one per gesture, e.g.
//   QUuid::createUuid().toString()): its steps are one undo step. Empty: never
//   merges.
// - An edit the user can't make (a send closing a cycle, a channel out of
//   range...) throws EditError with a message for the user. Callable from QML
//   (Q_INVOKABLE) means it never throws: the try* operations are the throwing
//   ones' QML forms, which report the message on `refused` and return false
//   (or ""). Asking about a track, device, chain or clip that isn't there is a
//   programming error (std::out_of_range from Project), but the Q_INVOKABLE
//   operations do nothing for ids that are gone (QML may hold stale ones).
// - Results hold copies, never references into the project (it holds tracks
//   by value: a reference into it doesn't outlive the next change).

#include "editor/ClipRef.h"
#include "editor/ClipboardContent.h"
#include "editor/CopiedAutomation.h"
#include "editor/CopiedTracks.h"
#include "editor/TimeRange.h"
#include "editor/TrackPlace.h"
#include "model/Automation.h"
#include "model/Clip.h"
#include "model/Device.h"
#include "model/Keys.h"
#include "model/OrderedMap.h"
#include "model/ParamSpec.h"
#include "model/Project.h"
#include "model/RecordedTake.h"
#include "model/Timebase.h"
#include "model/Track.h"

#include <QList>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariant>

#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

class QUndoCommand;
class QUndoStack;

namespace sub::app {

class ProjectEditor : public QObject {
    Q_OBJECT

public:
    // How the editor learns about a parameter it doesn't know (a plug-in's, a
    // rack's): its normalized mapping, for macros. (trackId, deviceId, paramId)
    // -> its spec; none: unknown. The engine bridge fills it in.
    using ParamInfoSource =
        std::function<std::optional<ParamSpec>(const QString& trackId, const QString& deviceId, const QString& paramId)>;
    // What a plug-in's parameter is now, as set in the plug-in's own editor (the
    // model doesn't have it): (owner, automation key) -> its plain value; none:
    // unknown. The engine bridge fills it in.
    using OwnValueSource = std::function<std::optional<double>(const QString& owner, const QString& key)>;
    // Where new devices come from: (kind, plug-in) -> a new device as the user's
    // default preset for that kind has it, or none (as it comes; see
    // io/Presets.h defaultDevice).
    using DeviceDefaults =
        std::function<std::optional<Device>(const QString& kind, const std::optional<PluginRef>& plugin)>;

    ProjectEditor(Project* project, QUndoStack* undoStack, QObject* parent = nullptr);
    ~ProjectEditor() override;

    Project* project() const { return project_; }
    QUndoStack* undoStack() const { return undoStack_; }

    // --- Tracks (EditorTracks.cpp) ---

    // A new audio track at `index` (< 0: last), in the group at that place;
    // named `name` (""; "<n> Audio"). Its id.
    Q_INVOKABLE QString addAudioTrack(int index = -1, const QString& name = {});
    QString addAudioTrack(int index, const QString& name, const TrackParent& parent);
    // A MIDI track with the default instrument (as its default preset has it).
    Q_INVOKABLE QString addMidiTrack(int index = -1, const QString& name = {});
    // A MIDI track with a built-in `instrument` ("": none), or with an
    // instrument `plugin` (as its default preset has it, if there is one; its
    // editor shows: pluginAdded).
    QString addMidiTrack(int index, const QString& name, const QString& instrument,
                         const std::optional<PluginRef>& plugin = std::nullopt, const TrackParent& parent = kAtIndex);
    // A MIDI track with an instrument device on it (an instrument preset: a
    // plug-in, a built-in one or an instrument rack). One undo step.
    QString addMidiTrackWith(const Device& device, int index = -1, const TrackParent& parent = kAtIndex,
                             const QString& text = QStringLiteral("Insert MIDI Track"));
    // Where a track inserted "after" this one goes: after it and what is in it,
    // in its group. Without a track (or ""): last, in no group.
    InsertionPoint insertionPoint(const QString& trackId) const;
    // Delete tracks (and return tracks); a group goes with what is in it, a
    // return with the sends into it (and their automation). The inputs and
    // sidechains they were the source of go too. One undo step. Refused if one
    // is in a frozen group (that isn't going too).
    Q_INVOKABLE void deleteTracks(const QStringList& trackIds);
    Q_INVOKABLE void renameTrack(const QString& trackId, const QString& name);
    Q_INVOKABLE void setTrackColor(const QString& trackId, const QString& color);
    // Mixer settings: VolumeDb, Pan, Mute, Solo (the master: volume and pan;
    // EditError for the others: the master is always heard). Mute and Solo are
    // bools (any value but 0 is true). Solo is a listening aid: saved, but not
    // undone (see soloTracks).
    void setTrackParam(const QString& trackId, TrackField field, double value, const QString& mergeKey = {});
    // Volume (VolumeDb) or pan (Pan) on several tracks (track id -> value) as one undo step.
    void setTracksParam(const QMap<QString, double>& values, TrackField field, const QString& mergeKey = {});
    // Solo (or unsolo) these tracks (returns too). `exclusive` (soloing): every
    // other track is unsoloed. A listening aid, as in Ableton: saved, but not an undo step.
    Q_INVOKABLE void soloTracks(const QStringList& trackIds, bool solo, bool exclusive = false);
    // Arm (or disarm) tracks for recording; `exclusive` (arming): every other
    // track is disarmed. Groups and frozen tracks aren't armed (the latter
    // refused). Saved, but not undone.
    Q_INVOKABLE void armTracks(const QStringList& trackIds, bool armed, bool exclusive = false);
    // View state: saved with the project but not undone.
    Q_INVOKABLE void setTrackHeight(const QString& trackId, int height);
    // Fold or unfold a track: folded, it is a thin row without its automation;
    // a folded group hides what is in it. View state.
    Q_INVOKABLE void setFolded(const QString& trackId, bool folded);

    // Ctrl+C on tracks: them (a group with what is in it) as they are now, to
    // paste later (plug-ins in the state last stored in the model: store their
    // states first). None if none of them is a track of the arrangement.
    std::optional<CopiedTracks> copyTracks(const QStringList& trackIds) const;
    // Ctrl+X on tracks: copy them, then delete them. None if they weren't
    // deleted (refused: in a frozen group), so nothing is cut.
    std::optional<CopiedTracks> cutTracks(const QStringList& trackIds);
    // Ctrl+V of copied tracks: new copies of them, together after track `after`
    // (and what is in it), in its group ("": last). As duplicateTracks makes
    // them; what they took their input or sidechain from, or sent to, is let go
    // of if it is gone now. One undo step; the copies of the roots.
    QStringList pasteTracks(const CopiedTracks& copied, const QString& after = {});
    // Ctrl+D on tracks: a copy of each (a group with what is in it), together
    // after the last of them (and what is in it), in its group. The copies have
    // new clips and devices (plug-ins in the state last stored in the model) and
    // the same automation, sends, inputs and sidechains (from the copies, where
    // they came from tracks copied with them); they aren't armed. One undo
    // step; the copies of these tracks.
    Q_INVOKABLE QStringList duplicateTracks(const QStringList& trackIds);

    // Groups.
    // Ctrl+G: a new group holding these tracks (and what is in them), where the
    // first of them was, in its group. One undo step; the new group.
    Q_INVOKABLE QString groupTracks(const QStringList& trackIds);
    // Ctrl+Shift+G: the groups go, and what was in them takes their place, in
    // their groups. One undo step.
    Q_INVOKABLE void ungroup(const QStringList& groupIds);
    // Whether moveTracks would do something with these.
    Q_INVOKABLE bool canMoveTracks(const QStringList& trackIds, int index, const QString& parent) const;
    // Move tracks (and what is in them) to before the track at `index` (as the
    // tracks are now; past the end: last), into group `parent` ("": none). One
    // undo step; false if they can't go there (a group into itself, amid
    // another group's tracks, into or out of a frozen group).
    Q_INVOKABLE bool moveTracks(const QStringList& trackIds, int index, const QString& parent);

    // Returns and sends.
    // Ctrl+Alt+T: a return track, last (or at `index` among the returns).
    Q_INVOKABLE QString addReturnTrack(int index = -1, const QString& name = {});
    // A track's (or a group's, or a return's) send to a return: its level (dB,
    // held to the faders' range) and where it taps (none: as it is). A send not
    // made yet starts silent and after the fader. EditError for a send the
    // routing can't have (into itself, or a cycle among returns).
    void setSend(const QString& trackId, const QString& returnId, std::optional<double> levelDb = std::nullopt,
                 std::optional<bool> preFader = std::nullopt, const QString& mergeKey = {});
    Q_INVOKABLE void removeSend(const QString& trackId, const QString& returnId);

    // Inputs and monitoring.
    // A track's audio input: device channels, none for none. EditError for
    // more than two (an input is one channel or a pair).
    void setTrackInput(const QString& trackId, const std::vector<int>& channels);
    // An audio track's input from another track's output, after its fader (a
    // track, a group or a return), or the master's (kMaster: resampling),
    // instead of device channels; none: no input. EditError for a source it
    // can't take (itself, one it feeds: a cycle; not an audio track).
    void setTrackInputTrack(const QString& trackId, const std::optional<QString>& sourceId);
    // One of kMonitorModes; EditError for another.
    void setTrackMonitor(const QString& trackId, const QString& mode);
    // A MIDI track's MIDI input (none: none). EditError for a channel out of 0..16.
    void setTrackMidiInput(const QString& trackId, const std::optional<MidiInput>& midiInput);

    // Recording.
    // Finished takes become clips, one undo step: audio clips, and MIDI clips of
    // the notes played (their starts on the grid of `quantize` beats, if given:
    // record quantization). As in Ableton's Arrangement recording, a take
    // replaces what was under it. The clips made.
    ClipRefs addRecordings(const std::vector<RecordedTake>& takes, double quantize = 0.0);

    // --- Settings (EditorSettings.cpp) ---

    // Change the tempo (20..999, to 0.01), trimming clips that would otherwise
    // overlap (within a drag, from the clips as they were when it began).
    Q_INVOKABLE void setTempo(double bpm, const QString& mergeKey = {});
    void setTimeSignature(const TimeSignature& timeSignature);
    Q_INVOKABLE void setTimeSignature(int numerator, int denominator);
    // The project's key (none: none). Audio added afterwards is transposed to it.
    void setKey(const std::optional<Key>& key);
    // By its name as Key::name has it ("F#m"; "" or a name that isn't one: none).
    Q_INVOKABLE void setKeyByName(const QString& name);
    // The loop: at least a sixteenth long, not before the timeline's start.
    Q_INVOKABLE void setLoop(bool enabled, double start, double end, const QString& mergeKey = {});
    Q_INVOKABLE void setLoopEnabled(bool enabled);

    // --- Clips and time selections (EditorClips.cpp) ---

    // Replace tracks' clips (track id -> its clips; sorted by the project), as
    // one undo step: a clip edit the pure functions (model/Edits.h) worked out.
    void commitClips(const QString& text, const QMap<QString, std::vector<Clip>>& after,
                     const QString& mergeKey = {});
    // Place audio files one after another; `sources` is (path, duration in
    // seconds) each. With no track ("", or a MIDI or group track) a new audio
    // track is made (at `trackIndex`, < 0: last). A tempo or key in a file's
    // name sets up its clip (see clipSettings): loops and long files are warped,
    // and audio is transposed to the project's key. The clips made.
    ClipRefs addClips(const QString& trackId, double startBeat, const std::vector<std::pair<QString, double>>& sources,
                      int trackIndex = -1);
    // An empty MIDI clip on a MIDI track, named after the track. It wins against
    // clips it overlaps, like a placed clip. None: not a MIDI track, too short,
    // or refused (frozen).
    std::optional<ClipRef> addMidiClip(const QString& trackId, double startBeat, double lengthBeats);
    // An empty MIDI clip over a time range on each MIDI track of `trackIds`.
    ClipRefs addMidiClipsOver(double startBeat, double endBeat, const QStringList& trackIds);
    // Where a new MIDI clip made at `beat` goes: from the grid line at or before
    // it (not reaching back over the clip before), one bar long or up to the
    // next clip. (start, length).
    std::pair<double, double> midiClipSpan(const QString& trackId, double beat, double gridStep = 0.0) const;
    // Replace a MIDI clip's notes (piano roll edits). With a merge key, one
    // gesture's edits are one undo step.
    void setClipNotes(const ClipRef& ref, const std::vector<Note>& notes, const QString& text,
                      const QString& mergeKey = {});
    // How far these clips can move across tracks: within the track list, and
    // only onto tracks of their own kind (audio or MIDI). A move that would put
    // any clip on the other kind of track keeps them on their tracks.
    int clampTrackDelta(const ClipRefs& refs, int trackDelta) const;
    // Move (or copy) clips in time and across tracks, with the automation under
    // them (unless it is locked). The clips where they are now.
    ClipRefs moveClips(const ClipRefs& refs, double deltaBeats, int trackDelta = 0, bool copyClips = false);
    // Commit an edited version of one clip (e.g. after trimming): it wins where
    // it overlaps others.
    void replaceClip(const QString& trackId, const Clip& clip, const QString& text);
    // Apply `change` to each clip in `refs` as one undo step (clip view
    // settings). Positions must not change. Lengths in beats may (warping,
    // segment BPM): a clip that would run into the next one is trimmed, as on a
    // tempo change (within a drag, from the clips as they were when it began).
    void updateClips(const ClipRefs& refs, const std::function<Clip(const Clip&)>& change, const QString& text,
                     const QString& mergeKey = {});
    void deleteClips(const ClipRefs& refs);
    // Ableton's Ctrl+D: copies land right after the selection.
    ClipRefs duplicateClips(const ClipRefs& refs);
    void splitClips(const ClipRefs& refs, double atBeat);
    // The MIDI clips among `refs` that Consolidate joins: on each track with two or more.
    OrderedMap<QString, std::vector<Clip>> consolidatable(const ClipRefs& refs) const;
    // Join the selected MIDI clips on each track into one clip spanning them
    // (Ctrl+J), as one undo step. The new clips; none if there was nothing to join.
    ClipRefs consolidateClips(const ClipRefs& refs);
    // The clips on these tracks under a beat (not at their edges).
    ClipRefs clipsAt(const QStringList& trackIds, double beat) const;
    // The grid area that fully contains these clips: the earliest start to the
    // latest end, on every track from the topmost clip's to the lowest one's.
    // None if none of them exist.
    std::optional<TimeRange> clipsArea(const ClipRefs& refs) const;
    // The clips on these tracks that overlap the beat range.
    QSet<ClipRef> clipsInRange(double start, double end, const QStringList& trackIds) const;

    // Time selections. A time selection is everything in it: unless automation
    // is locked, its edits act on the automation of every track in it as on its
    // clips, whether the track has clips there or not (a group's too); across
    // tracks only the mixer's (a device's automation belongs to its track).
    //
    // Over frozen tracks (and what is in frozen groups) these edits take the
    // frozen audio along: they do to its segments (Freeze::segments) what they
    // do to the clips, in the same undo step, so what plays stays in step with
    // the arrangement (and the automation baked into it goes with them too). On
    // a frozen track a stretch that is moved, copied, duplicated or pasted
    // replaces everything where it lands, as its audio does (on other tracks
    // only the clips it brings replace what they land on). Refused (`refused`
    // says why) when a selection takes in some of what a frozen group holds
    // but not all of it (its audio is one render of all of it), when clips
    // would move between a frozen track and another track, and when what is
    // pasted into a frozen track wasn't copied from it (see paste).
    //
    // Delete what is between two beats on the given tracks: the clip content,
    // and the automation (unless automation is locked). False if refused.
    Q_INVOKABLE bool deleteRange(double start, double end, const QStringList& trackIds);
    // Ableton's Ctrl+D on a time selection: copy what is between two beats (the
    // clip content, and the automation unless it is locked) to right after
    // `end`, replacing what was there. The copies; none if refused.
    std::optional<ClipRefs> duplicateRange(double start, double end, const QStringList& trackIds);
    // Ctrl+C on a time selection: what is between two beats on these tracks: the
    // clip content (clips across its edges are cut there) and the automation,
    // unless automation is locked; and the frozen audio there of each frozen
    // track (or group) the selection takes in whole. None if there is nothing there.
    std::optional<ClipboardContent> copyRange(double start, double end, const QStringList& trackIds) const;
    // Ctrl+X on a time selection: copy it, then take the clip content, and the
    // automation copied with it, out. One undo step. None if there was nothing
    // to cut, or it was refused.
    std::optional<ClipboardContent> cutRange(double start, double end, const QStringList& trackIds);
    // Where pasted content goes: its top track onto `trackId` and the rest onto
    // the tracks below, as they were copied. If they aren't all tracks of the
    // content's kind, the tracks it was copied from; none if those are gone.
    std::optional<QStringList> pasteTargets(const ClipboardContent& content, const QString& trackId) const;
    // Ctrl+V: copied clip content at `atBeat` (see pasteTargets for which
    // tracks), replacing what is there, as new clips. Its automation comes along
    // unless automation is locked; onto another track, only the mixer's volume
    // and pan. Onto unfrozen tracks the clips go as they are (copied from a
    // frozen track too). Into a frozen track (or what is in a frozen group)
    // only content copied from it pastes, carrying its frozen audio (the same
    // render), onto the tracks it came from: it replaces everything where it
    // lands, frozen audio and clips alike; other content is refused. One undo
    // step. The area pasted over (from the top track to the lowest); none if
    // the content has nowhere to go, or was refused.
    std::optional<TimeRange> paste(const ClipboardContent& content, double atBeat, const QString& trackId = {});
    // What moveRange would make of the clips, without making it. For previews while dragging.
    MovedRange movedRange(double start, double end, const QStringList& trackIds, double deltaBeats,
                          int trackDelta = 0, bool copyClips = false) const;
    // Ableton's drag of a time selection: move (or copy) what is between two
    // beats, in time and across tracks: the clip content, and the automation
    // (unless it is locked; across tracks, only the mixer's). Clips across the
    // range's edges are split there; what moves replaces what it lands on.
    // Frozen tracks move in time only, their frozen audio along. Where the
    // range ended up: its new start and tracks (where it was, if refused).
    std::pair<double, QStringList> moveRange(double start, double end, const QStringList& trackIds,
                                             double deltaBeats, int trackDelta = 0, bool copyClips = false);
    // Reverse the audio between two beats on these tracks: each audio clip whose
    // file is in `reversedFiles` (file -> (its reversed copy, its length in
    // seconds)) is split at the range's edges, and the part inside plays the
    // reversed copy, backwards (see edits::reverseClip). One undo step. The
    // reversed clips.
    ClipRefs reverseRange(double start, double end, const QStringList& trackIds,
                          const QMap<QString, std::pair<QString, double>>& reversedFiles);

    // --- Devices in chains (EditorDeviceChains.cpp) ---

    // Where new devices come from (see DeviceDefaults). Without it, devices
    // start as they come.
    void setDeviceDefaults(DeviceDefaults defaults);
    // Add a built-in device of `kind` (or an empty rack, kRackKind) to a track's
    // (or the master's) chain ("") or to a rack's chain on it (its id), before
    // the device at `index` (< 0: last), as its default preset has it if there
    // is one. An instrument only goes on a MIDI track ("" otherwise), where it
    // comes first in its chain and replaces any other instrument there; effects
    // never go before it. Its id.
    Q_INVOKABLE QString addDevice(const QString& trackId, const QString& kind, int index = -1,
                                  const QString& chain = {});
    // The same with a plug-in (kind kPluginKind and `plugin`): its editor shows (pluginAdded).
    QString addDevice(const QString& trackId, const QString& kind, int index, const std::optional<PluginRef>& plugin,
                      const QString& chain = {});
    // Put a new device (a whole rack too: a preset) into a chain, as addDevice
    // does. One undo step (`text`; "": "Add <its name>"); false if it can't go there.
    bool insertDevice(const QString& trackId, const Device& device, int index = -1, const QString& chain = {},
                      const QString& text = {}, bool showEditors = true);
    // Put new devices (racks too) into a chain, in their order, before the
    // device at `index` (< 0: last), as addDevice does: an instrument only on a
    // MIDI track, first, replacing the one there; nothing nesting racks too
    // deep. One undo step; the ids of the devices that went in (none: nothing
    // changed). Their plug-ins' editors show if `showEditors`.
    QStringList insertDevices(const QString& trackId, const std::vector<Device>& devices, int index = -1,
                              const QString& chain = {}, const QString& text = QStringLiteral("Add Devices"),
                              bool showEditors = true);
    // Copies of devices (racks with everything in them; those in a selected
    // rack go with it), in their order on the track, for pasting: plug-ins in
    // the state last stored in the model (store their states first).
    std::vector<Device> copyDevices(const QString& trackId, const QStringList& deviceIds) const;
    // New devices like the copied ones into a chain of a track, before the
    // device at `index` (< 0: last), as insertDevices puts them. Their
    // sidechains stay, unless the source is gone or would close a cycle here.
    // Those whose originals are `folded` are folded too. One undo step; the ids
    // of the devices pasted.
    QStringList pasteDevices(const QString& trackId, const std::vector<Device>& copied, int index = -1,
                             const QString& chain = {}, const QSet<QString>& folded = {}, const QString& text = {});
    // Fold or unfold devices in the device view. View state: saved, not undone.
    Q_INVOKABLE void setDevicesFolded(const QString& trackId, const QStringList& deviceIds, bool folded);
    // Move a device to position `index` in its chain (an instrument stays first).
    Q_INVOKABLE void moveDevice(const QString& trackId, const QString& deviceId, int index);
    // Move devices together (in their order on the track) to before the device
    // at `index` in a chain of the track as it is now (the end if past it): its
    // own ("") or a rack's. One undo step; an instrument doesn't move, and
    // nothing goes before one; a rack doesn't go into itself, nor nest too
    // deep. False if nothing moved.
    Q_INVOKABLE bool moveDevices(const QString& trackId, const QStringList& deviceIds, int index,
                                 const QString& chain = {});
    // Move effects (racks too, with everything in them; in their order on the
    // track) to another track's (or the master's) chain, or a rack's chain there
    // (`chain`), before the device at `index` (< 0: last; never before its
    // instrument). They stay the same devices, so plug-ins keep their state,
    // and their automation goes with them, and their sidechains (unless one
    // would close a cycle there). One undo step; false if nothing moved.
    Q_INVOKABLE bool moveDevicesToTrack(const QString& trackId, const QStringList& deviceIds, const QString& toTrackId,
                                        int index = -1, const QString& chain = {});
    Q_INVOKABLE void removeDevice(const QString& trackId, const QString& deviceId);
    // Delete devices from a track (in racks too; a rack with what is in it), in
    // one undo step, with their automation (and their chains' faders') and the
    // macro mappings to them.
    Q_INVOKABLE void removeDevices(const QString& trackId, const QStringList& deviceIds);

    // --- Racks (EditorRacks.cpp) ---

    // Ctrl+G in the device view: these devices (in one chain, in its order) go
    // into a new rack, in one chain, where the first of them was. One undo
    // step; the rack ("" if they aren't all in one chain, or it would nest too deep).
    Q_INVOKABLE QString groupDevices(const QString& trackId, const QStringList& deviceIds);
    // Ctrl+Shift+G: a rack goes, and its chains' devices take its place, one
    // chain after another (an instrument coming out goes first). The automation
    // of its chains' faders and its macros go too. One undo step; false if it
    // isn't a rack, or several instruments would come out of it (layered
    // instruments: a chain has just one).
    Q_INVOKABLE bool ungroupRack(const QString& trackId, const QString& rackId);
    // A new, empty chain of a rack (last, or at `index`), named `name` ("":
    // "Chain <n>"). Its id; EditError if it isn't a rack.
    QString addRackChain(const QString& trackId, const QString& rackId, int index = -1, const QString& name = {});
    // Delete chains of racks, with their devices (and their automation, and
    // their faders'). One undo step.
    Q_INVOKABLE void removeRackChains(const QString& trackId, const QStringList& chainIds);
    // A copy of a chain right after it: new devices with the same settings
    // (plug-ins in the state they were last saved in, as presets are). Its id.
    Q_INVOKABLE QString duplicateRackChain(const QString& trackId, const QString& chainId);
    // Reorder a rack's chains: this one to `index` (among the others).
    Q_INVOKABLE void moveRackChain(const QString& trackId, const QString& chainId, int index);
    Q_INVOKABLE void renameChain(const QString& trackId, const QString& chainId, const QString& name);
    // A rack chain's mixer: VolumeDb, Pan, Mute, Solo (saved, but not undone,
    // as a track's). Mute and Solo are bools (any value but 0 is true).
    void setChainParam(const QString& trackId, const QString& chainId, ChainField field, double value,
                       const QString& mergeKey = {});
    // The same by the field's name ("volume_db", "pan", "mute", "solo").
    Q_INVOKABLE void setChainParam(const QString& trackId, const QString& chainId, const QString& field, double value,
                                   const QString& mergeKey = {});

    // Macros: a rack's parameters, each moving the parameters mapped to it.
    // Their values are normalized. paramInfo() describes a device's parameter;
    // built-in devices' are known, others' come from the ParamInfoSource.
    void setParamInfo(ParamInfoSource describe);
    // Where setMacro() learns what a plug-in's parameter is now (see OwnValueSource).
    void setOwnValue(OwnValueSource read);
    std::optional<ParamSpec> paramInfo(const QString& trackId, const QString& deviceId, const QString& paramId) const;
    // What a rack's macro at `value` sets: itself, and each parameter mapped to
    // it ((device id, param id) -> plain value).
    QMap<DeviceParam, double> macroTargets(const QString& trackId, const QString& rackId, int index,
                                           double value) const;
    // Turn a rack's macro: it and every parameter mapped to it, one undo step
    // (one per gesture, with a merge key).
    Q_INVOKABLE void setMacro(const QString& trackId, const QString& rackId, int index, double value,
                              const QString& mergeKey = {});
    // Map a rack's macro to a parameter of a device in it, over the parameter's
    // normalized `low`..`high` (a parameter is mapped to one macro of the rack
    // at a time). EditError for a device not in the rack (or no such macro).
    void mapMacro(const QString& trackId, const QString& rackId, int index, const QString& deviceId,
                  const QString& paramId, double low = 0.0, double high = 1.0);
    Q_INVOKABLE void unmapMacro(const QString& trackId, const QString& rackId, const QString& deviceId,
                                const QString& paramId);
    // The rack and macro a parameter is mapped to (the nearest rack's), if any.
    std::optional<std::pair<QString, int>> macroOf(const QString& trackId, const QString& deviceId,
                                                   const QString& paramId) const;

    // --- A device's settings (EditorDeviceSettings.cpp) ---

    // Change a parameter. The automation lane shows it (parameterTouched).
    Q_INVOKABLE void setDeviceParam(const QString& trackId, const QString& deviceId, const QString& paramId,
                                    double value, const QString& mergeKey = {});
    // The same with `old`, its value before, if the model doesn't know it (a
    // plug-in's parameters live in the plug-in).
    void setDeviceParam(const QString& trackId, const QString& deviceId, const QString& paramId, double value,
                        const QString& mergeKey, std::optional<double> old);
    // Change several of a built-in device's parameters (param id -> value, the
    // first one's lane shows) as one undo step (one per gesture, with a merge
    // key, while the same parameters change).
    void setDeviceParams(const QString& trackId, const QString& deviceId, const OrderedMap<QString, double>& values,
                         const QString& mergeKey = {},
                         const QString& text = QStringLiteral("Change Device Parameters"));
    Q_INVOKABLE void setDeviceParams(const QString& trackId, const QString& deviceId, const QStringList& paramIds,
                                     const QList<double>& values, const QString& mergeKey = {},
                                     const QString& text = QStringLiteral("Change Device Parameters"));
    // A parameter taken hold of (clicked) without changing it: as Ableton does,
    // the arrangement shows its automation (parameterTouched).
    Q_INVOKABLE void touchParameter(const QString& owner, const QString& key);
    // Replace a device's state (base64): a plug-in's, e.g. with a preset, or a
    // built-in device's besides its parameters. `old` is its state before, to go
    // back to on undo.
    void setDeviceState(const QString& trackId, const QString& deviceId, const std::optional<QString>& old,
                        const std::optional<QString>& state, const QString& text = QStringLiteral("Load Preset"));
    // Load a preset (a device: io/Serialization.h loadPreset) into a device of
    // the same kind (loadsInto), which stays where it is, with its id, on/off
    // switch and sidechain: a plug-in takes the preset's state, a built-in
    // device its parameters and state, a rack its chains (new devices), macros
    // and name. One undo step; false if it can't (another kind of device, or a
    // rack that would nest too deep there). A plug-in's state before is the
    // model's: store it first.
    bool loadPresetInto(const QString& trackId, const QString& deviceId, const Device& preset,
                        const QString& text = QStringLiteral("Load Preset"));
    // A rack's name ("": named by its kind again).
    Q_INVOKABLE void renameRack(const QString& trackId, const QString& deviceId, const QString& name,
                                const QString& text = QStringLiteral("Rename Rack"));
    Q_INVOKABLE void setDeviceEnabled(const QString& trackId, const QString& deviceId, bool enabled);
    // What a device's sidechain (aux) input hears: a track's (a group's, a
    // return's) signal, after its fader, before it, or after one of its devices;
    // none: nothing. EditError for a source it can't take (the master, its own
    // track, or one its track feeds: a cycle) or a tap after a device that
    // isn't on the source.
    void setDeviceSidechain(const QString& trackId, const QString& deviceId, const std::optional<Sidechain>& sidechain);

    // --- Automation (EditorAutomation.cpp) ---
    // Envelopes are normalized (see model/Automation.h); an owner is a track id
    // (a return's) or kMaster.

    void setEnvelope(const QString& owner, const QString& key, const Envelope& points,
                     const QString& text = QStringLiteral("Change Automation"), const QString& mergeKey = {});
    // A new breakpoint; its index. With a merge key, dragging it right away
    // (moveAutomationPoints with the same key) is the same undo step.
    Q_INVOKABLE int addAutomationPoint(const QString& owner, const QString& key, double beat, double value,
                                       const QString& mergeKey = {});
    // Move points of `original` (the envelope when the drag began) together.
    // Where they are now ({index in `original`: index}).
    QMap<int, int> moveAutomationPoints(const QString& owner, const QString& key, const Envelope& original,
                                        const QList<int>& indices, double deltaBeats, double deltaValue,
                                        const QString& mergeKey = {});
    Q_INVOKABLE void deleteAutomationPoints(const QString& owner, const QString& key, const QList<int>& indices);
    void setAutomationCurve(const QString& owner, const QString& key, const Envelope& original, int index,
                            double curve, const QString& mergeKey = {});
    Q_INVOKABLE void clearEnvelope(const QString& owner, const QString& key);
    // Delete the automation between two beats on these lanes, as one undo step.
    void deleteAutomationRange(double start, double end, const QList<LaneRef>& lanes);
    // Move the automation between two beats on these lanes (`originals`: their
    // envelopes when the drag began) in time and value, as one undo step.
    void moveAutomationRange(double start, double end, const QMap<LaneRef, Envelope>& originals, double deltaBeats,
                             double deltaValue, const QString& mergeKey = {});
    // Copy the automation between two beats to right after `end`, over what was there.
    void duplicateAutomationRange(double start, double end, const QList<LaneRef>& lanes);
    // Ctrl+C on a lane range: the automation between two beats on these lanes
    // (those that have any). None if none of them has.
    std::optional<CopiedAutomation> copyAutomationRange(double start, double end, const QList<LaneRef>& lanes) const;
    // Ctrl+X on a lane range: copy it, then delete it. One undo step.
    std::optional<CopiedAutomation> cutAutomationRange(double start, double end, const QList<LaneRef>& lanes);
    // Where each copied lane goes: onto `lanes` (the selected ones, in order)
    // one to one if there are as many, or one copied lane onto each of them;
    // otherwise onto the lanes it was copied from. None for a lane that is gone
    // (its owner, or the device or send it automates).
    QList<std::optional<LaneRef>> automationPasteTargets(const CopiedAutomation& content,
                                                         const QList<LaneRef>& lanes = {}) const;
    // Ctrl+V: copied automation at `atBeat`, replacing what is there, onto the
    // lanes automationPasteTargets picks. One undo step; the lanes pasted onto.
    QList<LaneRef> pasteAutomation(const CopiedAutomation& content, double atBeat, const QList<LaneRef>& lanes = {});
    // Lock Envelopes: whether automation stays in place when clips move
    // (unlocked, it moves with them). A setting, not an edit: not undone.
    Q_INVOKABLE void setAutomationLocked(bool locked);

    // What the arrangement shows of each owner's automation. View state: saved
    // with the project, but not undone (like track heights).
    // What a lane shows when nothing was chosen: the first automated target, else the volume.
    Q_INVOKABLE QString defaultAutomationKey(const QString& owner) const;
    // Show an owner's automation, with `key` (if given) in its main lane.
    Q_INVOKABLE void showAutomation(const QString& owner, const QString& key = {});
    Q_INVOKABLE void hideAutomation(const QString& owner);
    // Show every track's (and the master's) automation, or hide it all if all of
    // it shows. Whether it shows now.
    Q_INVOKABLE bool toggleAllAutomation();
    // Another lane below the owner's main lane: the first automated target not
    // shown yet (else the first mixer control not shown).
    Q_INVOKABLE void addAutomationLane(const QString& owner);
    // Show `key` in lane `index` (-1: the main lane).
    Q_INVOKABLE void setAutomationLane(const QString& owner, int index, const QString& key);
    Q_INVOKABLE void removeAutomationLane(const QString& owner, int index);
    Q_INVOKABLE void resetAutomationView(const QString& owner);

    // --- Freezing (EditorFreezing.cpp) ---

    // Freezes tracks (and groups, returns) with their rendered audio (the
    // engine bridge renders it), one undo step; they are disarmed. Tracks in a
    // group frozen with them, or that can't be frozen (freezeProblem), are left
    // as they are. Those frozen.
    QStringList freezeTracks(const OrderedMap<QString, Freeze>& freezes);
    // Unfreezes tracks, one undo step: their devices load again. (A track in a
    // frozen group stays as it is until the group is unfrozen.) Those unfrozen.
    Q_INVOKABLE QStringList unfreezeTracks(const QStringList& trackIds);
    // Flattens frozen audio and MIDI tracks: each becomes an audio track playing
    // its frozen audio as a clip, without its devices or their automation (its
    // mixer, sends and routing stay). One undo step; those flattened.
    Q_INVOKABLE QStringList flattenTracks(const QStringList& trackIds);
    // Whether an automation lane is baked into frozen audio: a device's of a
    // frozen track (or one in a frozen group), and the mixer's of a track in a
    // frozen group. (A frozen track's own mixer and every send stay live.)
    Q_INVOKABLE bool laneFrozen(const QString& owner, const QString& key) const;

    // --- For QML: the operations above that throw EditError, reporting it on
    // `refused` instead (false or "": refused) ---

    // field: "volume_db", "pan", "mute" or "solo" (another: false, nothing said).
    Q_INVOKABLE bool trySetTrackParam(const QString& trackId, const QString& field, double value,
                                      const QString& mergeKey = {});
    Q_INVOKABLE bool trySetSendLevel(const QString& trackId, const QString& returnId, double levelDb,
                                     const QString& mergeKey = {});
    Q_INVOKABLE bool trySetSendPreFader(const QString& trackId, const QString& returnId, bool preFader);
    Q_INVOKABLE bool trySetTrackInput(const QString& trackId, const QList<int>& channels);
    // sourceId "": no input.
    Q_INVOKABLE bool trySetTrackInputTrack(const QString& trackId, const QString& sourceId);
    Q_INVOKABLE bool trySetTrackMonitor(const QString& trackId, const QString& mode);
    // `enabled` false: no MIDI input. device "": every input; channel 0: every channel.
    Q_INVOKABLE bool trySetTrackMidiInput(const QString& trackId, bool enabled, const QString& device = {},
                                          int channel = 0);
    // sourceTrackId "": none. tap: kPostFader, kPreFader, kPreFx or a device id.
    Q_INVOKABLE bool trySetDeviceSidechain(const QString& trackId, const QString& deviceId,
                                           const QString& sourceTrackId, const QString& tap = QStringLiteral("post"));
    Q_INVOKABLE QString tryAddRackChain(const QString& trackId, const QString& rackId, int index = -1,
                                        const QString& name = {});
    Q_INVOKABLE bool tryMapMacro(const QString& trackId, const QString& rackId, int index, const QString& deviceId,
                                 const QString& paramId, double low = 0.0, double high = 1.0);

Q_SIGNALS:
    // (track id, device id) when the user adds a plug-in (not on undo or redo):
    // the UI opens its editor.
    void pluginAdded(const QString& trackId, const QString& deviceId);
    // (automation owner, target key) when the user changes a parameter that can
    // be automated, or takes hold of one (not on undo or redo): its automation
    // lane shows it.
    void parameterTouched(const QString& owner, const QString& key);
    // Why an edit wasn't made: it would change what a frozen track's audio
    // holds, or (the try* operations) it can't be made.
    void refused(const QString& message);

private:
    // Pushes a command onto the undo stack unless it changes what frozen audio
    // holds: then it is dropped and `refused` says why. Whether it was pushed.
    bool push(std::unique_ptr<QUndoCommand> command);
    // Runs `edit`, reporting an EditError on `refused`. Whether it went through.
    bool reportRefusal(const std::function<void()>& edit);

    // Tracks.
    QString insertTrack(Track track, int index, const TrackParent& parent, const QString& text);
    Device newDeviceOf(const QString& kind, const std::optional<PluginRef>& plugin = std::nullopt) const;
    QStringList roots(const QStringList& trackIds) const;
    TrackTree arranged(const QStringList& roots, int at, const std::optional<QString>& parent) const;
    void arrange(const TrackTree& tree, const QString& text, const QSet<QString>& going = {});
    bool validTree(const TrackTree& tree) const;
    QStringList insertCopies(const CopiedTracks& copied, int index, std::optional<QString> parent,
                             const QString& text);
    void setInput(const QString& trackId, const std::vector<int>& channels, const std::optional<QString>& sourceId);
    void dropInputs(const QSet<QString>& sourceIds, const QString& text);
    void dropSidechains(const QSet<QString>& sourceIds, const QString& text);
    Clip recordedMidiClip(const Track& track, const RecordedTake& take, double quantize) const;

    // Settings.
    void setSettings(const QString& text, const SettingsValues& values, const QString& mergeKey = {});

    // Clips.
    bool commitMoved(const QString& text, const QMap<QString, std::vector<Clip>>& after,
                     const QMap<LaneRef, Envelope>& envelopes, const QMap<QString, std::vector<Clip>>& frozen = {});
    struct Span {
        QString source;
        QString dest;
        double start;
        double end;
    };
    QMap<LaneRef, Envelope> carriedAutomation(const std::vector<Span>& spans, double deltaBeats, bool copyClips) const;
    QMap<LaneRef, Envelope> clearedAutomation(double start, double end, const QStringList& trackIds) const;

    // Devices.
    bool setDevices(const QString& trackId, const std::vector<Device>& before, std::vector<Device> after,
                    const QString& text);

    // Automation.
    void eachLane(const QString& text, const QList<LaneRef>& lanes, const std::function<Envelope(const Envelope&)>& change);
    bool laneExists(const LaneRef& lane) const;
    void updateView(const QString& owner, const std::function<void(AutomationView&)>& change);

    // Freezing.
    std::optional<QString> frozenProblem(const QUndoCommand& command) const;
    QStringList frozenRenders(const QStringList& trackIds) const;
    bool carriesFrozen(const QString& trackId, const QStringList& renders) const;
    bool coversFrozen(const QString& holder, const QStringList& trackIds) const;
    std::optional<QString> frozenAreaProblem(const QStringList& trackIds) const;
    std::vector<Clip> frozenSegments(const QString& trackId) const;
    std::pair<int, std::optional<QString>> outsideFrozen(int index, const std::optional<QString>& parent) const;
    std::optional<QString> heldProblem(const QStringList& trackIds) const;
    std::optional<QString> arrangementProblem(const TrackTree& tree, const QSet<QString>& going = {}) const;

    Project* project_;
    QUndoStack* undoStack_;
    ParamInfoSource describe_;
    OwnValueSource readOwn_;
    DeviceDefaults defaultDevice_;
};

}  // namespace sub::app
