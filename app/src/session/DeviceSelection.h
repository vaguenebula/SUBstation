#pragma once
// The device view without its widgets: the track it shows, which of its
// devices are selected, the rack chains it shows, the device clipboard, and
// what acts on them (Delete, Ctrl+G, Ctrl+Shift+G, Ctrl+C/X/V/D, folding,
// presets, drops). The device panel binds to it and calls it; the session
// dispatches the Edit menu's commands to it while the device view has the focus
// (Selection::Focus::Devices).
//
// The device view shows the selected track's chain (Selection::trackId(): a
// track, a return or the master; trackId() here, "" for none), and after each
// rack, unless it is folded, the chain it shows (the one last clicked in its
// chain list: shownChain(), else its first), and so on inside: shownDevices().
// A frozen track (or one in a frozen group) shows none (hint()).
//
// Selecting (selectDevice): a click selects a device, Ctrl adds or removes it,
// Shift selects the range from the last one clicked (the anchor); a selection
// is of one chain's devices. A press on a device already selected keeps the
// others (to drag them all), and if no drag follows, the release selects just
// it (press(), release()). Selecting devices focuses the device view
// (Selection::focusDevices: Delete and the clipboard act on them); selecting
// anything else (the focus going elsewhere) deselects them. Devices selected
// that are no longer shown (their chain hidden, their rack folded, or gone) are
// no longer selected. `selected` is in chain order (every device on the track,
// depth first).
//
// The clipboard holds the devices copied or cut (as they were: plug-ins in
// their state then) and which of them (and of what is in them) were folded:
// their copies are folded too. It is the device view's own (the arrangement's
// clips, automation and tracks have theirs).

#include <QHash>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include <optional>
#include <vector>

#include "model/Device.h"

namespace sub::app {

class EngineBridge;
class Project;
class ProjectEditor;
class Selection;

inline const QString kEffectsHint =
    QStringLiteral("Drop audio effects here from the browser (Built-in or Plug-ins › Audio Effects)");
inline const QString kInstrumentHint =
    QStringLiteral("Drop an instrument here from the browser (Built-in or Plug-ins › Instruments)");
inline const QString kInstrumentRefused = QStringLiteral("Instruments go on MIDI tracks (Create › Insert MIDI Track).");

class DeviceSelection : public QObject {
    Q_OBJECT
    // The track the device view shows ("": none).
    Q_PROPERTY(QString trackId READ trackId NOTIFY changed)
    // The selected devices, in chain order.
    Q_PROPERTY(QStringList selected READ selected NOTIFY changed)
    Q_PROPERTY(QString anchor READ anchor NOTIFY changed)
    // The rack chain last clicked (Ctrl+R renames it): its rack and its id ("": none).
    Q_PROPERTY(QString clickedRack READ clickedRack NOTIFY changed)
    Q_PROPERTY(QString clickedChain READ clickedChain NOTIFY changed)
    // The devices shown, in order (each rack's shown chain after it).
    Q_PROPERTY(QStringList shownDevices READ shownDevices NOTIFY changed)
    // What the view says beside (or instead of) the devices ("": nothing): no
    // track, frozen, an instrument wanted, or effects to drop.
    Q_PROPERTY(QString hint READ hint NOTIFY changed)
    Q_PROPERTY(bool hasClipboard READ hasClipboard NOTIFY clipboardChanged)

public:
    DeviceSelection(ProjectEditor* editor, Selection* selection, EngineBridge* bridge, QObject* parent = nullptr);

    QString trackId() const { return trackId_; }
    QStringList selected() const { return selected_; }
    QString anchor() const { return anchor_; }
    QString clickedRack() const { return clickedRack_; }
    QString clickedChain() const { return clickedChain_; }
    QStringList shownDevices() const { return shown_; }
    QString hint() const;
    bool hasClipboard() const { return !clipboard_.empty(); }
    const std::vector<Device>& clipboard() const { return clipboard_; }
    const QSet<QString>& clipboardFolded() const { return clipboardFolded_; }

    Q_INVOKABLE bool isSelected(const QString& deviceId) const { return selected_.contains(deviceId); }
    // The chain a rack shows (the one last clicked, else its first; "" for none).
    Q_INVOKABLE QString shownChain(const QString& rackId) const;
    // The chain a device is in ("": the track's own).
    Q_INVOKABLE QString containerOf(const QString& deviceId) const;
    // The devices of a chain of the track shown ("": its own), in order.
    Q_INVOKABLE QStringList chainDevices(const QString& chain) const;

