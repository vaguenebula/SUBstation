#include "session/Session.h"

#include <QTimer>

#include "Engine.h"
#include "audio/AudioSettings.h"
#include "audio/EngineBridge.h"
#include "browser/BrowserController.h"
#include "editor/ProjectEditor.h"
#include "io/Presets.h"
#include "model/Automation.h"
#include "model/ParamSpec.h"
#include "model/Project.h"
#include "plugins/PluginIndex.h"
#include "session/Selection.h"

namespace sub::app {

Session::Session(sub::Engine& engine, QObject* parent) : Session(engine, Options{}, parent) {}

Session::Session(sub::Engine& engine, Options options, QObject* parent)
    : QObject(parent), engine_(engine), options_(std::move(options)) {
    project_ = new Project(this);
    undoStack_ = new QUndoStack(this);
    editor_ = new ProjectEditor(project_, undoStack_, this);
    selection_ = new Selection(this);
    bridge_ = new EngineBridge(engine_, project_, this);
    plugins_ = new PluginIndex(this, options_.scanner);
    BrowserController::Options browserOptions;
    browserOptions.scanPlugins = options_.scanPlugins;
    if (!options_.browserIndex) browserOptions.indexPath = QString();
    browser_ = new BrowserController(plugins_, browserOptions, this);
    wire();
}

Session::~Session() { shutdown(); }

void Session::wire() {
    // What the editor needs from the engine: a plug-in's (or a rack's)
    // parameter mapping, for macros; a plug-in parameter's value as its own
    // editor set it, to undo a macro to. And where new devices come from: the
    // user's default presets.
    editor_->setParamInfo([this](const QString& trackId, const QString& deviceId,
                                 const QString& paramId) -> std::optional<ParamSpec> {
        const auto info = bridge_->deviceParamInfo(trackId, deviceId, paramId);
        if (!info) return std::nullopt;
        return ParamSpec::fromInfo(*info, automation::deviceKey(deviceId, paramId), QString());
    });
    editor_->setOwnValue([this](const QString& owner, const QString& key) { return bridge_->ownValue(owner, key); });
    editor_->setDeviceDefaults([](const QString& kind, const std::optional<PluginRef>& plugin) {
        return defaultDevice(kind, plugin);
    });

    for (QObject* source : {static_cast<QObject*>(bridge_), static_cast<QObject*>(browser_),
                            static_cast<QObject*>(plugins_)}) {
        connect(source, SIGNAL(statusMessage(QString)), this, SIGNAL(statusMessage(QString)));
    }
    connect(editor_, &ProjectEditor::refused, this, &Session::statusMessage);  // an edit a frozen track can't take

    // The plug-ins the scan found are where the bridge looks for a device's plug-in.
    connect(plugins_, &PluginIndex::updated, this, [this] { bridge_->setKnownPlugins(plugins_->plugins()); });
    // Previewing files from the browser.
    connect(browser_, &BrowserController::previewRequested, bridge_,
            [this](const QString& path) { bridge_->previewFile(path); });
    connect(browser_, &BrowserController::previewStopped, bridge_, [this] { bridge_->stopPreview(); });

    // Only the selected track's plug-in editors show; its plug-ins load first
    // after a project opens.
    connect(selection_, &Selection::changed, this, [this] {
        bridge_->showPluginEditors(selection_->trackId());
        bridge_->prioritizePlugins(selection_->trackId());
    });
    // A plug-in the user added shows its editor: after the add is done (the
    // track may be selected just after it, and a drop finished), when its track shows.
    connect(editor_, &ProjectEditor::pluginAdded, this, [this](const QString& trackId, const QString& deviceId) {
        QTimer::singleShot(0, this, [this, trackId, deviceId] {
            if (project_->hasOwner(trackId)) bridge_->requestPluginEditor(trackId, deviceId);
        });
    });
    // A plug-in's own editor changed a parameter: an undo step (one per knob drag).
    connect(bridge_, &EngineBridge::pluginParamEdited, this,
            [this](const QString& trackId, const QString& deviceId, const QString& paramId, double value, double old,
                   quint32 gesture) {
                if (!project_->hasOwner(trackId)) return;
                const QString mergeKey = QStringLiteral("plugin edit|%1|%2|%3").arg(deviceId, paramId).arg(gesture);
                editor_->setDeviceParam(trackId, deviceId, paramId, value, mergeKey, old);
            });
    connect(bridge_, &EngineBridge::pluginParamTouched, this,
            [this](const QString& trackId, const QString& deviceId, const QString& paramId) {
                if (project_->hasOwner(trackId))
                    editor_->touchParameter(trackId, automation::deviceKey(deviceId, paramId));
            });
    // A plug-in changed in a way no edit shows: the project has changes to save.
    connect(bridge_, &EngineBridge::pluginStateDirty, undoStack_, &QUndoStack::resetClean);
    // A recording ended: its takes become clips, one undo step, and are selected.
    connect(bridge_, &EngineBridge::takesRecorded, this, [this](const std::vector<RecordedTake>& takes) {
        const ClipRefs refs = editor_->addRecordings(takes, recordQuantize());
        if (!refs.isEmpty()) selection_->selectClips(*editor_, refs);
    });
}

void Session::setOwnerWindow(std::function<uintptr_t()> ownerWindow) { bridge_->setOwnerWindow(std::move(ownerWindow)); }

void Session::start() { bridge_->startAudio(); }

void Session::shutdown() {
    if (shutDown_) return;
    shutDown_ = true;
    bridge_->stop();
    bridge_->stopPreview();
    bridge_->closeAllEditors();
    browser_->shutdown();
    engine_.closeDevice();
    bridge_->shutdown();  // unloads the plug-ins, while the application is still whole
}

}  // namespace sub::app
