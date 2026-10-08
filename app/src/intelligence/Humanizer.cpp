#include "intelligence/Humanizer.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QHash>
#include <QStringList>

#include <algorithm>

#include "humanize/VelocityModel.h"
#include "model/Project.h"

namespace sub::app {

namespace humanize = intelligence::humanize;

Humanizer::Humanizer(Project* project, QObject* parent) : QObject(parent), project_(project) {}

Humanizer::~Humanizer() = default;

QString Humanizer::velocityModelPath() {
    return QCoreApplication::applicationDirPath() + QStringLiteral("/models/velocity.hbm");
}

bool Humanizer::velocityAvailable() const { return QFileInfo(velocityModelPath()).isFile(); }

const humanize::VelocityModel* Humanizer::velocityModel() {
    if (!velocityTried_) {
        velocityTried_ = true;
        try {
            velocity_ = std::make_unique<humanize::VelocityModel>(velocityModelPath().toStdString());
        } catch (const humanize::ModelError& error) {
            Q_EMIT statusMessage(QStringLiteral("The velocity model couldn't be loaded: ") +
                                 QString::fromStdString(error.what()));
        }
    }
    return velocity_.get();
}

std::optional<std::vector<int>> Humanizer::velocities(const std::vector<Target>& targets, double amount) {
    std::vector<int> result;
    result.reserve(targets.size());
    for (const Target& target : targets) result.push_back(target.note.velocity);
    if (targets.empty() || !project_) return result;
    const humanize::VelocityModel* model = velocityModel();
    if (!model) return std::nullopt;

    const Project& project = *project_;
    const humanize::Meter meter{project.tempo(), project.timeSignature().numerator,
                                project.timeSignature().denominator};
    // Each track's targets (indices into `targets`), in the order the tracks first come.
    QStringList tracks;
    QHash<QString, std::vector<size_t>> byTrack;
    for (size_t i = 0; i < targets.size(); ++i) {
        if (!project.findClip(targets[i].trackId, targets[i].clipId)) continue;
        if (!byTrack.contains(targets[i].trackId)) tracks.append(targets[i].trackId);
        byTrack[targets[i].trackId].push_back(i);
    }
    for (const QString& trackId : tracks) {
        const std::vector<size_t>& mine = byTrack[trackId];
        // The track's part: what it plays around the targets, then the targets.
        QHash<QString, std::vector<Note>> targetNotes;  // by clip
        for (const size_t i : mine) targetNotes[targets[i].clipId].push_back(targets[i].note);
        std::vector<humanize::Note> part;
        for (const Clip& clip : project.track(trackId).clips) {
            const auto found = targetNotes.constFind(clip.id);
            for (const PlayedNote& played : clip.heardNotes()) {
                if (found != targetNotes.cend() && std::find(found->begin(), found->end(), played.note) != found->end())
                    continue;
                part.push_back({played.note.pitch, played.start, played.end - played.start, played.note.velocity, false});
            }
        }
        const size_t first = part.size();
        for (const size_t i : mine) {
            const Note& note = targets[i].note;
            const Clip* clip = project.findClip(trackId, targets[i].clipId);
            part.push_back({note.pitch, clip->toTimeline(note.start), note.length, note.velocity, true});
        }
        try {
            const std::vector<int> velocities = model->humanize(part, meter, amount);
            for (size_t k = 0; k < mine.size(); ++k) result[mine[k]] = velocities[first + k];
        } catch (const humanize::ModelError& error) {
            Q_EMIT statusMessage(QStringLiteral("The velocity model couldn't humanize the notes: ") +
                                 QString::fromStdString(error.what()));
            return std::nullopt;
        }
    }
    return result;
}

}  // namespace sub::app
