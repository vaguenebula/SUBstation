#pragma once
// Pure clip-editing functions (no undo): easy to test in isolation.
//
// They edit audio and MIDI clips alike: each is a window onto its content (the
// audio file, the notes), and editing moves the window's edges while the
// content stays where it is on the timeline (but stretchClip and slipClip,
// which move the content). An audio clip's fades stay at its ends: a piece cut
// from inside it has none there, and they are held to its length (fitFades).

#include "model/Clip.h"

#include <QSet>
#include <QString>

#include <optional>
#include <utility>
#include <vector>

namespace sub::app::edits {

inline constexpr double kMinClipSec = 0.005;
inline constexpr double kMinMidiClipBeats = 1.0 / 64;
inline constexpr double kEps = 1e-9;

using Interval = std::pair<double, double>;  // [start, end) in beats

// The parts of [start, end) not covered by any cut interval.
std::vector<Interval> subtractIntervals(double start, double end, const std::vector<Interval>& cuts);
// Ableton's overlap rule: clips in `winners` keep their place, and every other
// clip on the track is trimmed, split or removed where a winner covers it.
std::vector<Clip> resolveOverlaps(const std::vector<Clip>& clips, const QSet<QString>& winners, double tempo);
// The parts of `clip` outside every cut (beat intervals): nothing, the clip
// itself, a trimmed clip, or pieces. The first piece keeps the clip's id; every
// piece still plays the same audio (or notes) at the same place on the timeline.
std::vector<Clip> cutClip(const Clip& clip, const std::vector<Interval>& cuts, double tempo);
// Delete everything between two beats: whole clips inside go, clips across an
// edge are trimmed, and a clip spanning the range keeps its start and its end.
std::vector<Clip> removeRange(const std::vector<Clip>& clips, double start, double end, double tempo);
// Unwarped clips keep their length in seconds, so a faster tempo makes them
// longer in beats (and a warped clip grows when its segment BPM drops). Trim any
// clip that would run into the next one (the later clip keeps its place) so
// clips never overlap. The same clips if nothing changes (`changed`, if given,
// says whether anything did).
std::vector<Clip> fitToTempo(const std::vector<Clip>& clips, double tempo, bool* changed = nullptr);
// New clips (fresh ids) holding just the parts of `clips` between two beats. A
// clip wholly inside is taken as it is (with its own id if `keepIds`).
std::vector<Clip> sliceRange(const std::vector<Clip>& clips, double start, double end, double tempo,
                             bool keepIds = false);
// Split into two clips at `atBeat`; none if the point is not inside the clip.
std::optional<std::pair<Clip, Clip>> splitClip(const Clip& clip, double atBeat, double tempo);
// Move the left edge. The audio (or notes) stay in place on the timeline.
Clip trimStart(const Clip& clip, double newStartBeat, double tempo);
// Move the right edge, limited by the end of the source file (MIDI clips can grow freely).
Clip trimEnd(const Clip& clip, double newEndBeat, double tempo);
// Stretch the clip by an edge (Alt-dragging it): that edge (`left`: the start)
// goes to `edgeBeat`, the other stays, and the clip plays the same audio (or
// notes) faster or slower to fill the new length. An audio clip is warped to
// do it, its segment BPM set (kMinSegmentBpm..kMaxSegmentBpm limit how far it
// stretches); a MIDI clip's notes are scaled with it. It never starts before
// beat 0.
Clip stretchClip(const Clip& clip, double edgeBeat, bool left, double tempo);
// Slide the clip's content by `deltaBeats` (Ctrl+Shift-dragging it): the clip
// stays where it is, as long as it is, and plays what is `deltaBeats` earlier
// (or, negative, later) in its audio or notes. Audio stops at the ends of its
// file; notes can go anywhere (revealing time before the first content beat
// moves them along, as trimStart does).
Clip slipClip(const Clip& clip, double deltaBeats, double tempo);
// An audio clip with its fade in (`out`: its fade out) `beats` long, held to
// what its other fade leaves of it; a MIDI clip as it is.
Clip fadeClip(const Clip& clip, bool out, double beats, double tempo);
// ... and that fade's curve (-1..1; none while the fade is).
Clip curveFade(const Clip& clip, bool out, double curve);
// `clip` playing `path` (its file reversed, `totalSec` long) instead: the same
// stretch of audio, backwards, in the same place on the timeline. Going back to
// the file it was reversed from forgets it; otherwise it remembers its file, to
// go back to.
Clip reverseClip(const Clip& clip, const QString& path, double totalSec);
// Whether an audio clip plays all of its file (as dropped in), not a stretch of it.
bool playsWholeFile(const Clip& clip);
// `clip` playing another file (`path`, `totalSec` long) in its place, where it
// is, with its settings (warp, pitch, gain, pan, fades, activation): a clip
// that played all of its file plays all of the new one (a longer kick, longer);
// one that played a stretch of it plays the same stretch of the new one, as
// far as the new one goes (from its start, if it doesn't go that far). It is
// named after the new file, and plays it forwards (a reversed clip isn't
// reversed any more). A MIDI clip as it is.
Clip replaceFile(const Clip& clip, const QString& path, double totalSec);
// `clip` with a file found somewhere else: `to` wherever it played `from`, or
// was reversed from it (paths compared as the system compares them). Nothing
// else changes: it is the same audio.
Clip relinkFile(const Clip& clip, const QString& from, const QString& to);
// Whether two paths are the same file: absolute and clean, and on Windows in any case.
bool samePath(const QString& a, const QString& b);
// (earliest start, latest end) of some clips (there must be some).
std::pair<double, double> selectionSpan(const std::vector<Clip>& clips, double tempo);
// Ableton's Consolidate (Ctrl+J): one MIDI clip from the first clip's start to
// the last one's end, holding just the notes the clips play, where they play
// them (cut at their clip's end). A deactivated clip's notes come deactivated,
// unless every clip is (the joined clip is deactivated then, its notes as they
// were). It keeps the first clip's id and name.
Clip consolidateMidi(const std::vector<Clip>& clips);

}  // namespace sub::app::edits
