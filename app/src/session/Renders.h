#pragma once
// The session's renders in the background, as state machines driven by their
// tasks' signals (RenderTask: progressChanged, ended) and the progress dialog's
// Cancel (RenderProgress): no nested event loops. Each shows itself in the
// session's RenderProgress, says what came of it on its signals, and deletes
// itself when it has finished. The application's own, not the UI's.
//
// - ExportAudioRender: the arrangement (the loop, a time selection) into a WAV
//   or MP3 file, once the devices are ready.
// - FreezeTracksRender: several tracks, one after another, then frozen in one
//   undo step: all or nothing (cancelled, or one failing, none is frozen, and
//   the renders made so far are deleted).
// - ReverseClipsRender: reversed copies of long files, written side by side,
//   followed one after another; cancelled, none is used.

#include <QMap>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "audio/BridgeTypes.h"
#include "model/OrderedMap.h"
#include "model/Track.h"

namespace sub::app {

class EngineBridge;
class EngineRender;
class FreezeRender;
class ProjectEditor;
class RenderProgress;
class RenderTask;
class ReverseJob;

class Render : public QObject {
    Q_OBJECT

public:
    ~Render() override;

    // Starts it (the progress dialog shows).
    virtual void start() = 0;
    // Stops it now, waiting for its threads (the application closing): nothing
    // changes, what it wrote goes, nothing is said. It finishes.
    virtual void abort() = 0;
    bool isFinished() const { return finished_; }

Q_SIGNALS:
    void statusMessage(const QString& message);
    void warning(const QString& message);  // what was a message box
    void finished();                       // it ended (the dialog went); it deletes itself

protected:
    Render(RenderProgress* progress, EngineBridge* bridge, QObject* parent);

    // Until every device is ready to render offline (a project's plug-ins still
    // loading, samples): a busy bar meanwhile, saying what it waits for. Then
    // then(true); then(false) if cancelled first.
    void waitForDevices(std::function<void(bool)> then);
    // Shows a task's progress (as part `index` of `count` of the bar) until it
    // ends (cancelled, if the user cancels); then `ended` (which finishes it).
    void follow(RenderTask* task, const QString& label, int index, int count, std::function<void()> ended);
    bool cancelled() const;
    // The dialog goes; finished (deleted later).
    void done();

    RenderProgress* progress_;
    EngineBridge* bridge_;

private:
    void showWaiting();
    void pollDevices();
    void taskEnded();

    QTimer devicesTimer_;
    std::function<void(bool)> devicesReady_;
    QPointer<RenderTask> followed_;
    std::function<void()> ended_;
    bool finished_ = false;
};

// Export Audio: from `startBeat` to `endBeat` into `path`, at `bitDepth`.
class ExportAudioRender : public Render {
    Q_OBJECT

public:
    ExportAudioRender(RenderProgress* progress, EngineBridge* bridge, const QString& path, double startBeat,
                      double endBeat, const AudioExportFormat& format, QObject* parent = nullptr);
    ~ExportAudioRender() override;

    void start() override;
    void abort() override;

private:
    void rendered();

    QString path_;
    double startBeat_;
    double endBeat_;
    AudioExportFormat format_;
    std::unique_ptr<EngineRender> render_;
};

// Freezing tracks (each rendered in turn, then all frozen in one undo step).
class FreezeTracksRender : public Render {
    Q_OBJECT

public:
    // How a track's render becomes its frozen audio (EngineBridge::finishFreeze;
    // the tests make it fail).
    using Finisher = std::function<std::optional<Freeze>(FreezeRender&)>;

    // `tracks`: those to freeze, in order (that can be frozen).
    FreezeTracksRender(RenderProgress* progress, EngineBridge* bridge, ProjectEditor* editor, QStringList tracks,
                       Finisher finisher, QObject* parent = nullptr);
    ~FreezeTracksRender() override;

    void start() override;
    void abort() override;
    // The tracks frozen (once finished; none if cancelled or failed).
    const QStringList& frozen() const { return frozen_; }

private:
    void next();
    void rendered();
    void discard();

    ProjectEditor* editor_;
    QStringList tracks_;
    Finisher finisher_;
    int index_ = 0;
    std::unique_ptr<FreezeRender> render_;
    OrderedMap<QString, Freeze> freezes_;
    QStringList frozen_;
};

// Reversed copies of files written in the background (long ones), followed in turn.
class ReverseClipsRender : public Render {
    Q_OBJECT

public:
    using Reversed = QMap<QString, std::pair<QString, double>>;  // file -> (its reversed copy, its seconds)

    ReverseClipsRender(RenderProgress* progress, EngineBridge* bridge,
                       std::vector<std::pair<QString, std::unique_ptr<ReverseJob>>> jobs, QObject* parent = nullptr);
    ~ReverseClipsRender() override;

    void start() override;
    void abort() override;
    // Once finished: whether every copy was made (or failed, said so) without
    // being cancelled, and the copies made.
    bool completed() const { return completed_; }
    const Reversed& reversed() const { return reversed_; }

private:
    void next();
    void written();
    void cancelRest();

    std::vector<std::pair<QString, std::unique_ptr<ReverseJob>>> jobs_;
    size_t index_ = 0;
    Reversed reversed_;
    bool completed_ = false;
};

}  // namespace sub::app