    // --- Selecting ---
    // A click on a device: Shift selects the range from the last one clicked,
    // Ctrl adds or removes it, and a plain click selects just it (`modifiers`:
    // Qt::KeyboardModifiers).
    Q_INVOKABLE void selectDevice(const QString& deviceId, int modifiers = 0);
    // A press on a device: as a click, but a plain press on one already
    // selected keeps the others (for a drag) and only focuses the device view.
    Q_INVOKABLE void press(const QString& deviceId, int modifiers = 0);
    // Its release, if no drag followed: a plain one selects just it.
    Q_INVOKABLE void release(const QString& deviceId, int modifiers = 0);
    // Its menu: it is selected, unless it is already.
    Q_INVOKABLE void menuRequested(const QString& deviceId);
    // A click beside the devices: none selected, and the device view has the
    // focus (Ctrl+V pastes there).
    Q_INVOKABLE void clickBeside();
    // A rack's chain clicked in its chain list: shown beside it, and the one
    // Ctrl+R renames; the device view has the focus.
    Q_INVOKABLE void clickChain(const QString& rackId, const QString& chainId);
    // The devices a drag starting on this device moves: the selected ones (if
    // it is one of them), else it; not instruments (they stay first).
    Q_INVOKABLE QStringList dragDevices(const QString& deviceId) const;
    // Selects these devices (in chain order), for the tests and the view.
    void setSelected(const QStringList& deviceIds);

    // --- Acting on them ---
    // Delete: the selected devices (one undo step). False if there were none.
    Q_INVOKABLE bool deleteSelected();
    // Ctrl+G: the selected devices into a new rack, which is selected. False if none were selected.
    Q_INVOKABLE bool groupSelected();
    // A device's menu's Group: the selected devices if it is one of them, else it alone.
    Q_INVOKABLE void groupDevice(const QString& deviceId);
    // Ctrl+Shift+G: the selected racks go, their devices take their place. False if none was selected.
    Q_INVOKABLE bool ungroupSelected();
    // A rack's menu's Ungroup.
    Q_INVOKABLE void ungroupRack(const QString& rackId);
    // Fold or unfold a device; when it is one of several selected, they all take its new state.
    Q_INVOKABLE void toggleFold(const QString& deviceId);
    // Ctrl+C: the selected devices (plug-ins as they are now). False if none were selected.
    Q_INVOKABLE bool copySelected();
    // Ctrl+X: copied, then deleted. False if none were selected.
    Q_INVOKABLE bool cutSelected();
    // Ctrl+V: new devices like those copied, after the selected devices (if
    // any; else last), and selected. False if nothing was pasted.
    Q_INVOKABLE bool paste();
    // New devices like those copied into a chain of the track shown (`chain`,
    // "": its own) before the device at `index` (< 0: last), and selected.
    Q_INVOKABLE bool pasteAt(const QString& chain, int index);
    // A device's menu's Paste: right after it.
    Q_INVOKABLE bool pasteAfter(const QString& deviceId);
    // Ctrl+D: copies of the selected devices right after them (what was copied stays copied).
    Q_INVOKABLE bool duplicateSelected();
    // Ctrl+R: the rack chain last clicked, if it shows: {trackId, rackId, chainId, name}; {} for none.
    Q_INVOKABLE QVariantMap renameTarget() const;

