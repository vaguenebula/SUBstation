#include "session/DeviceSelection.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QUndoStack>

#include <algorithm>
#include <functional>

#include "audio/EngineBridge.h"
#include "editor/ProjectEditor.h"
#include "io/Presets.h"
#include "io/Serialization.h"
#include "model/Devices.h"
#include "model/Errors.h"
#include "model/Project.h"
#include "session/Selection.h"
#include "session/SessionSupport.h"

namespace sub::app {

namespace {

constexpr const char* kPresetFolderKey = "presets/dir";
constexpr const char* kVst3PresetFolderKey = "plugins/preset_dir";
const QString kNestedTooDeep = QStringLiteral("The preset can't go there: racks nest at most 8 deep.");

bool has(int modifiers, Qt::KeyboardModifier modifier) { return (modifiers & modifier) != 0; }

}  // namespace

DeviceSelection::DeviceSelection(ProjectEditor* editor, Selection* selection, EngineBridge* bridge, QObject* parent)
    : QObject(parent), editor_(editor), project_(editor->project()), selection_(selection), bridge_(bridge) {
    connect(selection_, &Selection::changed, this, &DeviceSelection::onSelectionChanged);
    connect(project_, &Project::devicesChanged, this, &DeviceSelection::onDevicesChanged);
    for (auto viewChanged : {&Project::devicesFolded, &Project::rackViewChanged}) {
        connect(project_, viewChanged, this, [this](const QString& trackId) {
            if (trackId == trackId_) rebuild();
        });
    }
    // (It, or its group.)
    connect(project_, &Project::freezeChanged, this, [this](const QString&) { rebuild(); });
    connect(project_, &Project::reset, this, [this] { showTrack({}); });
    for (auto removed : {&Project::trackRemoved, &Project::returnRemoved}) {
        connect(project_, removed, this, [this](const QString& trackId, int) {
            if (trackId == trackId_) showTrack({});
        });
    }
    // Its devices' processors were (re)created, or a plug-in's parameters changed: shown again.
    connect(bridge_, &EngineBridge::devicesLoaded, this, [this](const QString& trackId) {
        if (trackId == trackId_) rebuild();
    });
    connect(bridge_, &EngineBridge::pluginParamsRebuilt, this, [this](const QString& trackId, const QString&) {
        if (trackId == trackId_) rebuild();
    });
    showTrack(selection_->trackId());
}

// --- What it shows ----------------------------------------------------------------------------

const std::vector<Device>* DeviceSelection::devices() const {
    const Track* track = trackId_.isEmpty() ? nullptr : project_->findTrack(trackId_);
    return track != nullptr ? &track->devices : nullptr;
}

QStringList DeviceSelection::chainIds() const {
    QStringList ids;
    if (const auto* list = devices()) {
        for (const Device* device : iterDevices(*list)) ids.append(device->id);
    }
    return ids;
}

QString DeviceSelection::containerOf(const QString& deviceId) const {
    const auto* list = devices();
    if (list == nullptr) return {};
    return sub::app::containerOf(*list, deviceId).value_or(QString());
}

QStringList DeviceSelection::chainDevices(const QString& chain) const {
    QStringList ids;
    const auto* list = devices();
    if (list == nullptr) return ids;
    const auto* devices = sub::app::chainDevices(*list, chain.isEmpty() ? std::nullopt : std::optional<QString>(chain));
    if (devices != nullptr) {
        for (const Device& device : *devices) ids.append(device.id);
    }
    return ids;
}

QString DeviceSelection::shownChain(const QString& rackId) const {
    const auto* list = devices();
    const Device* rack = list != nullptr ? findDevice(*list, rackId) : nullptr;
    if (rack == nullptr || rack->chains.empty()) return {};
    const QString shown = shownChains_.value(rackId);
    for (const Chain& chain : rack->chains) {
        if (chain.id == shown) return shown;
    }
    return rack->chains.front().id;
}

bool DeviceSelection::frozen() const { return !trackId_.isEmpty() && project_->frozenBy(trackId_).has_value(); }

// The devices shown: each, then (a rack not folded, showing its devices) those in the chain it shows.
QStringList DeviceSelection::computeShown() const {
    QStringList shown;
    const auto* list = devices();
    if (list == nullptr || frozen()) return shown;
    std::function<void(const std::vector<Device>&)> add = [&](const std::vector<Device>& chain) {
        for (const Device& device : chain) {
            shown.append(device.id);
            if (!device.isRack() || device.chains.empty() || project_->isDeviceFolded(device.id) ||
                !project_->areRackDevicesShown(device.id)) {
                continue;
            }
            const QString chainId = shownChain(device.id);
            for (const Chain& rackChain : device.chains) {
                if (rackChain.id == chainId) add(rackChain.devices);
            }
        }
    };
    add(*list);
    return shown;
}

QString DeviceSelection::hint() const {
    if (trackId_.isEmpty()) return QStringLiteral("No track selected");
    const Track* track = project_->findTrack(trackId_);
    if (track == nullptr) return QStringLiteral("No track selected");
    if (const auto holder = project_->frozenBy(trackId_)) {
        if (*holder == trackId_) {
            return QStringLiteral("%1 is frozen: unfreeze it (Ctrl+Shift+F) to change its devices").arg(track->name);
        }
        const QString name = project_->track(*holder).name;
        return QStringLiteral("%1 is in %2, which is frozen: unfreeze %2 (Ctrl+Shift+F) to change its devices")
            .arg(track->name, name);
    }
    const bool needsInstrument = track->isMidi() && std::none_of(track->devices.begin(), track->devices.end(),
                                                                 [](const Device& d) { return deviceIsInstrument(d); });
    if (needsInstrument) return kInstrumentHint;
    return track->devices.empty() ? kEffectsHint : QString();
}

// Shows a track's devices ("": none); the devices still shown stay selected.
void DeviceSelection::showTrack(const QString& given) {
    const QString trackId = !given.isEmpty() && project_->hasOwner(given) ? given : QString();
    if (trackId != trackId_) {
        selected_.clear();
        anchor_.clear();
        clickedRack_.clear();
        clickedChain_.clear();
    }
    trackId_ = trackId;
    shown_ = computeShown();
    if (trackId_.isEmpty() || frozen()) {
        selected_.clear();
        Q_EMIT changed();
        return;
    }
    QStringList kept;
    for (const QString& id : selected_) {
        if (shown_.contains(id)) kept.append(id);
    }
    selected_.clear();
    applySelected(kept);
}

void DeviceSelection::rebuild() { showTrack(trackId_); }

void DeviceSelection::onSelectionChanged() {
    if (selection_->trackId() != trackId_) {
        showTrack(selection_->trackId());
    } else if (selection_->focus() != Selection::Focus::Devices && !selected_.isEmpty()) {
        applySelected({});  // the user went on to select something else
    }
}

void DeviceSelection::onDevicesChanged(const QString& trackId) {
    if (trackId != trackId_) return;
    if (computeShown() == shown_) {
        Q_EMIT changed();  // the same devices in the same places (one switched on or off, macros mapped...)
        return;
    }
    rebuild();
}

// --- Selecting --------------------------------------------------------------------------------

void DeviceSelection::applySelected(const QStringList& deviceIds) {
    const QSet<QString> wanted(deviceIds.begin(), deviceIds.end());
    selected_.clear();
    for (const QString& id : chainIds()) {
        if (wanted.contains(id)) selected_.append(id);
    }
    if (!wanted.isEmpty()) {
        clickedRack_.clear();
        clickedChain_.clear();
    }
    Q_EMIT changed();
    if (!selected_.isEmpty()) selection_->focusDevices();
}

void DeviceSelection::setSelected(const QStringList& deviceIds) { applySelected(deviceIds); }

void DeviceSelection::selectDevice(const QString& deviceId, int modifiers) {
    const QStringList chain = chainDevices(containerOf(deviceId));  // a selection is of one chain's devices
    if (has(modifiers, Qt::ShiftModifier) && !anchor_.isEmpty() && chain.contains(anchor_)) {
        const auto a = chain.indexOf(anchor_);
        const auto b = chain.indexOf(deviceId);
        applySelected(chain.mid(std::min(a, b), std::abs(b - a) + 1));
        return;
    }
    if (has(modifiers, Qt::ControlModifier)) {
        QStringList toggled;
        for (const QString& id : selected_) {
            if (chain.contains(id) && id != deviceId) toggled.append(id);
        }
        if (!selected_.contains(deviceId)) toggled.append(deviceId);
        applySelected(toggled);
    } else {
        applySelected({deviceId});
    }
    anchor_ = deviceId;
    Q_EMIT changed();
}

void DeviceSelection::press(const QString& deviceId, int modifiers) {
    const bool extend = has(modifiers, Qt::ShiftModifier) || has(modifiers, Qt::ControlModifier);
    if (extend || !selected_.contains(deviceId)) {
        selectDevice(deviceId, modifiers);
    } else {
        selection_->focusDevices();
    }
}

void DeviceSelection::release(const QString& deviceId, int modifiers) {
    const bool extend = has(modifiers, Qt::ShiftModifier) || has(modifiers, Qt::ControlModifier);
    if (!extend && selected_.size() > 1) selectDevice(deviceId);
}

void DeviceSelection::menuRequested(const QString& deviceId) {
    if (!selected_.contains(deviceId)) selectDevice(deviceId);
}

void DeviceSelection::clickBeside() {
    applySelected({});
    clickedRack_.clear();
    clickedChain_.clear();
    Q_EMIT changed();
    if (!trackId_.isEmpty()) selection_->focusDevices();  // (Ctrl+V pastes here)
}

void DeviceSelection::clickChain(const QString& rackId, const QString& chainId) {
    if (!trackId_.isEmpty()) editor_->setRackDevicesShown(trackId_, rackId, true);  // (its devices, as asked for)
    const bool showing = shown_.contains(rackId) && !project_->isDeviceFolded(rackId) && shownChain(rackId) == chainId;
    if (shownChains_.value(rackId) != chainId || !showing) {
        shownChains_.insert(rackId, chainId);
        rebuild();
    }
    clickedRack_ = rackId;
    clickedChain_ = chainId;
    Q_EMIT changed();
    selection_->focusDevices();
}

QStringList DeviceSelection::dragDevices(const QString& deviceId) const {
    QStringList moving;
    const auto* list = devices();
    if (list == nullptr) return moving;
    for (const QString& id : selected_.contains(deviceId) ? selected_ : QStringList{deviceId}) {
        const Device* device = findDevice(*list, id);
        if (device != nullptr && !deviceIsInstrument(*device)) moving.append(id);
    }
    return moving;
}

QVariantMap DeviceSelection::renameTarget() const {
    if (clickedRack_.isEmpty() || !shown_.contains(clickedRack_)) return {};
    const auto* list = devices();
    const Device* rack = list != nullptr ? findDevice(*list, clickedRack_) : nullptr;
    if (rack == nullptr || project_->isDeviceFolded(rack->id)) return {};
    for (const Chain& chain : rack->chains) {
        if (chain.id == clickedChain_) {
            return {{QStringLiteral("trackId"), trackId_},
                    {QStringLiteral("rackId"), clickedRack_},
                    {QStringLiteral("chainId"), clickedChain_},
                    {QStringLiteral("name"), chain.name}};
        }
    }
    return {};
}

// --- Acting on them ---------------------------------------------------------------------------

bool DeviceSelection::deleteSelected() {
    if (trackId_.isEmpty() || selected_.isEmpty()) return false;
    const QStringList deviceIds = selected_;
    selected_.clear();
    Q_EMIT changed();
    editor_->removeDevices(trackId_, deviceIds);
    return true;
}

bool DeviceSelection::groupSelected() {
    if (trackId_.isEmpty() || selected_.isEmpty()) return false;
    const QString rack = editor_->groupDevices(trackId_, selected_);
    if (rack.isEmpty()) {
        Q_EMIT statusMessage(QStringLiteral("Racks nest at most 8 deep."));
        return false;
    }
    selectDevice(rack);
    return true;
}

void DeviceSelection::groupDevice(const QString& deviceId) {
    if (trackId_.isEmpty()) return;
    if (selected_.contains(deviceId)) {
        groupSelected();
    } else {
        editor_->groupDevices(trackId_, {deviceId});
    }
}

bool DeviceSelection::ungroupSelected() {
    QStringList racks;
    if (!trackId_.isEmpty()) {
        for (const QString& id : selected_) {
            const Device* device = project_->findDevice(trackId_, id);
            if (device != nullptr && device->isRack()) racks.append(id);
        }
    }
    if (racks.isEmpty()) return false;
    selected_.clear();
    Q_EMIT changed();
    QUndoStack* stack = editor_->undoStack();
    stack->beginMacro(racks.size() == 1 ? QStringLiteral("Ungroup Rack") : QStringLiteral("Ungroup Racks"));
    for (const QString& rackId : racks) {
        if (!editor_->ungroupRack(trackId_, rackId)) {
            Q_EMIT statusMessage(QStringLiteral("A rack of several instruments can't be ungrouped: a chain has one."));
        }
    }
    stack->endMacro();
    return true;
}

void DeviceSelection::ungroupRack(const QString& rackId) {
    if (trackId_.isEmpty()) return;
    if (!editor_->ungroupRack(trackId_, rackId)) {
        Q_EMIT statusMessage(QStringLiteral("A rack of several instruments can't be ungrouped: a chain has one."));
    }
}

void DeviceSelection::toggleFold(const QString& deviceId) {
    if (trackId_.isEmpty() || !project_->hasDevice(trackId_, deviceId)) return;
    const QStringList devices = selected_.contains(deviceId) ? selected_ : QStringList{deviceId};
    editor_->setDevicesFolded(trackId_, devices, !project_->isDeviceFolded(deviceId));
}

void DeviceSelection::toggleChainList(const QString& rackId) {
    if (!trackId_.isEmpty()) editor_->setChainListShown(trackId_, rackId, !project_->isChainListShown(rackId));
}

void DeviceSelection::toggleRackDevices(const QString& rackId) {
    if (!trackId_.isEmpty()) editor_->setRackDevicesShown(trackId_, rackId, !project_->areRackDevicesShown(rackId));
}

bool DeviceSelection::copySelected() {
    if (trackId_.isEmpty() || selected_.isEmpty()) return false;
    const QSet<QString> inside = deviceIdsOfList(editor_->copyDevices(trackId_, selected_));
    bridge_->storePluginStates(inside);  // (the plug-ins' states as they are now)
    clipboard_ = editor_->copyDevices(trackId_, selected_);
    clipboardFolded_.clear();
    for (const QString& id : inside) {
        if (project_->isDeviceFolded(id)) clipboardFolded_.insert(id);
    }
    Q_EMIT clipboardChanged();
    return true;
}

bool DeviceSelection::cutSelected() { return copySelected() && deleteSelected(); }

bool DeviceSelection::paste() {
    if (clipboard_.empty()) {
        Q_EMIT statusMessage(QStringLiteral("Nothing to paste: copy (Ctrl+C) or cut (Ctrl+X) devices first."));
        return false;
    }
    if (trackId_.isEmpty()) {
        Q_EMIT statusMessage(QStringLiteral("Select a track to paste the devices onto."));
        return false;
    }
    if (selected_.isEmpty()) return pasteAt({}, -1);
    const QString last = selected_.back();
    const QString chain = containerOf(last);
    return pasteAt(chain, static_cast<int>(chainDevices(chain).indexOf(last)) + 1);
}

bool DeviceSelection::pasteAt(const QString& chain, int index) {
    if (clipboard_.empty()) {
        Q_EMIT statusMessage(QStringLiteral("Nothing to paste: copy (Ctrl+C) or cut (Ctrl+X) devices first."));
        return false;
    }
    if (trackId_.isEmpty()) {
        Q_EMIT statusMessage(QStringLiteral("Select a track to paste the devices onto."));
        return false;
    }
    const QStringList pasted = editor_->pasteDevices(trackId_, clipboard_, index, chain, clipboardFolded_);
    if (pasted.size() < static_cast<qsizetype>(clipboard_.size())) {
        const Track* track = project_->findTrack(trackId_);
        const bool instrumentRefused =
            (track == nullptr || !track->isMidi()) &&
            std::any_of(clipboard_.begin(), clipboard_.end(), [](const Device& d) { return deviceIsInstrument(d); });
        Q_EMIT statusMessage(instrumentRefused ? kInstrumentRefused
                                               : QStringLiteral("The devices can't all go there: racks nest at most 8 deep."));
    }
    if (pasted.isEmpty()) return false;
    applySelected(pasted);
    anchor_ = pasted.back();
    Q_EMIT changed();
    return true;
}

bool DeviceSelection::pasteAfter(const QString& deviceId) {
    if (trackId_.isEmpty() || !project_->hasDevice(trackId_, deviceId)) return false;
    const QString chain = containerOf(deviceId);
    return pasteAt(chain, static_cast<int>(chainDevices(chain).indexOf(deviceId)) + 1);
}

bool DeviceSelection::duplicateSelected() {
    const std::vector<Device> clipboard = clipboard_;
    const QSet<QString> folded = clipboardFolded_;
    const bool duplicated = copySelected() && paste();
    clipboard_ = clipboard;
    clipboardFolded_ = folded;
    Q_EMIT clipboardChanged();
    return duplicated;
}

// --- Presets ----------------------------------------------------------------------------------

std::optional<Device> DeviceSelection::readPreset(const QString& path) {
    try {
        return loadPreset(path);
    } catch (const ProjectFileError& error) {
        Q_EMIT statusMessage(error.message());
        return std::nullopt;
    }
}

bool DeviceSelection::insertPreset(const QString& path, const QString& chain, int index) {
    const std::optional<Device> device = readPreset(path);
    if (!device || trackId_.isEmpty()) return false;
    if (!editor_->insertDevice(trackId_, *device, index, chain, QStringLiteral("Load Preset ") + fileStem(path),
                               !device->isRack())) {
        Q_EMIT statusMessage(deviceIsInstrument(*device) ? kInstrumentRefused : kNestedTooDeep);
        return false;
    }
    return true;
}

bool DeviceSelection::loadPresetInto(const QString& deviceId, const QString& path) {
    const std::optional<Device> preset = readPreset(path);
    if (!preset || trackId_.isEmpty()) return false;
    const Device* device = project_->findDevice(trackId_, deviceId);
    if (device == nullptr) return false;
    if (!loadsInto(*preset, *device)) {
        Q_EMIT statusMessage(QStringLiteral("A %1 preset can't load into %2.").arg(deviceName(*preset), deviceName(*device)));
        return false;
    }
    bridge_->storePluginStates(QSet<QString>{deviceId});  // (to undo to)
    if (!editor_->loadPresetInto(trackId_, deviceId, *preset, QStringLiteral("Load Preset ") + fileStem(path))) {
        Q_EMIT statusMessage(kNestedTooDeep);
        return false;
    }
    return true;
}

bool DeviceSelection::presetLoadsInto(const QString& path, const QString& deviceId) {
    if (trackId_.isEmpty()) return false;
    if (!dragPresets_.contains(path)) {
        try {
            dragPresets_.insert(path, loadPreset(path));
        } catch (const ProjectFileError&) {
            dragPresets_.insert(path, std::nullopt);
        }
    }
    const std::optional<Device>& preset = dragPresets_[path];
    const Device* device = project_->findDevice(trackId_, deviceId);
    return preset && device != nullptr && loadsInto(*preset, *device);
}

bool DeviceSelection::loadPresetFile(const QString& path, const QString& chain, int index) {
    if (path.isEmpty() || trackId_.isEmpty()) return false;
    QSettings().setValue(QString::fromLatin1(kPresetFolderKey), QFileInfo(path).absolutePath());
    return insertPreset(path, chain, index);
}

QString DeviceSelection::presetFolder() const {
    const QString stored = QSettings().value(QString::fromLatin1(kPresetFolderKey)).toString();
    if (!stored.isEmpty() && QFileInfo(stored).isDir()) return stored;
    const QString library = libraryDir();
    return QFileInfo(library).isDir() ? library : QDir::homePath() + QStringLiteral("/Documents");
}

QString DeviceSelection::presetName(const QString& deviceId) const {
    const Device* device = trackId_.isEmpty() ? nullptr : project_->findDevice(trackId_, deviceId);
    return device != nullptr ? deviceName(*device) : QString();
}

QVariantMap DeviceSelection::checkPresetName(const QString& deviceId, const QString& name) const {
    const Device* device = trackId_.isEmpty() ? nullptr : project_->findDevice(trackId_, deviceId);
    if (device == nullptr) return {{QStringLiteral("error"), QStringLiteral("There is no such device.")}};
    QString path;
    try {
        path = presetPath(*device, name.trimmed());
    } catch (const EditError& error) {
        return {{QStringLiteral("error"), error.message()}};
    }
    if (!QFileInfo::exists(path)) return {{QStringLiteral("path"), path}};
    const QFileInfo info(path);
    return {{QStringLiteral("path"), path},
            {QStringLiteral("exists"), true},
            {QStringLiteral("question"), QStringLiteral("There is a %1 preset called “%2” already. Replace it?")
                                             .arg(info.dir().dirName(), fileStem(path))}};
}

QString DeviceSelection::savePreset(const QString& deviceId, const QString& given) {
    if (trackId_.isEmpty() || !project_->hasDevice(trackId_, deviceId)) return {};
    bridge_->storePluginStates(deviceIdsOf(project_->device(trackId_, deviceId)));  // (as they are now)
    const Device device = project_->device(trackId_, deviceId);
    const QString name = given.trimmed();
    QString path;
    try {
        path = presetPath(device, name);
    } catch (const EditError& error) {
        Q_EMIT statusMessage(error.message());
        return {};
    }
    try {
        saveToLibrary(device, name);
    } catch (const ProjectFileError& error) {
        Q_EMIT statusMessage(QStringLiteral("Could not save the preset: ") + error.message());
        return {};
    } catch (const EditError& error) {
        Q_EMIT statusMessage(QStringLiteral("Could not save the preset: ") + error.message());
        return {};
    }
    const QString stem = fileStem(path);
    Q_EMIT statusMessage(QStringLiteral("Saved the preset %1 (%2).").arg(stem, QFileInfo(path).dir().dirName()));
    if (device.isRack()) editor_->renameRack(trackId_, deviceId, stem, QStringLiteral("Save Preset ") + stem);  // (a rack is named as its preset)
    Q_EMIT presetSaved(path);
    return path;
}

QString DeviceSelection::saveAsDefault(const QString& deviceId) {
    if (trackId_.isEmpty() || !project_->hasDevice(trackId_, deviceId)) return {};
    bridge_->storePluginStates(QSet<QString>{deviceId});
    const Device device = project_->device(trackId_, deviceId);
    QString path;
    try {
        path = saveDefault(device);
    } catch (const ProjectFileError& error) {
        Q_EMIT statusMessage(QStringLiteral("Could not save the default preset: ") + error.message());
        return {};
    } catch (const EditError& error) {
        Q_EMIT statusMessage(QStringLiteral("Could not save the default preset: ") + error.message());
        return {};
    }
    Q_EMIT statusMessage(QStringLiteral("New %1 devices will start like this one.").arg(deviceName(device)));
    return path;
}

void DeviceSelection::clearDefault(const QString& deviceId) {
    const Device* device = trackId_.isEmpty() ? nullptr : project_->findDevice(trackId_, deviceId);
    if (device == nullptr) return;
    if (sub::app::clearDefault(device->kind, device->plugin)) {
        Q_EMIT statusMessage(QStringLiteral("New %1 devices will start as they come.").arg(deviceName(*device)));
    }
}

bool DeviceSelection::hasDefault(const QString& deviceId) const {
    const Device* device = trackId_.isEmpty() ? nullptr : project_->findDevice(trackId_, deviceId);
    return device != nullptr && sub::app::hasDefault(device->kind, device->plugin);
}

// --- A plug-in's own presets ------------------------------------------------------------------

QString DeviceSelection::vst3PresetFolder(const QString& deviceId) const {
    const QString stored = QSettings().value(QString::fromLatin1(kVst3PresetFolderKey)).toString();
    if (!stored.isEmpty() && QFileInfo(stored).isDir()) return stored;
    const QString documents = QDir::homePath() + QStringLiteral("/Documents");
    const Device* device = trackId_.isEmpty() ? nullptr : project_->findDevice(trackId_, deviceId);
    if (device == nullptr || !device->plugin) return documents;
    const PluginRef& plugin = *device->plugin;
    const QString folder = QStringLiteral("%1/VST3 Presets/%2/%3")
                               .arg(documents, plugin.vendor.isEmpty() ? QStringLiteral("Unknown") : plugin.vendor,
                                    plugin.name);
    return QFileInfo(folder).isDir() ? folder : documents;
}

bool DeviceSelection::loadVst3Preset(const QString& deviceId, const QString& path) {
    const Device* device = trackId_.isEmpty() ? nullptr : project_->findDevice(trackId_, deviceId);
    if (path.isEmpty() || device == nullptr || !device->isPlugin()) return false;
    QSettings().setValue(QString::fromLatin1(kVst3PresetFolderKey), QFileInfo(path).absolutePath());
    const QString fileName = QFileInfo(path).fileName();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        Q_EMIT statusMessage(QStringLiteral("Could not load %1: %2").arg(fileName, file.errorString()));
        return false;
    }
    const QByteArray data = file.readAll();
    const std::optional<QByteArray> old = bridge_->pluginState(trackId_, deviceId);
    // (It fails for another plug-in's settings.)
    const QString problem = bridge_->applyPluginState(trackId_, deviceId, data);
    if (!problem.isEmpty()) {
        Q_EMIT statusMessage(QStringLiteral("Could not load %1: %2").arg(fileName, problem));
        return false;
    }
    auto encoded = [](const std::optional<QByteArray>& state) -> std::optional<QString> {
        if (!state || state->isEmpty()) return std::nullopt;
        return QString::fromLatin1(state->toBase64());
    };
    editor_->setDeviceState(trackId_, deviceId, encoded(old), encoded(data), QStringLiteral("Load Preset ") + fileStem(path));
    return true;
}

