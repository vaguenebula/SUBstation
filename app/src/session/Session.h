#pragma once
// The session: one open project and everything that works on it, as the main
// window had it. It owns the project, the undo stack, the editor, the
// selection, the engine bridge, the browser and the plug-in index, and wires
// them together (the hooks and signals the main window used to connect). The
// UI reaches all of it through the session: QML as the `Session` singleton
// (registered by the UI), the UI's C++ items through these accessors.
//
// The session's actions (transport, files, the Edit menu's commands that act on
// what is selected, renders in the background, preferences) are the main
// window's logic without its widgets: the UI shows dialogs and asks the session
// to act.

#include <QObject>
#include <QString>
#include <QUndoStack>

#include <cstdint>
#include <functional>
#include <memory>

namespace sub {
class Engine;
}

namespace sub::app {

class BrowserController;
class EngineBridge;
class PluginIndex;
class Project;
class ProjectEditor;
class Selection;

class Session : public QObject {
    Q_OBJECT
    Q_PROPERTY(sub::app::Project* project READ project CONSTANT)
    Q_PROPERTY(QUndoStack* undoStack READ undoStack CONSTANT)
    Q_PROPERTY(sub::app::ProjectEditor* editor READ editor CONSTANT)
    Q_PROPERTY(sub::app::Selection* selection READ selection CONSTANT)
    Q_PROPERTY(sub::app::EngineBridge* bridge READ bridge CONSTANT)
    Q_PROPERTY(sub::app::BrowserController* browser READ browser CONSTANT)
    Q_PROPERTY(sub::app::PluginIndex* plugins READ plugins CONSTANT)

public:
    struct Options {
        QString scanner;            // the plug-in scanner's executable; empty: next to the application
        bool scanPlugins = true;    // scan the plug-ins at once (tests turn it off)
        bool browserIndex = true;   // keep the browser's index on disk (tests turn it off)
    };

    explicit Session(sub::Engine& engine, QObject* parent = nullptr);
    Session(sub::Engine& engine, Options options, QObject* parent = nullptr);
    ~Session() override;

    sub::Engine& engine() const { return engine_; }
    Project* project() const { return project_; }
    QUndoStack* undoStack() const { return undoStack_; }
    ProjectEditor* editor() const { return editor_; }
    Selection* selection() const { return selection_; }
    EngineBridge* bridge() const { return bridge_; }
    BrowserController* browser() const { return browser_; }
    PluginIndex* plugins() const { return plugins_; }

    // The application's main window, which plug-in editors float above (a
    // native handle: an HWND on Windows; 0: none).
    void setOwnerWindow(std::function<uintptr_t()> ownerWindow);

    // After the UI shows: the audio device (and MIDI inputs, render threads) as
    // the preferences have them.
    void start();
    // Before the application ends: the device closes, plug-ins unload (while the
    // application is still whole), the browser's threads stop.
    void shutdown();

Q_SIGNALS:
    // For the status line: what the bridge, the browser, the plug-in index and the
    // editor (refused edits) have to say, and what the session's actions report.
    void statusMessage(const QString& message);

private:
    void wire();

    sub::Engine& engine_;
    Options options_;
    Project* project_ = nullptr;
    QUndoStack* undoStack_ = nullptr;
    ProjectEditor* editor_ = nullptr;
    Selection* selection_ = nullptr;
    EngineBridge* bridge_ = nullptr;
    PluginIndex* plugins_ = nullptr;
    BrowserController* browser_ = nullptr;
    bool shutDown_ = false;
};

}  // namespace sub::app
