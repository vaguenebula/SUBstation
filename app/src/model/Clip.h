#pragma once
// Clips and notes.
//
// A Clip is an audio clip or a MIDI clip (kind); both are values, and an edit
// makes a new one (copy it and set what changes). Each is a window onto its
// content (the audio file, the notes), and editing moves the window's edges
// while the content stays where it is on the timeline. Its methods behave per
// kind, so both kinds of clip are edited alike (Edits.h).
//
// An audio clip: `offsetSec` and `durationSec` measure the source audio it
// plays. Unwarped, that plays at its own speed, so the clip's length in beats
// follows the tempo. Warped, the audio is taken to be at `segmentBpm` and is
// stretched to the project tempo, so its length in beats is fixed (as in
// Ableton). A reversed clip plays a reversed copy of a file (`path`);
// `reversedFrom` is the file it was made from, which reversing it again goes
// back to. Its fades (`fadeInSec`, `fadeOutSec`) are measured in its audio
// too, so they stretch with it; each bends by its curve (-1..1, 0 a straight
// line, positive bulging up: as automation's, automation::shape()). They never
// overlap (fitFades()). Without one, the engine fades an edge that cuts into
// the file for a few milliseconds against clicks, but not one at the file's own
// start or end (a one-shot's attack stays as it is).
//
// A MIDI clip: a window onto its notes, as an audio clip is onto its file.
// Content beat `offsetBeats` plays at `startBeat`. Notes outside the window are
// kept but not played, so trimming or splitting a clip never loses notes. MIDI
// is measured in beats, so a clip's length doesn't follow the tempo (`tempo`
// arguments are accepted and ignored, so both kinds of clip can be edited alike).
// MIDI clips have no name of their own: the arrangement shows their title bars
// empty (a name in a project saved earlier is dropped on load).
//
// A clip of either kind can be deactivated (`muted`, Ableton's clip activator
// switched off): it stays where it is, is edited, copied and saved like any
// other, but doesn't play (nor count towards the song's chords): plays(),
// heardNotes(). Its pieces (split, trimmed, copied) are deactivated too. So
// can single notes of a MIDI clip (Note::muted).

#include <QString>
#include <QStringList>

#include <vector>

namespace sub::app {

// Warp modes, in the engine's order (sub::WarpMode).
inline const QStringList kWarpModes{QStringLiteral("Transients"), QStringLiteral("Standard"),
                                    QStringLiteral("Smooth"), QStringLiteral("Formants"),
                                    QStringLiteral("Re-Pitch")};
inline const QString kDefaultWarpMode = QStringLiteral("Standard");
// The segment BPMs a clip can have (the clip view's box, and stretching).
inline constexpr double kMinSegmentBpm = 20.0;
inline constexpr double kMaxSegmentBpm = 999.0;
// Names used by earlier versions, mapped to the mode that plays the same way.
// (Unknown: the name itself.)
QString legacyWarpMode(const QString& name);

// A breakpoint of a note's bend (MIDI 2.0's per-note pitch bend): `time` in
// beats from the note's start, `semitones` from its pitch, and how the segment
// from it to the next point bends (-1..1, as an automation breakpoint's curve).
struct BendPoint {
    double time = 0.0;
    double semitones = 0.0;
    double curve = 0.0;

    friend bool operator==(const BendPoint&, const BendPoint&) = default;
};

// Vibrato over a stretch of a note, on top of its bend: from `start` (beats
// from the note's start) for `length` beats, swinging `depth` semitones either
// way `rate` times a second, swelling in over the first `fade` (0..1) of it.
struct Vibrato {
    double start = 0.0;
    double length = 0.0;
    double depth = 0.5;
    double rate = 5.5;
    double fade = 0.3;

    friend bool operator==(const Vibrato&, const Vibrato&) = default;
};

// A MIDI note. Times are in beats from the start of its clip's content. A
// deactivated note (`muted`, 0 in the piano roll, as in Ableton) is kept and
// edited like any other but isn't heard (Clip::heardNotes).
//
// Its bend is a curve of points from its start (sorted by time), and vibratos
// on top of it: it starts at the note's pitch, goes through the points, then
// holds the last one's value; a vibrato swells in and dies away on the curve.
// How it sounds is the engine's sub::bend (engine/src/NoteBend.h): bendAt().
// Its times are the note's own, so it moves with the note; trimming the note's
// start keeps it where it was in time, and points or vibratos past its end are
// kept but not heard.
struct Note {
    int pitch = 60;  // MIDI note number, 60 = C3
    double start = 0.0;
    double length = 0.0;
    int velocity = 100;  // 1..127
    bool muted = false;  // deactivated: silent
    std::vector<BendPoint> bend;    // sorted by time
    std::vector<Vibrato> vibrato;   // in time order, not overlapping