    // --- Presets ---
    // A preset file into a chain of the track shown (`chain`, "": its own)
    // before the device at `index` (< 0: last), as a new device. One undo step.
    Q_INVOKABLE bool insertPreset(const QString& path, const QString& chain = {}, int index = -1);
    // A preset loaded into a device of its kind on the track shown (it stays,
    // with its new settings). One undo step.
    Q_INVOKABLE bool loadPresetInto(const QString& deviceId, const QString& path);
    // Whether a preset dragged over a device loads into it (of its kind),
    // rather than going in beside it (read once per drag: dragEnded() forgets).
    Q_INVOKABLE bool presetLoadsInto(const QString& path, const QString& deviceId);
    // Load Preset… (the UI asks for the file, in presetFolder()): as
    // insertPreset, and the folder is remembered.
    Q_INVOKABLE bool loadPresetFile(const QString& path, const QString& chain = {}, int index = -1);
    // Where preset files are loaded from: where the last one was, else the library (else Documents).
    Q_INVOKABLE QString presetFolder() const;
    // The save button: a device's name as a preset (the name offered).
    Q_INVOKABLE QString presetName(const QString& deviceId) const;
    // Before saving a device as a preset called `name`: {error} (a name that
    // can't be a file's), or {exists: true, question} (the UI asks it before
    // replacing), or {} (go ahead).
    Q_INVOKABLE QVariantMap checkPresetName(const QString& deviceId, const QString& name) const;
    // Saves a device (a rack with everything in it, plug-ins as they are now)
    // as a preset in the library under `name` (replacing one of that name):
    // its file ("" if not saved: a statusMessage says why). A rack takes the
    // preset's name (an undo step "Save Preset <name>"). presetSaved.
    Q_INVOKABLE QString savePreset(const QString& deviceId, const QString& name);
    // The device as the default preset of its kind: new devices of that kind
    // (that plug-in) start as it is now. Its file ("": not saved).
    Q_INVOKABLE QString saveAsDefault(const QString& deviceId);
    // New devices of this kind start as they come again.
    Q_INVOKABLE void clearDefault(const QString& deviceId);
    Q_INVOKABLE bool hasDefault(const QString& deviceId) const;

    // --- A plug-in's own presets (.vstpreset files, to share settings with other hosts) ---
    // Where their file dialogs start: where the last one was, else Documents/VST3
    // Presets/<vendor>/<plug-in> (where VST3 presets usually live), else Documents.
    Q_INVOKABLE QString vst3PresetFolder(const QString& deviceId) const;
    // Load VST3 Preset… (the UI asks for the file): its settings into the
    // plug-in, then one undo step ("Load Preset <name>") with the state before
    // and after. Another plug-in's file changes nothing (a statusMessage says
    // why). The folder is remembered. Whether it loaded.
    Q_INVOKABLE bool loadVst3Preset(const QString& deviceId, const QString& path);
    // Save VST3 Preset…: the plug-in's state as a .vstpreset (the save button
    // saves a SUBstation preset). The folder is remembered. Whether it was saved.
    Q_INVOKABLE bool saveVst3Preset(const QString& deviceId, const QString& path);

    // --- Drops on the device view ---
    // Devices of this track's chain dragged to `index` of `chain` ("": its own).
    Q_INVOKABLE bool dropMoved(const QStringList& deviceIds, const QString& chain, int index);
    // Built-in devices and plug-ins (PluginRef fields) from the browser: new
    // devices where they were dropped (each after the one before; an instrument
    // always goes first).
    Q_INVOKABLE void dropDevices(const QStringList& kinds, const QVariantList& plugins, const QString& chain,
                                 int index);
    // Presets from the browser: into `intoDeviceId` (one preset of its kind
    // dropped onto it: presetLoadsInto), else new devices where they were dropped.
    Q_INVOKABLE void dropPresets(const QStringList& paths, const QString& chain, int index,
                                 const QString& intoDeviceId = {});
    // A drag over the device view ended (dropped or left).
    Q_INVOKABLE void dragEnded() { dragPresets_.clear(); }

Q_SIGNALS:
    void changed();
    void clipboardChanged();
    void statusMessage(const QString& message);
    void presetSaved(const QString& path);  // a device was saved to the preset library (its file)

private:
    const std::vector<Device>* devices() const;
    QStringList chainIds() const;  // every device on the track, depth first
    QStringList computeShown() const;
    void showTrack(const QString& trackId);
    void rebuild();
    void onSelectionChanged();
    void onDevicesChanged(const QString& trackId);
    void applySelected(const QStringList& deviceIds);
    std::optional<Device> readPreset(const QString& path);
    bool frozen() const;

    ProjectEditor* editor_;
    Project* project_;
    Selection* selection_;
    EngineBridge* bridge_;
    QString trackId_;
    QStringList selected_;
    QString anchor_;
    QString clickedRack_;
    QString clickedChain_;
    QMap<QString, QString> shownChains_;  // rack id -> the chain it shows (view state)
    QStringList shown_;
    std::vector<Device> clipboard_;
    QSet<QString> clipboardFolded_;
    QHash<QString, std::optional<Device>> dragPresets_;  // the presets dragged over the view, read (none: unreadable)
};

}  // namespace sub::app
