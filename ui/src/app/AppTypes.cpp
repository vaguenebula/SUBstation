#include "AppTypes.h"

#include <QQmlEngine>
#include <QUndoStack>

#include "audio/EngineBridge.h"
#include "audio/RenderTask.h"
#include "browser/BrowserController.h"
#include "browser/ItemListModel.h"
#include "browser/SidebarModel.h"
#include "editor/ProjectEditor.h"
#include "intelligence/Harmony.h"
#include "intelligence/Humanizer.h"
#include "intelligence/SoundSimilarity.h"
#include "model/Project.h"
#include "plugins/PluginFolderModel.h"
#include "plugins/PluginIndex.h"
#include "plugins/PluginListModel.h"
#include "session/ArrangementActions.h"
#include "session/AudioPreferences.h"
#include "session/ComputerKeyboard.h"
#include "session/DeviceSelection.h"
#include "session/MidiPreferences.h"
#include "session/RenderProgress.h"
#include "session/Selection.h"
#include "session/Session.h"

namespace sub::ui {

namespace {

constexpr const char* kUri = "SUBstation";
const QString kMadeByTheSession = QStringLiteral("made by the session");

template <typename T>
void registerUncreatable(const char* name) {
    qmlRegisterUncreatableType<T>(kUri, 1, 0, name, kMadeByTheSession);
}

}  // namespace

void registerSession(sub::app::Session* session) {
    using namespace sub::app;
    registerUncreatable<Project>("Project");
    registerUncreatable<ProjectEditor>("ProjectEditor");
    registerUncreatable<Selection>("Selection");
    registerUncreatable<EngineBridge>("EngineBridge");
    registerUncreatable<RenderTask>("RenderTask");
    registerUncreatable<BrowserController>("BrowserController");
    registerUncreatable<ItemListModel>("ItemListModel");
    registerUncreatable<SidebarModel>("SidebarModel");
    registerUncreatable<PluginIndex>("PluginIndex");
    registerUncreatable<SoundSimilarity>("SoundSimilarity");
    registerUncreatable<Harmony>("Harmony");
    registerUncreatable<Humanizer>("Humanizer");
    registerUncreatable<PluginListModel>("PluginListModel");
    registerUncreatable<PluginFolderModel>("PluginFolderModel");
    registerUncreatable<ArrangementActions>("ArrangementActions");
    registerUncreatable<DeviceSelection>("DeviceSelection");
    registerUncreatable<RenderProgress>("RenderProgress");
    registerUncreatable<ComputerKeyboard>("ComputerKeyboard");
    registerUncreatable<AudioPreferences>("AudioPreferences");
    registerUncreatable<MidiPreferences>("MidiPreferences");
    qmlRegisterUncreatableType<QUndoStack>(kUri, 1, 0, "UndoStack", kMadeByTheSession);
    qmlRegisterSingletonInstance(kUri, 1, 0, "Session", session);
}

}  // namespace sub::ui
