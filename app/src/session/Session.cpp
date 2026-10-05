#include "session/Session.h"

#include <QTimer>

#include <cmath>

#include "AppInfo.h"
#include "Engine.h"
#include "audio/AudioSettings.h"
#include "audio/EngineBridge.h"
#include "browser/BrowserController.h"
#include "browser/PresetIndex.h"
#include "editor/ProjectEditor.h"
#include "io/Presets.h"
#include "model/Automation.h"
#include "model/ParamSpec.h"
#include "model/Project.h"
#include "plugins/PluginIndex.h"
#include "session/ArrangementActions.h"
#include "session/AudioPreferences.h"
#include "session/ComputerKeyboard.h"
#include "session/DeviceSelection.h"
#include "session/MidiPreferences.h"
#include "session/RenderProgress.h"
#include "session/Renders.h"
#include "session/Selection.h"
#include "session/SessionSupport.h"

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
    presets_ = new PresetIndex(this);
    render_ = new RenderProgress(this);
    arrangement_ = new ArrangementActions(editor_, selection_, bridge_, render_, this);
    devices_ = new DeviceSelection(editor_, selection_, bridge_, this);
    keyboard_ = new ComputerKeyboard(bridge_, this);
    audioPreferences_ = new AudioPreferences(bridge_, this);
    midiPreferences_ = new MidiPreferences(bridge_, this);
    finishFreeze_ = [this](FreezeRender& render) { return bridge_->finishFreeze(render); };
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
                            static_cast<QObject*>(plugins_), static_cast<QObject*>(arrangement_),
                            static_cast<QObject*>(devices_), static_cast<QObject*>(keyboard_)}) {
        connect(source, SIGNAL(statusMessage(QString)), this, SIGNAL(statusMessage(QString)));
    }
    connect(editor_, &ProjectEditor::refused, this, &Session::statusMessage);  // an edit a frozen track can't take

    // The plug-ins the scan found are where the bridge looks for a device's plug-in.
    connect(plugins_, &PluginIndex::updated, this, [this] { bridge_->setKnownPlugins(plugins_->plugins()); });
    // Previewing files from the browser.
    connect(browser_, &BrowserController::previewRequested, bridge_,
            [this](const QString& path) { bridge_->previewFile(path); });
    connect(browser_, &BrowserController::previewStopped, bridge_, [this] { bridge_->stopPreview(); });
    // What the browser adds (a double-click, Enter) goes on the selected track.
    connect(browser_, &BrowserController::fileActivated, this, &Session::addFileAtInsert);
    connect(browser_, &BrowserController::deviceActivated, this, &Session::addDeviceToSelectedTrack);
    connect(browser_, &BrowserController::pluginActivated, this, &Session::addPluginToSelectedTrack);
    connect(browser_, &BrowserController::presetActivated, this, &Session::addPresetToSelectedTrack);
    // The presets the browser lists: the library's, listed again when it changes.
    auto listPresets = [this] { browser_->setPresets(presets_->items(), presets_->groups(), presets_->root()); };
    listPresets();
    connect(presets_, &PresetIndex::updated, this, listPresets);
    connect(browser_, &BrowserController::presetsChanged, presets_, &PresetIndex::rescan);
    connect(devices_, &DeviceSelection::presetSaved, this, &Session::presetSaved);

    // Only the selected track's plug-in editors show; its plug-ins load first
    // after a project opens.
    connect(selection_, &Selection::changed, this, [this] {
        bridge_->showPluginEditors(selection_->trackId());
        bridge_->prioritizePlugins(selection_->trackId());
    });
    // The selection lets go of what is gone.
    for (auto structure : {&Project::tracksArranged, &Project::reset}) connect(project_, structure, this, &Session::pruneSelection);
    for (auto changed : {&Project::trackInserted, &Project::trackRemoved, &Project::returnInserted, &Project::returnRemoved}) {
        connect(project_, changed, this, [this](const QString&, int) { pruneSelection(); });
    }
    connect(project_, &Project::clipsChanged, this, [this](const QString&) { pruneSelection(); });
    connect(project_, &Project::automationChanged, this, [this](const QString&, const QString&) { pruneSelection(); });
    // A parameter changed by hand: its automation shows (as the lane's parameter).
    connect(editor_, &ProjectEditor::parameterTouched, this, [this](const QString& owner, const QString& key) {
        if (!project_->hasOwner(owner)) return;
        const AutomationView& view = project_->automationView(owner);
        if (view.shown && view.key == key) return;
        if (bridge_->canAutomate(owner, key)) editor_->showAutomation(owner, key);
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
        const ClipRefs refs = editor_->addRecordings(takes, sub::app::recordQuantize());
        if (!refs.isEmpty()) selection_->selectClips(*editor_, refs);
    });
    // Re-Enable Automation: lit while automation is overridden somewhere.
    connect(bridge_, &EngineBridge::automationStateChanged, this, [this](const QString&) {
        const bool overridden = bridge_->hasOverrides();
        if (overridden == automationOverridden_) return;
        automationOverridden_ = overridden;
        Q_EMIT automationOverriddenChanged();
    });
    // A project's plug-ins loading after it opened.
    connect(bridge_, &EngineBridge::pluginsLoading, this, [this](int loaded, int total) {
        pluginsLoaded_ = loaded;
        pluginsTotal_ = total;
        Q_EMIT pluginsLoadingChanged();
    });

    // The title: the project's name, and whether it has changes to save.
    connect(undoStack_, &QUndoStack::cleanChanged, this, [this] {
        Q_EMIT cleanChanged();
        Q_EMIT titleChanged();
    });
    for (auto renamed : {&Project::reset, &Project::pathChanged}) connect(project_, renamed, this, &Session::titleChanged);

    // Renders in the background: one at a time, the arrangement's (reversing) too.
    arrangement_->setRenderStarter([this](Render* render) { return startRender(render); });
}

