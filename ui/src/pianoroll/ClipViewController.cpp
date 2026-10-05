#include "pianoroll/ClipViewController.h"

#include "editor/ProjectEditor.h"
#include "model/Errors.h"
#include "model/Numbers.h"
#include "model/Project.h"
#include "model/Timebase.h"
#include "pianoroll/PianoRoll.h"

#include <algorithm>
#include <stdexcept>

namespace sub::ui {

namespace {

const QString kTransposeTip = QStringLiteral("Pitch shift in semitones; the speed stays the same");
const QString kDetuneTip = QStringLiteral("Fine pitch shift in cents");
const QString kRepitchNote =
    QStringLiteral("Re-Pitch: the pitch follows the speed, so Transpose and Detune have no effect.");
const QString kDiffer = QStringLiteral("Clips differ: changes move them all by the same amount.");
const QString kRePitch = QStringLiteral("Re-Pitch");

// A knob: the clip setting it moves, its range and look, and its undo text.
struct KnobSpec {
    const char* name;
    const char* caption;
    double from;
    double to;
    double defaultValue;
    bool bipolar;
    bool integer;  // whole numbers (semitones)
    const char* text;
    QString tooltip;
};

const std::vector<KnobSpec>& knobSpecs() {
    static const std::vector<KnobSpec> specs{
        {"transpose", "Transpose", -48.0, 48.0, 0.0, true, true, "Transpose Clips", kTransposeTip},
        {"detune", "Detune", -50.0, 50.0, 0.0, true, false, "Detune Clips", kDetuneTip},
        {"gain", "Gain", -70.0, 24.0, 0.0, false, false, "Change Clip Gain",
         QStringLiteral("Clip gain: how loud the clip plays (its waveform grows with it)")},
        {"pan", "Pan", -1.0, 1.0, 0.0, true, false, "Change Clip Pan", QString()},
    };
    return specs;
}

const KnobSpec* knobSpec(const QString& name) {
    for (const KnobSpec& spec : knobSpecs()) {
        if (name == QLatin1String(spec.name)) return &spec;
    }
    return nullptr;
}

double knobValue(const app::Clip& clip, const QString& name) {
    if (name == u"transpose") return clip.transpose;
    if (name == u"detune") return clip.detune;
    if (name == u"gain") return clip.gainDb;
    return clip.pan;
}

app::Clip withKnobValue(app::Clip clip, const QString& name, double value) {
    if (name == u"transpose")
        clip.transpose = static_cast<int>(value);
    else if (name == u"detune")
        clip.detune = value;
    else if (name == u"gain")
        clip.gainDb = value;
    else
        clip.pan = value;
    return clip;
}

// round(value, 2), as Python rounds it.
double round2(double value) { return app::formatFixed(value, 2).toDouble(); }

}  // namespace

ClipViewController::ClipViewController(QObject* parent) : QObject(parent) { refresh(); }

ClipViewController::~ClipViewController() = default;

app::Project* ClipViewController::project() const { return session_ ? session_->project() : nullptr; }

void ClipViewController::setSession(app::Session* session) {
    if (session == session_) return;
    if (session_) disconnect(session_->project(), nullptr, this, nullptr);
    session_ = session;
    if (session) {
        app::Project* p = session->project();
        connect(p, &app::Project::clipsChanged, this, [this](const QString& trackId) {
            for (const app::ClipRef& ref : refs_) {
                if (ref.trackId == trackId) {
                    dropMissing();
                    return;
                }
            }
        });
        connect(p, &app::Project::settingsChanged, this, &ClipViewController::refresh);
        connect(p, &app::Project::trackChanged, this, &ClipViewController::refresh);
        connect(p, &app::Project::reset, this, &ClipViewController::close);
        connect(p, &app::Project::trackRemoved, this, &ClipViewController::dropMissing);
    }
    Q_EMIT sessionChanged();
    reopen();
}

void ClipViewController::setPianoRoll(PianoRoll* roll) {
    if (roll == pianoRoll_) return;
    if (pianoRoll_) pianoRoll_->setClip({}, {});
    pianoRoll_ = roll;
    if (roll && midi_ && !refs_.isEmpty()) roll->setClip(refs_.front().trackId, refs_.front().clipId);
    Q_EMIT pianoRollChanged();
}

void ClipViewController::setTrackId(const QString& trackId) {
    if (trackId == trackId_) return;
    trackId_ = trackId;
    Q_EMIT requestChanged();
    reopen();
}

void ClipViewController::setClipIds(const QVariantList& clipIds) {
    if (clipIds == clipIds_) return;
    clipIds_ = clipIds;
    Q_EMIT requestChanged();
    reopen();
}

void ClipViewController::setLeadClipId(const QString& clipId) {
    if (clipId == leadClipId_) return;
    leadClipId_ = clipId;
    Q_EMIT requestChanged();
    reopen();
}

void ClipViewController::setActive(bool active) {
    if (active == active_) return;
    active_ = active;
    Q_EMIT activeChanged();
    if (active)
        reopen();
    else
        forget();
}

void ClipViewController::reopen() {
    if (!active_) return;
    app::ClipRefs refs;
    for (const QVariant& entry : clipIds_) {
        if (entry.canConvert<QVariantMap>() && entry.typeId() != QMetaType::QString) {
            const QVariantMap map = entry.toMap();
            refs.append({map.value(QStringLiteral("trackId")).toString(),
                         map.value(QStringLiteral("clipId")).toString()});
        } else {
            refs.append({trackId_, entry.toString()});
        }
    }
    std::optional<app::ClipRef> lead;
    for (const app::ClipRef& ref : refs) {
        if (!leadClipId_.isEmpty() && ref.clipId == leadClipId_) {
            lead = ref;
            break;
        }
    }
    open(refs, lead);
}

std::vector<ClipViewController::Shown> ClipViewController::existing(const app::ClipRefs& refs) const {
    std::vector<Shown> result;
    const app::Project* p = project();
    if (!p) return result;
    for (const app::ClipRef& ref : refs) {
        if (const app::Clip* clip = p->findClip(ref.trackId, ref.clipId)) result.push_back({ref.trackId, clip});
    }
    return result;
}

void ClipViewController::open(const app::ClipRefs& refs, const std::optional<app::ClipRef>& lead) {
    std::vector<Shown> clips = existing(refs);
    baseline_.reset();
    if (clips.empty()) {
        forget();  // nothing to show (yet): an empty view
        return;
    }
    const app::Project* p = project();
    std::stable_sort(clips.begin(), clips.end(), [p](const Shown& a, const Shown& b) {
        const int ia = p->trackIndex(a.trackId), ib = p->trackIndex(b.trackId);
        if (ia != ib) return ia < ib;
        return a.clip->startBeat < b.clip->startBeat;
    });
    Shown first = clips.front();
    for (const Shown& shown : clips) {
        if (lead && shown.trackId == lead->trackId && shown.clip->id == lead->clipId) {
            first = shown;
            break;
        }
    }
    midi_ = first.clip->isMidi();
    refs_.clear();
    if (midi_) {
        refs_.append({first.trackId, first.clip->id});
    } else {
        for (const Shown& shown : clips) {
            if (!shown.clip->isMidi()) refs_.append({shown.trackId, shown.clip->id});
        }
    }
    if (pianoRoll_) {
        if (midi_)
            pianoRoll_->setClip(refs_.front().trackId, refs_.front().clipId);
        else
            pianoRoll_->setClip({}, {});
    }
    refresh();
    Q_EMIT opened();
}

void ClipViewController::forget() {
    refs_.clear();
    midi_ = false;
    baseline_.reset();
    if (pianoRoll_) pianoRoll_->setClip({}, {});
    refresh();
}

void ClipViewController::close() {
    const bool wasOpen = !refs_.isEmpty();
    forget();
    if (wasOpen) Q_EMIT closeRequested();
}

void ClipViewController::dropMissing() {
    if (refs_.isEmpty()) return;
    app::ClipRefs kept;
    for (const Shown& shown : existing(refs_)) kept.append({shown.trackId, shown.clip->id});
    if (kept.isEmpty()) {
        close();
        return;
    }
    refs_ = kept;
    refresh();
}

QStringList ClipViewController::shownTrackIds() const {
    QStringList ids;
    for (const app::ClipRef& ref : refs_) ids.append(ref.trackId);
    return ids;
}

QStringList ClipViewController::shownClipIds() const {
    QStringList ids;
    for (const app::ClipRef& ref : refs_) ids.append(ref.clipId);
    return ids;
}

QStringList ClipViewController::warpModes() { return app::kWarpModes; }

QStringList ClipViewController::warpModeTips() {
    return {
        QStringLiteral("Transients: short stretch blocks keep drum hits and other attacks tight."),
        QStringLiteral("Standard: good all-round, for melodies, bass lines and full mixes."),
        QStringLiteral("Smooth: long stretch blocks, for pads, ambience and noisy sounds; softens attacks."),
        QStringLiteral("Formants: like Standard, and keeps the formants (vocal character) when transposing."),
        QStringLiteral("Re-Pitch: no stretching; speed and pitch change together, like a turntable."),
    };
}

// --- Showing the clips' settings ----------------------------------------------------------

void ClipViewController::refresh() {
    const std::vector<Shown> items = existing(refs_);
    std::vector<const app::Clip*> clips;
    for (const Shown& shown : items) clips.push_back(shown.clip);
    audioClips_.clear();
    if (clips.empty()) {
        name_.clear();
        info_.clear();
        color_ = QColor();
        warp_ = false;
        warpModeIndex_ = -1;
        repitch_ = false;
    } else {
        const app::Project* p = project();
        const app::Clip& lead = *clips.front();
        color_ = QColor(p->track(items.front().trackId).color);
        if (midi_) {
            const auto count = lead.playedNotes().size();
            name_ = lead.name;
            info_ = QStringLiteral("%1 beats  ·  %2 note%3")
                        .arg(app::formatFixed(lead.durationBeats, 2))
                        .arg(count)
                        .arg(count == 1 ? QString() : QStringLiteral("s"));
        } else if (clips.size() == 1) {
            name_ = lead.name;
            const double tempo = p->tempo();
            const double beats = lead.lengthBeats(tempo);
            info_ = QStringLiteral("%1 s  ·  %2 beats")
                        .arg(app::formatFixed(app::beatsToSeconds(beats, tempo), 2), app::formatFixed(beats, 2));
        } else {
            QStringList tracks;
            for (const Shown& shown : items) {
                if (!tracks.contains(shown.trackId)) tracks.append(shown.trackId);
            }
            name_ = QStringLiteral("%1 Clips").arg(clips.size());
            info_ = QStringLiteral("on %1 track%2  ·  changes apply to every selected clip")
                        .arg(tracks.size())
                        .arg(tracks.size() > 1 ? QStringLiteral("s") : QString());
        }
        warp_ = std::all_of(clips.begin(), clips.end(), [](const app::Clip* c) { return c->warp; });
        const bool sameMode = std::all_of(clips.begin(), clips.end(),
                                          [&](const app::Clip* c) { return c->warpMode == lead.warpMode; });
        warpModeIndex_ = sameMode ? static_cast<int>(app::kWarpModes.indexOf(lead.warpMode)) : -1;
        segmentBpm_ = lead.segmentBpm ? lead.segmentBpm : p->tempo();
        repitch_ = std::all_of(clips.begin(), clips.end(),
                               [](const app::Clip* c) { return c->isWarped() && c->warpMode == kRePitch; });
        if (!midi_) {
            for (const Shown& shown : items)
                audioClips_.emplace_back(*shown.clip, QColor(p->track(shown.trackId).color));
        }
    }
    knobs_.clear();
    for (const KnobSpec& spec : knobSpecs()) {
        const QString name = QLatin1String(spec.name);
        QVariantMap knob;
        knob[QStringLiteral("caption")] = QLatin1String(spec.caption);
        knob[QStringLiteral("from")] = spec.from;
        knob[QStringLiteral("to")] = spec.to;
        knob[QStringLiteral("defaultValue")] = spec.defaultValue;
        knob[QStringLiteral("bipolar")] = spec.bipolar;
        const bool pitch = name == u"transpose" || name == u"detune";
        knob[QStringLiteral("tooltip")] = pitch && repitch_ ? kRepitchNote : spec.tooltip;
        double value = spec.defaultValue, low = value, high = value;
        if (!clips.empty() && !midi_) {
            value = knobValue(*clips.front(), name);
            low = high = value;
            for (const app::Clip* c : clips) {
                low = std::min(low, knobValue(*c, name));
                high = std::max(high, knobValue(*c, name));
            }
        }
        knob[QStringLiteral("value")] = value;
        knob[QStringLiteral("text")] =
            low == high ? format(name, low) : QStringLiteral("%1 … %2").arg(format(name, low), format(name, high));
        knob[QStringLiteral("differs")] = low != high;
        knob[QStringLiteral("readoutTooltip")] = low == high ? QString() : kDiffer;
        knobs_.insert(name, knob);
    }
    Q_EMIT changed();
}

QString ClipViewController::format(const QString& knob, double value) const {
    if (knob == u"transpose" || knob == u"detune") {
        const auto whole = static_cast<long long>(app::roundHalfEven(value));
        return QStringLiteral("%1%2 %3")
            .arg(whole < 0 ? QString() : QStringLiteral("+"))
            .arg(whole)
            .arg(knob == u"transpose" ? QStringLiteral("st") : QStringLiteral("ct"));
    }
    if (knob == u"gain") return app::formatDb(value);
    if (knob == u"pan") return app::formatPan(value);
    return QString::number(value);
}

// --- Editing all open clips ------------------------------------------------------------------

void ClipViewController::edit(const std::function<void()>& change) {
    if (!session_ || refs_.isEmpty()) return;
    try {
        change();
    } catch (const app::EditError& error) {
        Q_EMIT statusMessage(error.message());
    }
    refresh();  // (an edit refused leaves the controls where the clips are)
}

void ClipViewController::update(const std::function<app::Clip(const app::Clip&)>& change, const QString& text,
                                const QString& mergeKey) {
    edit([&] { session_->editor()->updateClips(refs_, change, text, mergeKey); });
}

void ClipViewController::setWarp(bool on) {
    const double tempo = project() ? project()->tempo() : 120.0;
    update(
        [on, tempo](const app::Clip& clip) {
            app::Clip changed = clip;
            changed.warp = on;
            if (!changed.segmentBpm) changed.segmentBpm = tempo;
            return changed;
        },
        QStringLiteral("Toggle Warp"));
}

void ClipViewController::setWarpMode(int index) {
    if (index < 0 || index >= app::kWarpModes.size()) return;
    const QString mode = app::kWarpModes.at(index);
    update(
        [mode](const app::Clip& clip) {
            app::Clip changed = clip;
            changed.warpMode = mode;
            return changed;
        },
        QStringLiteral("Change Warp Mode"));
}

void ClipViewController::setSegmentBpm(double bpm, const QString& gestureKey) {
    update(
        [bpm](const app::Clip& clip) {
            app::Clip changed = clip;
            changed.segmentBpm = bpm;
            return changed;
        },
        QStringLiteral("Change Segment BPM"), gestureKey);
}

void ClipViewController::scaleBpm(double factor) {
    const double tempo = project() ? project()->tempo() : 120.0;
    update(
        [factor, tempo](const app::Clip& clip) {
            app::Clip changed = clip;
            const double bpm = clip.segmentBpm ? clip.segmentBpm : tempo;
            changed.segmentBpm = round2(std::clamp(bpm * factor, kMinBpm, kMaxBpm));
            return changed;
        },
        QStringLiteral("Change Segment BPM"));
}

void ClipViewController::nudge(const QString& knob, double value, const QString& gestureKey) {
    const KnobSpec* spec = knobSpec(knob);
    const std::vector<Shown> items = existing(refs_);
    if (!spec || items.empty()) return;
    if (!baseline_ || baseline_->key != gestureKey || baseline_->knob != knob || gestureKey.isEmpty()) {
        Baseline baseline{gestureKey, knob, {}};
        for (const Shown& shown : items) baseline.values.insert(shown.clip->id, knobValue(*shown.clip, knob));
        baseline_ = baseline;
    }
    const QMap<QString, double> start = baseline_->values;
    double delta = value - start.value(items.front().clip->id);
    if (spec->integer) delta = app::roundHalfEven(delta);
    const bool integer = spec->integer;
    const double from = spec->from, to = spec->to;
    update(
        [&](const app::Clip& clip) {
            const double was = start.contains(clip.id) ? start.value(clip.id) : knobValue(clip, knob);
            const double moved = std::clamp(was + delta, from, to);
            return withKnobValue(clip, knob, integer ? app::roundHalfEven(moved) : moved);
        },
        QLatin1String(spec->text), gestureKey);
}

}  // namespace sub::ui