    double end() const { return start + length; }
    // It has a bend (points or vibrato) to draw and play.
    bool bent() const { return !bend.empty() || !vibrato.empty(); }
    // Its bend in semitones `beat` beats from its start, at `tempo` (the
    // vibratos' rates are in cycles a second); curveAt(): its points' curve
    // alone, what the vibratos swing around.
    double bendAt(double beat, double tempo) const;
    double curveAt(double beat) const;

    friend bool operator==(const Note&, const Note&) = default;
};

// A note a MIDI clip plays, on the timeline.
struct PlayedNote {
    double start = 0.0;  // timeline beats
    double end = 0.0;
    Note note;

    friend bool operator==(const PlayedNote&, const PlayedNote&) = default;
};

struct Clip {
    enum class Kind { Audio, Midi };

    Kind kind = Kind::Audio;
    QString id;
    QString name;  // an audio clip's (its file's name, as it was placed); none for a MIDI clip
    double startBeat = 0.0;
    bool muted = false;  // deactivated: silent (both kinds)

    // An audio clip's.
    QString path;
    double durationSec = 0.0;
    double offsetSec = 0.0;
    double sourceDurationSec = 0.0;
    double gainDb = 0.0;
    QString reversedFrom;
    // Clip view settings.
    bool warp = false;
    QString warpMode = kDefaultWarpMode;  // one of kWarpModes
    double segmentBpm = 0.0;  // tempo of the source audio; 0: not set, shown as the project tempo
    int transpose = 0;  // semitones
    double detune = 0.0;  // cents
    double pan = 0.0;
    // Fades, in seconds of source audio from each end; their curves -1..1.
    double fadeInSec = 0.0;
    double fadeOutSec = 0.0;
    double fadeInCurve = 0.0;
    double fadeOutCurve = 0.0;

    // A MIDI clip's.
    double durationBeats = 0.0;
    double offsetBeats = 0.0;
    std::vector<Note> notes;  // sorted by start, then pitch

    // An audio clip playing `durationSec` of `path` from `startBeat`.
    static Clip audio(const QString& id, const QString& path, const QString& name, double startBeat,
                      double durationSec, double offsetSec = 0.0, double sourceDurationSec = 0.0);
    // A MIDI clip.
    static Clip midi(const QString& id, const QString& name, double startBeat, double durationBeats,
                     double offsetBeats = 0.0, std::vector<Note> notes = {});

    bool isMidi() const { return kind == Kind::Midi; }
    bool isAudio() const { return kind == Kind::Audio; }
    // Whether it is heard: not deactivated (what the engine plays, and the song's chords count).
    bool plays() const { return !muted; }

    // Warped: warp on and a segment BPM set (MIDI clips never are).
    bool isWarped() const;
    // The tempo at which this clip's audio maps onto beats: its segment BPM when
    // warped, otherwise the project tempo (it plays at its own speed).
    double sourceTempo(double tempo) const;
    // Seconds of source audio covered by `beats` of this clip.
    double beatsToSource(double beats, double tempo) const;
    double sourceToBeats(double seconds, double tempo) const;
    double lengthBeats(double tempo = 0.0) const;
    double endBeat(double tempo = 0.0) const;
    // An audio clip's fades, in beats (0 for a MIDI clip).
    double fadeInBeats(double tempo) const { return isAudio() ? sourceToBeats(fadeInSec, tempo) : 0.0; }
    double fadeOutBeats(double tempo) const { return isAudio() ? sourceToBeats(fadeOutSec, tempo) : 0.0; }
    // Its fades held to its length: where together they are longer than the
    // clip, both shortened in proportion so they meet. A MIDI clip's are none.
    void fitFades();
    // The gain a fade of curve `curve` gives at `x` (0 where it is silent, 1 where
    // it is done).
    static double fadeGain(double x, double curve);

    // A MIDI clip's content beat at its end.
    double windowEnd() const;
    double toTimeline(double contentBeat) const;
    // (timeline start, timeline end, note) for each note the clip plays: those
    // starting inside its window, cut at the clip's end (as in Ableton).
    // Nothing for an audio clip.
    std::vector<PlayedNote> playedNotes() const;
    // The notes it is heard playing: playedNotes() but deactivated ones, none
    // while the clip is deactivated.
    std::vector<PlayedNote> heardNotes() const;

    friend bool operator==(const Clip&, const Clip&) = default;
};

}  // namespace sub::app
