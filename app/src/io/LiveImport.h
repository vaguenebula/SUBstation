#pragma once
// Importing an Ableton Live Set (Live 10 to 12): what of it SUBstation can
// have, as a project file's JSON (Serialization.h's loadInto reads it, with the
// repairs it makes of any project file), and what didn't come across, for the
// user. The set is read by LiveSet.h.
//
// What comes across:
// - the tempo (if it is automated, its value at the song's start), the time
//   signature, the loop and the key (Live 12's scale, if it is major or minor);
// - the arrangement's tracks: audio, MIDI and group tracks (nested and folded
//   as in Live), return tracks and the main track, with their names (Live's
//   numbered default names as SUBstation's: "4-Serum" is "# Serum"), colours,
//   volume, pan, activator, solo, sends (pre or post), where their audio goes
//   (the main track past their group, another track, nowhere) and an audio
//   track's input (device channels, another track before or after its effects
//   or mixer, the main track);
// - arrangement clips. MIDI clips with their notes (deactivated ones too).
//   Audio clips with their files (found beside the set if it moved, else where
//   the set says: the File Manager finds the missing ones), start marker,
//   length, warp mode, transpose, detune, gain and fades. A clip that loops is
//   written out loop after loop (SUBstation's clips don't loop). A warped clip
//   is a clip for each stretch between its warp markers that plays at a tempo
//   of its own (SUBstation's clips have one tempo each: two markers make one
//   clip). Deactivated clips stay deactivated;
// - devices: VST3 plug-ins with their settings (Live's processor and controller
//   states, as a .vstpreset); a VST2 plug-in as the installed VST3 of the same
//   name, with its settings if that VST3 is made to replace it (its class id is
//   Steinberg's for a VST2: "VST" + the VST2's id + its name), else at its
//   defaults. Utility, EQ Eight (its L/R and M/S modes too), Compressor (with
//   its sidechain), Delay, Simpler and a Sampler of one sample as SUBstation's
//   Utility, EQ, Compressor, Delay and Sampler. Audio Effect and Instrument
//   Racks as racks, their chains with their mixers (not their macros). A Drum
//   Rack as a group track with a MIDI track for each pad the clips play (named
//   after the pad, its devices: its Simpler a Sampler), each holding the clips'
//   notes of its pad, as the pad sends them on (a sidechain from a pad comes
//   from its track);
// - automation of the mixers (volume, pan, activator, sends), of devices'
//   on/off, of plug-ins' parameters and of the built-in devices' parameters
//   that came across.
//
// What doesn't (said in the notes, with how many): other Live devices, MIDI
// effects, Max for Live devices, VST2 plug-ins with no VST3, racks' macros,
// Drum Racks' return chains, tempo and time signature changes, clip envelopes,
// grooves, frozen tracks' frozen audio (they come unfrozen). The Session View
// isn't read.

#include "plugins/PluginInfo.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <vector>

namespace sub::app::live {

class Element;

struct ImportOptions {
    // The VST3 plug-ins installed (the plug-in index's): plug-in devices find
    // their files by them, and VST2 devices become the VST3 of the same name.
    std::vector<PluginInfo> plugins;
};

struct ImportResult {
    QJsonObject project;  // as a project file holds it
    QStringList notes;    // what didn't come across as it was, a line each
    int tracks = 0;       // tracks made (returns not counted)
    int clips = 0;        // clips made
};

// The project a Live Set makes. `root`: readLiveSet()'s; `setPath`: its file
// (samples are looked for relative to it too).
ImportResult importLiveSet(const Element& root, const QString& setPath, const ImportOptions& options = {});

// A VST3 plug-in's settings as Live keeps them (its processor's and its
// controller's states) as SUBstation keeps them: a .vstpreset's bytes, for the
// class whose id the .vstpreset spells out (`classId`, 32 hex digits).
QByteArray vstPreset(const QString& classId, const QByteArray& processorState, const QByteArray& controllerState);
// A VST2 plug-in's settings as a VST3 that replaces it takes them (Steinberg's
// VST 2 compatibility: "VstW", then the VST2's own bank or program chunk).
QByteArray vst2CompatibleState(const QByteArray& chunk, quint32 vst2Id, quint32 version, bool bank, int programs);

}  // namespace sub::app::live
