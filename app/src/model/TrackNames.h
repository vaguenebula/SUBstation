#pragma once
// Track names as Ableton's: a track's name is made from its nameTemplate, in
// which # stands for its number, its place in the arrangement from the top
// (from 1; every track counts, groups too): "# Kick" shows "3 Kick", and is
// "4 Kick" once a track goes in above it. Project keeps each track's name so.
//
// New audio and MIDI tracks are named by what they hold: an audio track after
// its clips' files (the one most of them play), a MIDI track after its
// instrument ("# Kick", "# Synth"; "# Audio", "# MIDI" while they hold none).
// The name follows what the track holds while it is the name that gives
// (Project changes it with its clips and devices, so undo restores it too);
// renamed to anything else, it keeps that name.

#include <QString>

namespace sub::app {

struct Track;

// What stands for a track's number in its nameTemplate.
inline constexpr char16_t kNumberMark = u'#';

// A name made from a template: each # the number.
QString numberedName(const QString& nameTemplate, int number);
// What names an audio or MIDI track: the stem of the file most of its audio
// clips play (a reversed clip's original; a take's without its time; a tie:
// the one played first), or its first instrument's name (deviceName); "Audio"
// or "MIDI" while there is none. Empty for other tracks.
QString contentsLabel(const Track& track);
// "# <its contentsLabel>": an audio or MIDI track's template while it is named by what it holds.
QString contentsName(const Track& track);
// Whether the track's name follows what it holds: an audio or MIDI track whose template is contentsName().
bool namedByContents(const Track& track);
// Whether it has a name new tracks had before they were numbered by place
// ("3 Audio" on an audio track, "3 MIDI" on a MIDI one, "3 Group" on a group):
// such a track is numbered by its place (and named by what it holds) once loaded.
bool hasPlainName(const Track& track);
// What the template of such a track is now: contentsName(), "# Group".
QString plainNameTemplate(const Track& track);
// What a take recorded on the track is called: its contentsLabel if its name
// follows it ("Kick", so the take names it as before), else its name.
QString takeName(const Track& track);

}  // namespace sub::app
