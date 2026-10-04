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
// version 14). Reversed audio clips store the file they were reversed from
// ("reversed_from", absolute and relative; version 15).
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
// preset loads as new devices (new ids), without sidechains (they name tracks of
// the project it came from). A rack loaded from a preset file is named as the
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
// frozen tracks, 15: reversed clips
inline constexpr int kProjectVersion = 15;
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
// Drops the sends, inputs and sidechains a project can't have (the file was
// edited): to a return (or from a track) that isn't there, and those closing a
// cycle (the later ones; sends first, then inputs).
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

}  // namespace sub::app