bool DeviceSelection::saveVst3Preset(const QString& deviceId, const QString& path) {
    const Device* device = trackId_.isEmpty() ? nullptr : project_->findDevice(trackId_, deviceId);
    if (path.isEmpty() || device == nullptr || !device->isPlugin()) return false;
    QSettings().setValue(QString::fromLatin1(kVst3PresetFolderKey), QFileInfo(path).absolutePath());
    const std::optional<QByteArray> state = bridge_->pluginState(trackId_, deviceId);
    if (!state) return false;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(*state) != state->size()) {
        Q_EMIT statusMessage(QStringLiteral("Could not save the preset: ") + file.errorString());
        return false;
    }
    return true;
}

// --- Drops ------------------------------------------------------------------------------------

bool DeviceSelection::dropMoved(const QStringList& deviceIds, const QString& chain, int index) {
    if (trackId_.isEmpty()) return false;
    const bool moved = editor_->moveDevices(trackId_, deviceIds, index, chain);
    if (!moved && !chain.isEmpty()) {
        Q_EMIT statusMessage(QStringLiteral("A rack can't go into itself, and racks nest at most 8 deep."));
    }
    return moved;
}

void DeviceSelection::dropDevices(const QStringList& kinds, const QVariantList& plugins, const QString& chain,
                                  int index) {
    if (trackId_.isEmpty()) return;
    // New effects go where they were dropped (an instrument always goes first).
    std::vector<std::pair<QString, std::optional<PluginRef>>> added;
    for (const QString& kind : kinds) {
        if (builtinDevice(kind) != nullptr) added.emplace_back(kind, std::nullopt);
    }
    for (const PluginRef& ref : pluginRefsFromList(plugins)) added.emplace_back(kPluginKind, ref);
    bool refused = false;
    for (const auto& [kind, plugin] : added) {
        const auto count = chainDevices(chain).size();
        const QString deviceId = editor_->addDevice(trackId_, kind, index, plugin, chain);
        refused = refused || deviceId.isEmpty();
        const QStringList now = chainDevices(chain);
        const Device* device = deviceId.isEmpty() ? nullptr : project_->findDevice(trackId_, deviceId);
        if (device != nullptr && !deviceIsInstrument(*device)) {
            index = static_cast<int>(now.indexOf(deviceId)) + 1;  // the next one goes after it
        } else {  // a new instrument (not one replacing another) went in first, before the drop point
            index += static_cast<int>(now.size() - count);
        }
    }
    if (refused) Q_EMIT statusMessage(kInstrumentRefused);
}

void DeviceSelection::dropPresets(const QStringList& paths, const QString& chain, int index,
                                  const QString& intoDeviceId) {
    if (trackId_.isEmpty() || paths.isEmpty()) return;
    if (!intoDeviceId.isEmpty() && paths.size() == 1 && presetLoadsInto(paths.front(), intoDeviceId)) {
        dragPresets_.clear();
        loadPresetInto(intoDeviceId, paths.front());
        return;
    }
    dragPresets_.clear();
    for (const QString& path : paths) {
        const auto count = chainDevices(chain).size();
        if (insertPreset(path, chain, index)) index += static_cast<int>(chainDevices(chain).size() - count);  // (the next one goes after it)
    }
}

}  // namespace sub::app
