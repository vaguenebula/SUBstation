#pragma once
// Humanizing (the intelligence module's humanize/, sub_intelligence) on the
// application thread's side: Session.humanizer.
//
// Velocities come from HUMANBRO's model, models/velocity.hbm next to the
// application (the build copies it into bin/models), loaded the first time
// they are asked for (about 20 ms, 16 MB) and kept; a model that is missing or
// can't be read is said so on statusMessage, and tried again the next time. A
// note's velocity depends on the notes around it, so each track is humanized as
// one part: the notes it plays on the timeline (every one of its clips', none
// of a deactivated clip nor deactivated notes) are the context, the notes
// humanized among them, each where its clip plays it. The model's velocities are levelled
// to each track's notes' own mean, so a part keeps its loudness and takes the
// model's shape: a melody over its accompaniment, a chord's top note, accents,
// phrases. Drum tracks (Harmony::isDrumTrack) are left as they are: the model
// knows pianos.
//
// Timing will be a model of its own, here beside this one; until then the
// piano roll nudges starts at random (notes::humanizedTiming).

#include <QObject>
#include <QPointer>
#include <QString>

#include <memory>
#include <optional>
#include <vector>

#include "model/Clip.h"

namespace sub::intelligence::humanize {
class VelocityModel;
}

namespace sub::app {

class Project;

class Humanizer : public QObject {
    Q_OBJECT
    // Whether the velocity model is there to load (the piano roll offers
    // Humanize › Velocity only then).
    Q_PROPERTY(bool velocityAvailable READ velocityAvailable CONSTANT)

public:
    // A note to humanize: its track's and clip's, and the note (in the clip's content beats).
    struct Target {
        QString trackId;
        QString clipId;
        Note note;
    };

    explicit Humanizer(Project* project, QObject* parent = nullptr);
    ~Humanizer() override;

    // <the application's folder>/models/velocity.hbm.
    static QString velocityModelPath();
    bool velocityAvailable() const;

    // New velocities for `targets`, one for each in order: each moved `amount`
    // (0..1) of the way to the model's, levelled to its track's targets' mean.
    // None if the model can't be loaded. Targets whose clip is gone, or on a
    // drum track, keep theirs.
    std::optional<std::vector<int>> velocities(const std::vector<Target>& targets, double amount);

Q_SIGNALS:
    // The model couldn't be loaded, or drum tracks were left out (for the status line).
    void statusMessage(const QString& message);

private:
    // Loaded on first use; null if it can't be (said on statusMessage).
    const intelligence::humanize::VelocityModel* velocityModel();

    QPointer<Project> project_;
    std::unique_ptr<intelligence::humanize::VelocityModel> velocity_;
};

}  // namespace sub::app
