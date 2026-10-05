#pragma once
// Long jobs on threads of their own, which the UI follows without waiting:
// exporting the arrangement and freezing a track (the engine's RenderJob:
// EngineRender, FreezeRender), and writing a reversed copy of a file
// (ReverseJob.h). Each says how far it got (0..1), whether its thread has ended,
// and can be cancelled (its file goes). While it runs it looks at itself every
// kPollMs, on the thread that made it, and says when it got further
// (progressChanged) and when it ended (ended); its owner then finishes it
// (EngineBridge::finishExport, finishFreeze, finishReversed), which no longer
// waits. Meanwhile (a render of the engine's) live output is silent, and the
// project as it was when it started is what renders: the UI takes no edits.

#include "model/Errors.h"

#include <QObject>
#include <QString>
#include <QTimer>

#include <memory>
#include <optional>

namespace sub {
class RenderJob;
}

namespace sub::app {

class RenderTask : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString path READ path CONSTANT)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(bool done READ done NOTIFY ended)
    Q_PROPERTY(bool cancelled READ cancelled NOTIFY cancelledChanged)

public:
    static constexpr int kPollMs = 30;  // how often it looks at itself while it runs

    ~RenderTask() override;

    // The file it writes.
    QString path() const { return path_; }
    // How far it got: 0..1.
    virtual double progress() const = 0;
    // Its thread has ended (finishing it doesn't wait).
    virtual bool done() const = 0;
    virtual bool cancelled() const = 0;
    // Stops it soon; its file goes.
    Q_INVOKABLE void cancel();

Q_SIGNALS:
    void progressChanged(double progress);
    void cancelledChanged();
    void ended();  // its thread has ended: finish it

protected:
    RenderTask(const QString& path, QObject* parent);
    virtual void cancelJob() = 0;
    // Starts looking at itself (once its thread runs).
    void follow();

private:
    void poll();

    QString path_;
    QTimer timer_;
    double reported_ = -1.0;
};

// The engine's render on a thread of its own (an export: EngineBridge::startExport).
class EngineRender : public RenderTask {
    Q_OBJECT

public:
    explicit EngineRender(std::shared_ptr<sub::RenderJob> job, QObject* parent = nullptr);
    ~EngineRender() override;

    double progress() const override;
    bool done() const override;
    bool cancelled() const override;

    // Waits for it (at once once it is done) and gives live output back: the
    // frames written, none if it was cancelled (its file gone). Throws EditError
    // (with a message for the user) if it failed. Once only.
    std::optional<qint64> finish();
    bool finished() const { return finished_; }

protected:
    void cancelJob() override;

private:
    std::shared_ptr<sub::RenderJob> job_;
    bool finished_ = false;
};

// A track's render for freezing it, in the background (EngineBridge::startFreeze).
class FreezeRender : public EngineRender {
    Q_OBJECT
    Q_PROPERTY(QString trackId READ trackId CONSTANT)

public:
    FreezeRender(const QString& trackId, std::shared_ptr<sub::RenderJob> job, double tempo,
                 QObject* parent = nullptr);

    QString trackId() const { return trackId_; }
    double tempo() const { return tempo_; }  // the project's when it started

private:
    QString trackId_;
    double tempo_;
};

}  // namespace sub::app
