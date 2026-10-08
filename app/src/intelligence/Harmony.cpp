#include "intelligence/Harmony.h"

#include <QRegularExpression>
#include <QSettings>

#include "model/Project.h"

namespace sub::app {

Harmony::Harmony(Project* project, QObject* parent) : QObject(parent), project_(project) {
    settle_.setSingleShot(true);
    settle_.setInterval(kSettleMs);
    connect(&settle_, &QTimer::timeout, this, &Harmony::changed);
    inputs_ = inputs();
    connect(project, &Project::clipsChanged, this, [this](const QString& trackId) {
        if (inputs_.tracks.contains(trackId)) invalidate();
    });
    // A track muted, renamed, grouped, added or gone; the time signature or the key.
    for (const auto signal : {&Project::trackInserted, &Project::trackRemoved})
        connect(project, signal, this, &Harmony::projectChanged);
    connect(project, &Project::trackChanged, this, &Harmony::projectChanged);
    connect(project, &Project::tracksArranged, this, &Harmony::projectChanged);
    connect(project, &Project::settingsChanged, this, &Harmony::projectChanged);
    connect(project, &Project::reset, this, [this] {
        inputs_ = inputs();
        invalidate();
    });
}

Harmony::Inputs Harmony::inputs() const {
    if (!project_) return {};
    return {heardTracks(*project_), project_->timeSignature(), project_->key()};
}

void Harmony::projectChanged() {
    Inputs now = inputs();
    if (now == inputs_) return;
    inputs_ = std::move(now);
    invalidate();
}

void Harmony::invalidate() {
    stale_ = true;
    if (!settle_.isActive()) settle_.start();
}

const intelligence::harmony::Harmony& Harmony::current() const {
    if (stale_ && project_) {
        intelligence::harmony::InferenceOptions options;
        options.barBeats = project_->timeSignature().beatsPerBar();
        if (const auto& key = project_->key()) options.key = toHarmony(*key);
        result_ = intelligence::harmony::inferHarmony(songNotes(*project_), options);
        stale_ = false;
    }
    return result_;
}

const std::vector<Harmony::ChordSpan>& Harmony::chords() const { return current().chords; }

std::optional<Key> Harmony::key() const {
    if (project_ && project_->key()) return project_->key();
    const auto& key = current().key;
    return key ? std::optional<Key>(fromHarmony(*key)) : std::nullopt;
}

QString Harmony::keyLabel() const {
    const auto key = this->key();
    return key ? key->label() : QString();
}

bool Harmony::keyInferred() const { return project_ && !project_->key() && key().has_value(); }

bool Harmony::shown() const { return QSettings().value(kShownKey, true).toBool(); }

void Harmony::setShown(bool shown) {
    if (shown == this->shown()) return;
    QSettings().setValue(kShownKey, shown);
    Q_EMIT shownChanged();
}

QStringList Harmony::heardTracks(const Project& project) {
    QStringList tracks;
    for (const Track& track : project.tracks()) {
        if (!track.isMidi() || track.mute || isDrumTrack(track.name)) continue;
        bool heard = true;
        for (const QString& group : project.ancestors(track.id)) heard = heard && !project.track(group).mute;
        if (heard) tracks.append(track.id);
    }
    return tracks;
}

std::vector<intelligence::harmony::Note> Harmony::songNotes(const Project& project) {
    std::vector<intelligence::harmony::Note> notes;
    for (const QString& id : heardTracks(project)) {
        for (const Clip& clip : project.track(id).clips) {
            for (const PlayedNote& played : clip.heardNotes())
                notes.push_back({played.note.pitch, played.start, played.end, played.note.velocity});
        }
    }
    return notes;
}

bool Harmony::isDrumTrack(const QString& name) {
    static const QRegularExpression drums(
        QStringLiteral("(?<![a-z])(drums?|kick|snare|(hi-?)?hats?|perc(ussion)?|claps?|cymbals?|toms?|rims?|shakers?|"
                       "cowbell|crash|breaks?)(?![a-z])"),
        QRegularExpression::CaseInsensitiveOption);
    return drums.match(name).hasMatch();
}

}  // namespace sub::app
