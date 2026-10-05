#include "model/Clip.h"

#include "model/Timebase.h"

#include <algorithm>

namespace sub::app {

QString legacyWarpMode(const QString& name) {
    if (name == u"Beats") return QStringLiteral("Transients");
    if (name == u"Tones") return QStringLiteral("Standard");
    if (name == u"Complex") return QStringLiteral("Standard");
    if (name == u"Texture") return QStringLiteral("Smooth");
    if (name == u"Complex Pro") return QStringLiteral("Formants");
    return name;
}

Clip Clip::audio(const QString& id, const QString& path, const QString& name, double startBeat, double durationSec,
                 double offsetSec, double sourceDurationSec) {
    Clip clip;
    clip.kind = Kind::Audio;
    clip.id = id;
    clip.path = path;
    clip.name = name;
    clip.startBeat = startBeat;
    clip.durationSec = durationSec;
    clip.offsetSec = offsetSec;
    clip.sourceDurationSec = sourceDurationSec;
    return clip;
}

Clip Clip::midi(const QString& id, const QString& name, double startBeat, double durationBeats, double offsetBeats,
                std::vector<Note> notes) {
    Clip clip;
    clip.kind = Kind::Midi;
    clip.id = id;
    clip.name = name;
    clip.startBeat = startBeat;
    clip.durationBeats = durationBeats;
    clip.offsetBeats = offsetBeats;
    clip.notes = std::move(notes);
    return clip;
}

bool Clip::isWarped() const { return isAudio() && warp && segmentBpm > 0; }

double Clip::sourceTempo(double tempo) const { return isWarped() ? segmentBpm : tempo; }

double Clip::beatsToSource(double beats, double tempo) const { return beatsToSeconds(beats, sourceTempo(tempo)); }

double Clip::sourceToBeats(double seconds, double tempo) const { return secondsToBeats(seconds, sourceTempo(tempo)); }

double Clip::lengthBeats(double tempo) const {
    return isMidi() ? durationBeats : sourceToBeats(durationSec, tempo);
}

double Clip::endBeat(double tempo) const { return startBeat + lengthBeats(tempo); }

double Clip::windowEnd() const { return offsetBeats + durationBeats; }

double Clip::toTimeline(double contentBeat) const { return startBeat + contentBeat - offsetBeats; }

std::vector<PlayedNote> Clip::playedNotes() const {
    std::vector<PlayedNote> played;
    if (!isMidi()) return played;
    const double end = windowEnd();
    for (const Note& note : notes) {
        if (offsetBeats <= note.start && note.start < end) {
            played.push_back({toTimeline(note.start), toTimeline(std::min(note.end(), end)), note});
        }
    }
    return played;
}

}  // namespace sub::app
