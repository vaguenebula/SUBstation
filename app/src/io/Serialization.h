#pragma once
// Project files: JSON (.gilproj). Clip paths are stored both absolute and
// relative to the project file, so a project folder can be moved. MIDI tracks
// store their clips' notes inline, as [pitch, start, length, velocity]. Plug-in
// devices store which plug-in they are and its state (a .vstpreset, base64).
// Automation is stored per track (and for the master) by target key, each
// envelope as [beat, value, curve] points, with what the arrangement shows of
// it. The master is stored apart from the tracks: its mixer, devices and
// automation. Files from before version 5 have no master devices; they load as
// they were. Tracks store their audio input (device channels), monitoring and
// whether they are armed (version 6; older files load with none, Auto, not
// armed), and MIDI tracks their MIDI input (version 7; older ones load hearing
// every input). Tracks store the group they are in ("parent") and whether they
// are folded (version 8); a track that can't be in its group (the file was
// edited) loads out of it. Return tracks are stored apart from the tracks
// ("returns"), and every track (and return) its sends, by return id (version 9;
// older files have none). Sends to a return that isn't there, or that would
// close a cycle, are dropped. Audio tracks store the track whose output they
// take as their input ("input_track", or kMaster: resampling; version 10); one
// that isn't there, or that would close a cycle, is dropped. Devices store their
// sidechain (the track and where it is tapped; version 11); one from a track
// that isn't there, or that would close a cycle, is dropped. Racks store their
// chains (each with its devices and mixer) and macro mappings (version 12; a
// mapping to a device not in its rack is dropped). The devices shown folded are
// stored by id ("folded_devices"; files without it load with none folded). Racks
// store their own name, if they have one (the preset's they were saved as or
// loaded from; version 13; older files have none: racks are named by their
// kind). Frozen tracks (and returns) store their frozen audio ("frozen": its
// file, absolute and relative, its length and the tempo it was rendered at;
// version 14), and what of it plays once a time selection over its track was
// edited ("segments": each one's id, start beat, offset and length in seconds;
// version 16; older files have none: all of it plays). Reversed audio clips
// store the file they were reversed from ("reversed_from", absolute and
// relative; version 15). Racks store how many macros they have and their names
// ("macro_names", "" for a macro named by its number; version 18; older racks
// had eight: they load with those they use, mapped or turned, and at least
// kDefaultMacroCount). The racks whose chain list shows ("chain_lists_shown")
// and those whose chain's devices don't ("rack_devices_hidden") are stored by
// id, as folded devices are (files without them show no chain list, and every
// rack's devices). Deactivated clips (audio and MIDI) store "muted", and
// deactivated notes a fifth value, true (version 20; older files have none:
// every clip and note plays). Tracks (and returns) store where their output goes
// unless into their group ("output": {"to": "master" | "none"}, {"to": "track",
// "track": id}, {"to": "sidechain", "device": id}), and audio tracks where their
// input from a track is tapped unless after its fader ("input_tap": "pre" or
// "pre-fx"; version 21; older files have neither: every track goes into its
// group, inputs are taken after the fader). An output into a track that isn't
// an audio track there, into a device that isn't there, or closing a cycle goes
// into its group. A bent note stores its bend as a sixth value (its fifth then
// written, false unless it is deactivated): {"bend": [[time, semitones, curve],
// ...], "vibrato": [[start, length, depth, rate, fade], ...]}; a device taking
// another track's notes stores that track ("midi_from"; version 22; older files
// have neither: no note bends, every device hears its own track). A MIDI input
// from a track that isn't a MIDI track there is dropped.
//
// There is no per-version migration code: each addition has a default that
// makes an older file load as it was, and saving writes the current version.
// Loading is tolerant: missing fields take their defaults, values are clamped,
// unknown automation targets are dropped, and routing a project can't have is
// repaired (repairTree, repairRouting). Earlier warp mode names load as the mode
// that plays the same way. The keys and structure are the Python version's, so
// either version reads the other's files.
//
// Presets: a device (a rack with everything in it too) on its own, in a file of
// its own (deviceToPreset, presetDevice): what the project file stores of it. A
// preset loads as new devices (new ids), without sidechains or MIDI inputs (they
// name tracks of the project it came from). A rack loaded from a preset file is named as the
// file is.
//
// Reading throws ProjectFileError (model/Errors.h) with a message for the user:
// not a project (or preset), from a newer version, damaged, or unreadable. A
// file that can't be written throws it too. A project is only replaced once its
// file has been read whole, so a damaged file leaves the open project as it was.

