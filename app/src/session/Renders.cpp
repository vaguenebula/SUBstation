#include "session/Renders.h"

#include <QFileInfo>

#include "audio/EngineBridge.h"
#include "audio/RenderTask.h"
#include "audio/ReverseJob.h"
#include "editor/ProjectEditor.h"
#include "model/Errors.h"
#include "model/Project.h"
#include "session/RenderProgress.h"

namespace sub::app {

// --- Render -----------------------------------------------------------------------------------

Render::Render(RenderProgress* progress, EngineBridge* bridge, QObject* parent)
    : QObject(parent), progress_(progress), bridge_(bridge), devicesTimer_(this) {
    devicesTimer_.setInterval(RenderTask::kPollMs);
    connect(&devicesTimer_, &QTimer::timeout, this, &Render::pollDevices);
    // Cancel: the task followed stops (its file goes); the render then ends as cancelled.
    connect(progress_, &RenderProgress::cancelRequested, this, [this] {
        if (followed_) followed_->cancel();
    });
}

Render::~Render() = default;

bool Render::cancelled() const { return progress_->cancelled(); }

void Render::waitForDevices(std::function<void(bool)> then) {
    devicesReady_ = std::move(then);
    showWaiting();
    progress_->setBusy(true);
    pollDevices();
    if (devicesReady_) devicesTimer_.start();
}

// What it waits for: plug-ins still loading, or built-in devices' states.
void Render::showWaiting() {
    if (cancelled()) return;
    const int waiting = bridge_->pluginsPending();
    progress_->setLabel(waiting > 0 ? QStringLiteral("Loading plug-ins (%1 to go)…").arg(waiting)
                                    : QStringLiteral("Loading devices…"));
}

void Render::pollDevices() {
    if (!devicesReady_) return;
    showWaiting();
    if (!cancelled() && !bridge_->devicesReady()) return;
    devicesTimer_.stop();
    progress_->setBusy(false);
    const auto then = std::move(devicesReady_);
    devicesReady_ = nullptr;
    then(!cancelled());
}

void Render::follow(RenderTask* task, const QString& label, int index, int count, std::function<void()> ended) {
    followed_ = task;
    ended_ = std::move(ended);
    progress_->setLabel(label);
    progress_->setProgress(index, count, task->progress());
    if (cancelled()) task->cancel();
    connect(task, &RenderTask::progressChanged, this,
            [this, index, count](double fraction) { progress_->setProgress(index, count, fraction); });
    // (Queued: the task's own signal is over before it is finished, and maybe deleted.)
    connect(task, &RenderTask::ended, this, &Render::taskEnded, Qt::QueuedConnection);
    if (task->done()) QMetaObject::invokeMethod(this, &Render::taskEnded, Qt::QueuedConnection);
}

void Render::taskEnded() {
    if (!followed_ || !ended_) return;  // (a task that had ended already, and said so too)
    disconnect(followed_, nullptr, this, nullptr);
    followed_ = nullptr;
    const auto ended = std::move(ended_);
    ended_ = nullptr;
    ended();
}

void Render::done() {
    if (finished_) return;
    finished_ = true;
    devicesTimer_.stop();
    devicesReady_ = nullptr;
    if (followed_) disconnect(followed_, nullptr, this, nullptr);
    followed_ = nullptr;
    ended_ = nullptr;
    progress_->end();
    Q_EMIT finished();
    deleteLater();
}

// --- Export ---------------------------------------------------------------------------------------

ExportAudioRender::ExportAudioRender(RenderProgress* progress, EngineBridge* bridge, const QString& path,
                                     double startBeat, double endBeat, int bitDepth, QObject* parent)
    : Render(progress, bridge, parent), path_(path), startBeat_(startBeat), endBeat_(endBeat), bitDepth_(bitDepth) {}

ExportAudioRender::~ExportAudioRender() = default;

void ExportAudioRender::start() {
    progress_->begin(QStringLiteral("Export Audio"));
    waitForDevices([this](bool ready) {  // plug-ins, samples still loading
        if (!ready) {
            Q_EMIT statusMessage(QStringLiteral("Export cancelled"));
            done();
            return;
        }
        try {
            render_ = bridge_->startExport(path_, startBeat_, endBeat_, bitDepth_);
        } catch (const EditError& error) {
            Q_EMIT warning(QStringLiteral("Export failed: ") + error.message());
            done();
            return;
        }
        follow(render_.get(), QStringLiteral("Exporting %1…").arg(QFileInfo(path_).fileName()), 0, 1,
               [this] { rendered(); });
    });
}

void ExportAudioRender::rendered() {
    std::optional<qint64> frames;
    try {
        frames = bridge_->finishExport(*render_);
    } catch (const EditError& error) {
        Q_EMIT warning(QStringLiteral("Export failed: ") + error.message());
        done();
        return;
    }
    const QString name = QFileInfo(path_).fileName();
    Q_EMIT statusMessage(frames ? QStringLiteral("Exported ") + name : QStringLiteral("Export cancelled"));
    done();
}

void ExportAudioRender::abort() {
    if (isFinished()) return;
    if (render_ && !render_->finished()) {
        render_->cancel();
        try {
            bridge_->finishExport(*render_);
        } catch (const EditError&) {
        }
    }
    done();
}

// --- Freezing ------------------------------------------------------------------------------------

FreezeTracksRender::FreezeTracksRender(RenderProgress* progress, EngineBridge* bridge, ProjectEditor* editor,
                                       QStringList tracks, Finisher finisher, QObject* parent)
    : Render(progress, bridge, parent), editor_(editor), tracks_(std::move(tracks)), finisher_(std::move(finisher)) {}

FreezeTracksRender::~FreezeTracksRender() = default;

void FreezeTracksRender::start() {
    progress_->begin(tracks_.size() > 1 ? QStringLiteral("Freeze Tracks") : QStringLiteral("Freeze Track"));
    waitForDevices([this](bool ready) {
        if (!ready) {
            done();
            return;
        }
        next();
    });
}

void FreezeTracksRender::next() {
    if (index_ >= tracks_.size()) {
        // Every render made: one undo step freezes them all.
        frozen_ = editor_->freezeTracks(freezes_);
        for (const auto& [trackId, freeze] : freezes_) {
            if (!frozen_.contains(trackId)) bridge_->discardFreeze(freeze);  // (the editor refused it)
        }
        done();
        return;
    }
    const QString trackId = tracks_.at(index_);
    const Track* track = editor_->project()->findTrack(trackId);
    const QString name = track != nullptr ? track->name : trackId;
    try {
        render_ = bridge_->startFreeze(trackId);
    } catch (const EditError& error) {
        Q_EMIT editor_->refused(QStringLiteral("%1 could not be frozen: %2").arg(name, error.message()));
        discard();
        done();
        return;
    }
    const auto count = tracks_.size();
    const QString of = count > 1 ? QStringLiteral(" (%1 of %2)").arg(index_ + 1).arg(count) : QString();
    follow(render_.get(), QStringLiteral("Freezing %1%2…").arg(name, of), static_cast<int>(index_),
           static_cast<int>(count), [this] { rendered(); });
}

void FreezeTracksRender::rendered() {
    const QString trackId = tracks_.at(index_);
    const Track* track = editor_->project()->findTrack(trackId);
    const QString name = track != nullptr ? track->name : trackId;
    std::optional<Freeze> freeze;
    try {
        freeze = finisher_(*render_);
    } catch (const EditError& error) {
        render_.reset();
        Q_EMIT editor_->refused(QStringLiteral("%1 could not be frozen: %2").arg(name, error.message()));
        discard();
        done();
        return;
    }
    render_.reset();
    if (!freeze) {  // cancelled: none is frozen
        discard();
        done();
        return;
    }
    freezes_.insert(trackId, *freeze);
    ++index_;
    next();
}

// The renders no track will play: gone.
void FreezeTracksRender::discard() {
    for (const auto& [trackId, freeze] : freezes_) bridge_->discardFreeze(freeze);
    freezes_.clear();
}

void FreezeTracksRender::abort() {
    if (isFinished()) return;
    if (render_ && !render_->finished()) {
        render_->cancel();
        try {
            if (const auto freeze = bridge_->finishFreeze(*render_)) bridge_->discardFreeze(*freeze);
        } catch (const EditError&) {
        }
    }
    render_.reset();
    discard();
    done();
}

// --- Reversing ------------------------------------------------------------------------------------

ReverseClipsRender::ReverseClipsRender(RenderProgress* progress, EngineBridge* bridge,
                                       std::vector<std::pair<QString, std::unique_ptr<ReverseJob>>> jobs,
                                       QObject* parent)
    : Render(progress, bridge, parent), jobs_(std::move(jobs)) {}

ReverseClipsRender::~ReverseClipsRender() = default;

void ReverseClipsRender::start() {
    progress_->begin(QStringLiteral("Reverse Clips"));
    next();
}

void ReverseClipsRender::next() {
    if (index_ >= jobs_.size()) {
        completed_ = true;
        done();
        return;
    }
    const auto& [path, job] = jobs_[index_];
    follow(job.get(), QStringLiteral("Reversing %1…").arg(QFileInfo(path).fileName()), static_cast<int>(index_),
           static_cast<int>(jobs_.size()), [this] { written(); });
}

void ReverseClipsRender::written() {
    const auto& [path, job] = jobs_[index_];
    std::optional<std::pair<QString, double>> result;
    try {
        result = bridge_->finishReversed(path, *job);
    } catch (const EditError& error) {
        Q_EMIT statusMessage(error.message());
        ++index_;
        next();
        return;
    }
    if (!result || cancelled()) {  // cancelled: the rest too, and none is used
        cancelRest();
        done();
        return;
    }
    reversed_.insert(path, *result);
    ++index_;
    next();
}

// Cancels the jobs after the current one (one done already is kept for next time).
void ReverseClipsRender::cancelRest() {
    for (size_t i = index_ + 1; i < jobs_.size(); ++i) {
        auto& [path, job] = jobs_[i];
        job->cancel();
        try {
            bridge_->finishReversed(path, *job);
        } catch (const EditError&) {
        }
    }
    index_ = jobs_.size();
}

void ReverseClipsRender::abort() {
    if (isFinished()) return;
    for (size_t i = index_; i < jobs_.size(); ++i) {
        auto& [path, job] = jobs_[i];
        job->cancel();
        try {
            bridge_->finishReversed(path, *job);
        } catch (const EditError&) {
        }
    }
    index_ = jobs_.size();
    done();
}

}  // namespace sub::app
