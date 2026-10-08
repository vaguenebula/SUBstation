#include "audio/EngineDescs.h"

#include "model/Timebase.h"

namespace sub::app {

sub::ClipDesc clipDesc(const Clip& clip) {
    sub::ClipDesc desc;
    desc.path = clip.path.toStdString();
    desc.startBeat = clip.startBeat;
    desc.durationSec = clip.durationSec;
    desc.offsetSec = clip.offsetSec;
    desc.gain = static_cast<float>(dbToGain(clip.gainDb));
    desc.pan = static_cast<float>(clip.pan);
    desc.warp = clip.isWarped();
    desc.segmentBpm = clip.segmentBpm;
    // The engine's modes are in kWarpModes' order; an unknown one plays as Standard.
    const qsizetype mode = kWarpModes.indexOf(clip.warpMode);
    desc.warpMode = mode >= 0 ? static_cast<sub::WarpMode>(mode) : sub::WarpMode::Standard;
    desc.transpose = clip.transpose + clip.detune / 100.0;
    desc.id = clip.id.toStdString();
    desc.fadeInSec = clip.fadeInSec;
    desc.fadeOutSec = clip.fadeOutSec;
    desc.fadeInCurve = static_cast<float>(clip.fadeInCurve);
    desc.fadeOutCurve = static_cast<float>(clip.fadeOutCurve);
    return desc;
}

std::vector<sub::ClipDesc> clipDescs(const std::vector<Clip>& clips) {
    std::vector<sub::ClipDesc> descs;
    for (const Clip& clip : clips) {
        if (clip.plays()) descs.push_back(clipDesc(clip));
    }
    return descs;
}

std::vector<sub::NoteDesc> noteDescs(const Track& track) { return clipNoteDescs(track.clips); }

std::vector<sub::NoteDesc> clipNoteDescs(const std::vector<Clip>& clips) {
    std::vector<sub::NoteDesc> notes;
    for (const Clip& clip : clips) {
        for (const PlayedNote& played : clip.heardNotes()) {
            notes.push_back({played.start, played.end - played.start, played.note.pitch, played.note.velocity});
        }
    }
    return notes;
}

}  // namespace sub::app