#include "model/Device.h"
#include "model/Track.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include <vector>

namespace sub::app {

class Project;

inline const QString kProjectFormat = QStringLiteral("gilstudio-project");
// 2: MIDI tracks, 3: plug-ins, 4: automation and master pan, 5: master devices,
// 6: inputs, 7: MIDI inputs, 8: group tracks, 9: return tracks and sends, 10:
// inputs from tracks (resampling), 11: sidechains, 12: racks, 13: rack names, 14:
// frozen tracks, 15: reversed clips, 16: frozen audio's segments, 17: clip fades,
// 18: racks' macros (how many, their names), 19: track names as templates (# the
// track's number; kNameTemplatesVersion), 20: deactivated clips and notes, 21:
// tracks' outputs and where inputs from tracks are tapped, 22: notes' bends
// (MIDI 2.0's per-note pitch bend: points and vibratos) and devices' MIDI inputs
// from other tracks
inline constexpr int kProjectVersion = 22;
inline constexpr int kNameTemplatesVersion = 19;
inline const QString kPresetFormat = QStringLiteral("gilstudio-preset");
inline constexpr int kPresetVersion = 1;
inline const QString kPresetExtension = QStringLiteral(".gilpreset");
inline const QString kProjectExtension = QStringLiteral(".gilproj");

// The project as its file holds it; paths relative to `projectFile`'s folder
// too ("": none, an unsaved project).
QJsonObject projectToJson(const Project& project, const QString& projectFile = {});
// Replaces the project with what `data` holds (read from `projectFile`, to
// find files moved with it).
void loadInto(Project& project, const QJsonObject& data, const QString& projectFile = {});
// The arrangement's tracks and the return tracks a project file holds (the
// tracks' groups repaired; not their routing: see repairRouting).
std::vector<Track> tracksFromJson(const QJsonObject& data, const QString& projectFile = {});
std::vector<Track> returnsFromJson(const QJsonObject& data, const QString& projectFile = {});
// Drops the sends, outputs, inputs and sidechains a project can't have (the
// file was edited): to a return (or from a track, into a track or a device)
// that isn't there, and those closing a cycle (the later ones; sends first,
// then outputs, then inputs).
void repairRouting(std::vector<Track>& tracks, std::vector<Track>& returns, Track* master = nullptr);

// A device as the project file stores it, and back.
QJsonObject deviceToJson(const Device& device);
Device deviceFromJson(const QJsonValue& data);

// A device as a preset: what the project file stores of it (a rack with its
// chains, the devices in them and its macros; plug-ins' states as last stored
// in the model), without its sidechains.
QJsonObject deviceToPreset(const Device& device);
// A preset's device, new: fresh ids for it and everything in it.
Device presetDevice(const QJsonValue& data);
void savePreset(const Device& device, const QString& path);
Device loadPreset(const QString& path);

// Never leaves a half-written file behind. Sets the project's path.
void saveProject(Project& project, const QString& path);
void loadProject(Project& project, const QString& path);

// A template: the project as its file holds it (its files by absolute path),
// written to `path` (its folder made) without becoming the project's file; and
// a new project from one, everything in it as it was saved, untitled (no path).
void saveTemplate(const Project& project, const QString& path);
void loadTemplate(Project& project, const QString& path);

}  // namespace sub::app