void Session::pruneSelection() { selection_->prune(*project_); }

void Session::setOwnerWindow(std::function<uintptr_t()> ownerWindow) { bridge_->setOwnerWindow(std::move(ownerWindow)); }

void Session::start() { bridge_->startAudio(); }

void Session::shutdown() {
    if (shutDown_) return;
    shutDown_ = true;
    if (rendering_) rendering_->abort();
    bridge_->stop();
    bridge_->stopPreview();
    bridge_->closeAllEditors();
    browser_->shutdown();
    engine_.closeDevice();
    bridge_->shutdown();  // unloads the plug-ins, while the application is still whole
}

bool Session::requestClose() {
    if (rendering_ && !rendering_->isFinished()) {  // (it ends first: its dialog's Cancel)
        render_->cancel();
        return false;
    }
    return true;
}

bool Session::startRender(Render* render) {
    if (rendering()) {
        delete render;
        return false;
    }
    rendering_ = render;
    connect(render, &Render::statusMessage, this, &Session::statusMessage);
    connect(render, &Render::warning, this, &Session::warning);
    render->start();
    return true;
}

bool Session::rendering() const { return rendering_ && !rendering_->isFinished(); }

void Session::setFreezeFinisher(std::function<std::optional<Freeze>(FreezeRender&)> finisher) {
    finishFreeze_ = std::move(finisher);
}

// --- What the window shows --------------------------------------------------------------------

QString Session::title() const {
    const QString path = project_->path();
    const QString name = path.isEmpty() ? QStringLiteral("Untitled") : fileStem(path);
    return QStringLiteral("%1%2 - %3").arg(name, clean() ? QString() : QStringLiteral("*"), QString::fromLatin1(kAppName));
}

bool Session::clean() const { return undoStack_->isClean(); }

QString Session::pluginsLoadingText() const {
    if (pluginsTotal_ <= 0) return {};
    return QStringLiteral("Loading plug-ins: %1 of %2").arg(pluginsLoaded_).arg(pluginsTotal_);
}

QString Session::aboutTitle() const { return QStringLiteral("About %1").arg(QString::fromLatin1(kAppName)); }

QString Session::aboutText() const {
    return QStringLiteral("<b>%1</b> %2<br>A basic DAW: Qt interface, C++ audio engine (miniaudio, WASAPI).<br><br>"
                          "Hosts VST3 instruments and effects.<br>"
                          "VST is a registered trademark of Steinberg Media Technologies GmbH.")
        .arg(QString::fromLatin1(kAppName), version());
}

double Session::recordQuantize() const { return sub::app::recordQuantize(); }

void Session::setRecordQuantize(double grid) {
    if (std::abs(grid - sub::app::recordQuantize()) < 1e-9) return;
    sub::app::setRecordQuantize(grid);
    Q_EMIT recordQuantizeChanged();
}

QVariantList Session::recordQuantizeChoices() const {
    QVariantList choices;
    for (const auto& [label, grid] : sub::app::recordQuantizeChoices()) {
        choices.append(QVariantMap{{QStringLiteral("label"), label}, {QStringLiteral("value"), grid}});
    }
    return choices;
}

}  // namespace sub::app
