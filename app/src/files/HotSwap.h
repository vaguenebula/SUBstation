#pragma once
// Hot-swapping samples, as Ableton's Hot-Swap: Session.hotSwap.
//
// Started on a file (an audio clip's menu: the clip's file; a File Manager
// row's hot swap button), it takes every clip and sampler of the project
// playing that file, and links the browser to them: the browser lists the
// sounds most like it at once (Find Similar Sounds: for a clip, like the part
// of its file it plays), and every audio file the user chooses there (a click
// let go of without a drag, the arrow keys, in any list or a place's folder
// tree: BrowserController::fileChosen) plays in their place at once, so it is
// heard in the song while it plays. A double-click or Enter keeps the file and
// ends the hot swap; Esc, the browser bar's ✕ or the button again end it,
// keeping what was swapped in last. However many files were tried, a hot swap
// is one undo step (ProjectEditor::replaceFile with a merge key of its own),
// and trying the file it began with again takes the step away.
//
// It never outlives what the user is doing with it: any other edit, an undo
// or redo (the undo stack moving but for its own swaps) ends it, and the UI
// ends it on a press outside the browser (but the File Manager and the
// transport bar) and when a drag starts from the browser. A sample dragged out
// of the browser while one runs is added, never swapped in.
//
// findSimilar() lists the sounds most like what is swapped in now (the
// browser bar's Similar). Clips and devices on frozen tracks are left out (a
// hot swap of nothing else doesn't start). It ends by itself when what it
// swaps is gone (deleted, another project opened).

#include <QObject>
#include <QPointer>
#include <QString>

class QUndoStack;

#include <optional>

#include "files/ProjectFiles.h"

namespace sub::app {

class BrowserController;
class EngineBridge;
class Project;
class ProjectEditor;

class HotSwap : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY changed)
    // The file swapped in now (at first, the one it began with) and its name.
    Q_PROPERTY(QString path READ path NOTIFY changed)
    Q_PROPERTY(QString name READ name NOTIFY changed)
    // The file it began with: the File Manager lights its row's button.
    Q_PROPERTY(QString originPath READ originPath NOTIFY changed)
    // What it swaps: "1 clip", "3 clips", "Sampler"...
    Q_PROPERTY(QString usesText READ usesText NOTIFY changed)

public:
    HotSwap(Project* project, ProjectEditor* editor, QUndoStack* undoStack, EngineBridge* bridge,
            BrowserController* browser, QObject* parent = nullptr);

    bool active() const { return active_; }
    QString path() const { return path_; }
    QString name() const;
    QString originPath() const { return origin_; }
    QString usesText() const;
    const FileUses& uses() const { return uses_; }

    // Starts hot-swapping `uses`, which play `path` (one running ends first),
    // and the browser lists the sounds most like it: like the part of it
    // `anchor` plays, if given (a clip's). False (and a statusMessage) if none
    // of it can be swapped: all on frozen tracks.
    bool start(const FileUses& uses, const QString& path, const std::optional<ClipRef>& anchor = std::nullopt);
    // Every clip and sampler playing this clip's file (an audio clip's menu);
    // the similar sounds are like the part of it this clip plays.
    bool startClip(const ClipRef& clip);
    // Every clip and sampler playing a file (a File Manager row).
    Q_INVOKABLE bool startFile(const QString& path);
    // The browser lists the sounds most like the one swapped in now (for a
    // hot swap started on a clip: like the part of it that clip plays).
    Q_INVOKABLE void findSimilar();
    // A file selected in the browser: it plays in their place (an audio file;
    // anything else is left alone). Whether it was swapped in.
    Q_INVOKABLE bool swap(const QString& path);
    // A double-click or Enter in the browser: swapped in, and the hot swap ends.
    Q_INVOKABLE void commit(const QString& path);
    Q_INVOKABLE void stop();

Q_SIGNALS:
    void changed();
    void statusMessage(const QString& message);

private:
    void prune();

    Project* project_;
    ProjectEditor* editor_;
    EngineBridge* bridge_;
    QPointer<BrowserController> browser_;
    bool active_ = false;
    bool swapping_ = false;  // (its own edit: the undo stack moving then doesn't end it)
    FileUses uses_;
    std::optional<ClipRef> anchor_;  // the clip it was started on
    QString path_;
    QString origin_;
    QString mergeKey_;
};

}  // namespace sub::app
