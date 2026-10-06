// The device view's selection and actions (DeviceSelection) through the
// session: selecting devices (Shift, Ctrl, press and release), the focus
// going elsewhere, Ctrl+C/X/V/D on devices (the device view's own clipboard),
// folding, racks (grouping, ungrouping, the chain shown and the one clicked,
// Ctrl+R), drops, what the view says; and presets: saving devices to the
// library (the browser lists them), loading them as new devices, into devices
// of their kind, onto tracks, default presets, renaming them in the browser,
// and the preset index watching the library.

#include "SessionFixture.h"
#include "TestSupport.h"

#include "browser/BrowserController.h"
#include "browser/ItemListModel.h"
#include "browser/PresetIndex.h"
#include "io/Presets.h"
#include "io/Serialization.h"
#include "model/Devices.h"
#include "session/ArrangementActions.h"
#include "session/DeviceSelection.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QSignalSpy>
#include <QTest>

using namespace sub::app;
using sub::app::test::SessionFixture;
using sub::app::test::TempDir;

namespace {

QStringList ids(const std::vector<Device>& devices) {
    QStringList list;
    for (const Device& device : devices) list.append(device.id);
    return list;
}

QStringList kinds(const std::vector<Device>& devices) {
    QStringList list;
    for (const Device& device : devices) list.append(device.kind);
    return list;
}

// A new audio track (Ctrl+T), selected, with these devices.
std::pair<QString, QStringList> shownTrack(SessionFixture& f, const QStringList& deviceKinds) {
    const QString track = f.s().insertAudioTrack();
    f.selection().selectTrack(track);
    QStringList devices;
    for (const QString& kind : deviceKinds) devices.append(f.editor().addDevice(track, kind));
    return {track, devices};
}

std::pair<QString, QStringList> shownTrack(SessionFixture& f, int utilities) {
    return shownTrack(f, QStringList(utilities, QStringLiteral("utility")));
}

constexpr int kShift = Qt::ShiftModifier;
constexpr int kCtrl = Qt::ControlModifier;

// Each test's own preset library (the session lists it when it is made).
struct PresetLibrary {
    QString root() const { return dir.path(QStringLiteral("Presets")); }
    QString path(const QString& group, const QString& name) const {
        return root() + u'/' + group + u'/' + name + kPresetExtension;
    }
    TempDir dir;
    test::ScopedEnv env{"SUBSTATION_PRESETS", root()};
};

QStringList itemNames(const std::vector<BrowserItem>& items) {
    QStringList names;
    for (const BrowserItem& item : items) names.append(item.name);
    return names;
}

}  // namespace

class TestSessionDevices : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { test::prepareApplication(); }
    void init() { QSettings().clear(); }

    // --- Selecting ---

    void selectingDevices() {
        SessionFixture f;
        DeviceSelection& view = *f.s().deviceSelection();
        const auto [track, devices] = shownTrack(f, 3);
        const QString a = devices[0], b = devices[1], c = devices[2];
        QCOMPARE(view.trackId(), track);
        QCOMPARE(view.shownDevices(), devices);
        view.selectDevice(a);  // a click
        QCOMPARE(view.selected(), QStringList{a});
        QCOMPARE(f.selection().focus(), Selection::Focus::Devices);
        view.selectDevice(c, kShift);  // the range from the last one clicked
        QCOMPARE(view.selected(), (QStringList{a, b, c}));
        view.selectDevice(b, kCtrl);  // out
        QCOMPARE(view.selected(), (QStringList{a, c}));
        view.selectDevice(b, kCtrl);  // in, in chain order
        QCOMPARE(view.selected(), (QStringList{a, b, c}));
        view.selectDevice(b, kCtrl);
        // A plain press on one selected keeps the others (for a drag); the release selects just it.
        view.press(a);
        QCOMPARE(view.selected(), (QStringList{a, c}));
        QCOMPARE(view.dragDevices(a), (QStringList{a, c}));
        view.release(a);
        QCOMPARE(view.selected(), QStringList{a});
        QCOMPARE(view.dragDevices(b), QStringList{b});
        view.press(c, kShift);
        QCOMPARE(view.selected(), (QStringList{a, b, c}));
        view.menuRequested(b);  // (one of them: they stay selected)
        QCOMPARE(view.selected(), (QStringList{a, b, c}));
        // A click beside the devices: none selected, and the device view has the focus.
        f.selection().focusTracks();
        view.clickBeside();
        QVERIFY(view.selected().isEmpty());
        QCOMPARE(f.selection().focus(), Selection::Focus::Devices);
        view.menuRequested(b);
        QCOMPARE(view.selected(), QStringList{b});

        // The user went on to select something else: none selected.
        f.selection().selectTrack(track, true);
        QVERIFY(view.selected().isEmpty());
        view.selectDevice(a);
        f.selection().setTimeRange(0.0, 4.0, {track}, QSet<ClipRef>());
        QVERIFY(view.selected().isEmpty());
        // Another track shown: its devices.
        view.selectDevice(a);
        const QString other = f.editor().addMidiTrack();
        f.selection().selectTrack(other);
        QCOMPARE(view.trackId(), other);
        QVERIFY(view.selected().isEmpty() && view.anchor().isEmpty());
        // An instrument doesn't move (it stays first).
        const QString synth = f.project().track(other).devices[0].id;
        const QString effect = f.editor().addDevice(other, QStringLiteral("utility"));
        view.selectDevice(synth);
        view.selectDevice(effect, kShift);
        QCOMPARE(view.dragDevices(effect), QStringList{effect});
        // A device deleted is no longer selected.
        f.editor().removeDevice(other, effect);
        QCOMPARE(view.selected(), QStringList{synth});
    }

    void whatTheDeviceViewSays() {
        SessionFixture f;
        DeviceSelection& view = *f.s().deviceSelection();
        QCOMPARE(view.hint(), QStringLiteral("No track selected"));
        const QString audio = f.s().insertAudioTrack();
        QCOMPARE(view.hint(), kEffectsHint);
        f.editor().addDevice(audio, QStringLiteral("utility"));
        QCOMPARE(view.hint(), QString());
        const QString keys = f.editor().addMidiTrack(-1, QString(), QString());  // no instrument
        f.selection().selectTrack(keys);
        QCOMPARE(view.hint(), kInstrumentHint);
        f.selection().selectTrack(kMaster);
        QCOMPARE(view.trackId(), kMaster);  // the master's effects
        QCOMPARE(view.hint(), kEffectsHint);
    }

    void theTrackShownPlaysLive() {
        // Background freezing keeps the track the device view shows live (its
        // devices' meters and displays move): the engine is told which it is.
        SessionFixture f;
        const QString audio = f.s().insertAudioTrack();
        const QString other = f.s().insertAudioTrack();
        f.selection().selectTrack(audio);
        const quint32 a = *f.bridge().engineTrackId(audio);
        const quint32 b = *f.bridge().engineTrackId(other);
        QVERIFY(f.engine.trackObserved(a));
        QVERIFY(!f.engine.trackObserved(b));
        f.selection().selectTrack(other);
        QVERIFY(!f.engine.trackObserved(a));
        QVERIFY(f.engine.trackObserved(b));
        f.selection().selectTrack(kMaster);
        QVERIFY(f.engine.trackObserved(sub::Engine::kMaster));
        QVERIFY(!f.engine.trackObserved(b));
    }

    // --- Its clipboard ---

    void cutCopyPasteAndDuplicateDevices() {
        SessionFixture f;
        Session& s = f.s();
        DeviceSelection& view = *s.deviceSelection();
        const auto [track, devices] = shownTrack(f, 3);
        const QString a = devices[0], b = devices[1], c = devices[2];
        f.editor().setDeviceParam(track, a, QStringLiteral("gain"), 0.3);
        view.selectDevice(a);
        QCOMPARE(f.selection().focus(), Selection::Focus::Devices);
        s.copy();  // Ctrl+C
        QVERIFY(view.hasClipboard());
        view.selectDevice(b);
        s.paste();  // Ctrl+V: after the selected device, and selected
        QStringList now = ids(f.project().track(track).devices);
        QCOMPARE(now.size(), 4);
        QCOMPARE(now.mid(0, 2), (QStringList{a, b}));
        QCOMPARE(now[3], c);
        const Device pasted = f.project().track(track).devices[2];
        QVERIFY(pasted.id != a && pasted.id != b && pasted.id != c);
        QCOMPARE(pasted.params.value(QStringLiteral("gain")), 0.3);
        QCOMPARE(view.selected(), QStringList{pasted.id});
        s.cut();  // Ctrl+X: the pasted one goes (to the clipboard)
        QCOMPARE(ids(f.project().track(track).devices), (QStringList{a, b, c}));
        f.stack().undo();
        QCOMPARE(f.project().track(track).devices.size(), size_t{4});
        f.stack().undo();
        QCOMPARE(ids(f.project().track(track).devices), (QStringList{a, b, c}));
        view.selectDevice(b);
        s.duplicate();  // Ctrl+D: a copy right after it; the clipboard keeps what was cut
        now = ids(f.project().track(track).devices);
        QCOMPARE(now.size(), 4);
        QCOMPARE(now[1], b);
        QCOMPARE(view.selected(), QStringList{now[2]});
        QCOMPARE(view.clipboard().size(), size_t{1});
        QCOMPARE(view.clipboard()[0].params.value(QStringLiteral("gain")), 0.3);

        // Onto another track: click beside its devices (the device view takes the focus), then paste.
        const auto [other, none] = shownTrack(f, 0);
        f.selection().selectTrack(other, true);
        QCOMPARE(f.selection().focus(), Selection::Focus::Track);
        view.clickBeside();
        QCOMPARE(f.selection().focus(), Selection::Focus::Devices);
        s.paste();
        QCOMPARE(f.project().track(other).devices.size(), size_t{1});
        const Device copy = f.project().track(other).devices[0];
        QCOMPARE(copy.params.value(QStringLiteral("gain")), 0.3);
        QCOMPARE(view.selected(), QStringList{copy.id});
        QVERIFY(!s.arrangement()->hasClipboard());  // (the clips' clipboard is another)

        // Delete deletes the selected devices.
        s.deleteSelection();
        QVERIFY(f.project().track(other).devices.empty());
    }

    void pastingWhereDevicesCantGo() {
        SessionFixture f;
        Session& s = f.s();
        DeviceSelection& view = *s.deviceSelection();
        QVERIFY(!view.paste());
        QCOMPARE(f.lastMessage(), QStringLiteral("Nothing to paste: copy (Ctrl+C) or cut (Ctrl+X) devices first."));
        const QString keys = s.insertMidiTrack();
        view.selectDevice(f.project().track(keys).devices[0].id);
        QVERIFY(view.copySelected());
        const auto [audio, devices] = shownTrack(f, 1);
        QVERIFY(!view.pasteAt({}, -1));  // an instrument on an audio track
        QCOMPARE(f.lastMessage(), kInstrumentRefused);
        QCOMPARE(ids(f.project().track(audio).devices), devices);
        // After a device (its menu's Paste), on a MIDI track: the instrument takes the instrument's place.
        f.selection().selectTrack(keys);
        const QString effect = f.editor().addDevice(keys, QStringLiteral("utility"));
        QVERIFY(view.pasteAfter(effect));
        QCOMPARE(f.project().track(keys).devices.size(), size_t{2});
    }

    // --- Folding ---

    void foldingDevices() {
        SessionFixture f;
        DeviceSelection& view = *f.s().deviceSelection();
        const auto [track, devices] = shownTrack(f, 3);
        const QString a = devices[0], b = devices[1], c = devices[2];
        const int steps = f.stack().count();
        view.toggleFold(a);
        QVERIFY(f.project().isDeviceFolded(a));
        view.toggleFold(a);
        QVERIFY(!f.project().isDeviceFolded(a));
        QCOMPARE(f.stack().count(), steps);  // view state: not undone
        view.selectDevice(c);
        // With several selected, folding one of them folds them all.
        view.selectDevice(a);
        view.selectDevice(b, kCtrl);
        view.toggleFold(b);
        QVERIFY(f.project().isDeviceFolded(a) && f.project().isDeviceFolded(b) && !f.project().isDeviceFolded(c));
        QCOMPARE(view.selected(), (QStringList{a, b}));  // still selected
        // A folded rack hides its chain (and what is in it).
        const QString rack = f.editor().groupDevices(track, {c});
        QVERIFY(view.shownDevices().contains(c));
        QCOMPARE(view.shownChain(rack), f.project().device(track, rack).chains[0].id);
        view.toggleFold(rack);
        QVERIFY(f.project().isDeviceFolded(rack));
        QVERIFY(!view.shownDevices().contains(c) && view.shownDevices().contains(rack));
        view.toggleFold(rack);
        QVERIFY(view.shownDevices().contains(c));
    }

    // --- Racks ---

    void groupingAndUngroupingInTheDeviceView() {
        SessionFixture f;
        Session& s = f.s();
        DeviceSelection& view = *s.deviceSelection();
        const auto [track, devices] = shownTrack(f, 2);
        const QString a = devices[0], b = devices[1];
        view.selectDevice(a);
        view.selectDevice(b, kShift);
        QCOMPARE(f.selection().focus(), Selection::Focus::Devices);
        s.groupSelected();  // Ctrl+G, in the device view
        const auto& chain = f.project().track(track).devices;
        QCOMPARE(chain.size(), size_t{1});
        QVERIFY(chain[0].isRack());
        const QString rack = chain[0].id;
        QCOMPARE(view.selected(), QStringList{rack});
        QCOMPARE(deviceName(chain[0]), QStringLiteral("Audio Effect Rack"));
        // Its chain shows beside it, with its devices, which select as any.
        QCOMPARE(view.shownDevices(), (QStringList{rack, a, b}));
        QCOMPARE(view.containerOf(a), f.project().device(track, rack).chains[0].id);
        view.selectDevice(b);
        QCOMPARE(view.selected(), QStringList{b});
        view.selectDevice(rack, kCtrl);  // another chain's: a selection of its own
        QCOMPARE(view.selected(), QStringList{rack});
        s.ungroupSelected();  // Ctrl+Shift+G
        QCOMPARE(ids(f.project().track(track).devices), (QStringList{a, b}));
        QCOMPARE(f.stack().undoText(), QStringLiteral("Ungroup Rack"));
        // Nothing selected: said so.
        s.groupSelected();
        QCOMPARE(f.lastMessage(), QStringLiteral("Select the devices to group."));
        s.ungroupSelected();
        QCOMPARE(f.lastMessage(), QStringLiteral("Select a rack to ungroup."));
        // A device's menu's Group: it alone, unless it is one of those selected.
        view.groupDevice(a);
        QVERIFY(f.project().track(track).devices[0].isRack());
        QCOMPARE(f.project().track(track).devices[1].id, b);
    }

    void chainsInTheDeviceView() {
        SessionFixture f;
        DeviceSelection& view = *f.s().deviceSelection();
        const auto [track, devices] = shownTrack(f, 2);
        const QString a = devices[0], b = devices[1];
        const QString rack = f.editor().groupDevices(track, {a});
        const QString second = f.editor().tryAddRackChain(track, rack);
        const QString first = f.project().device(track, rack).chains[0].id;
        QCOMPARE(view.shownChain(rack), first);  // the first one shows
        view.clickChain(rack, second);  // shows the other one
        QCOMPARE(view.shownChain(rack), second);
        QCOMPARE(view.clickedChain(), second);
        QCOMPARE(view.clickedRack(), rack);
        QCOMPARE(f.selection().focus(), Selection::Focus::Devices);
        QVERIFY(!view.shownDevices().contains(a));
        // Dropped into the chain shown: into it.
        view.dropDevices({QStringLiteral("utility")}, {}, second, 0);
        const auto& added = f.project().chain(track, second).devices;
        QCOMPARE(added.size(), size_t{1});
        QVERIFY(view.shownDevices().contains(added[0].id));
        // Dragged from the track's chain onto a chain's row: into it, last.
        QVERIFY(view.dropMoved({b}, first, static_cast<int>(view.chainDevices(first).size())));
        QCOMPARE(ids(f.project().chain(track, first).devices), (QStringList{a, b}));
        QCOMPARE(ids(f.project().track(track).devices), QStringList{rack});
        // A rack can't go into itself.
        QVERIFY(!view.dropMoved({rack}, first, 0));
        QCOMPARE(f.lastMessage(), QStringLiteral("A rack can't go into itself, and racks nest at most 8 deep."));
    }

    void ctrlRRenamesTheTrackOrTheRackChainClicked() {
        SessionFixture f;
        Session& s = f.s();
        DeviceSelection& view = *s.deviceSelection();
        const auto [track, devices] = shownTrack(f, 2);
        f.selection().selectTrack(track, true);
        QVariantMap target = s.renameTarget();  // Ctrl+R: the track
        QCOMPARE(target.value(QStringLiteral("kind")).toString(), QStringLiteral("track"));
        QCOMPARE(target.value(QStringLiteral("trackId")).toString(), track);
        f.editor().renameTrack(track, QStringLiteral("Drums"));

        const QString rack = f.editor().groupDevices(track, {devices[0]});
        const QString second = f.editor().tryAddRackChain(track, rack);
        view.clickChain(rack, second);
        QCOMPARE(f.selection().focus(), Selection::Focus::Devices);
        target = s.renameTarget();  // Ctrl+R: the chain clicked
        QCOMPARE(target.value(QStringLiteral("kind")).toString(), QStringLiteral("chain"));
        QCOMPARE(target.value(QStringLiteral("chainId")).toString(), second);
        QCOMPARE(target.value(QStringLiteral("rackId")).toString(), rack);
        f.editor().renameChain(track, second, QStringLiteral("Wet"));
        QCOMPARE(f.project().chain(track, second).name, QStringLiteral("Wet"));
        QCOMPARE(f.project().track(track).name, QStringLiteral("Drums"));
        // A device clicked since: Ctrl+R renames the track again.
        view.selectDevice(rack);
        QCOMPARE(s.renameTarget().value(QStringLiteral("kind")).toString(), QStringLiteral("track"));
    }

    void droppedDevicesGoWhereTheyWereDropped() {
        SessionFixture f;
        DeviceSelection& view = *f.s().deviceSelection();
        const auto [track, devices] = shownTrack(f, 1);
        view.dropDevices({QStringLiteral("compressor"), QStringLiteral("utility"), QStringLiteral("no such device")}, {},
                         {}, 0);
        QCOMPARE(kinds(f.project().track(track).devices),
                 (QStringList{QStringLiteral("compressor"), QStringLiteral("utility"), QStringLiteral("utility")}));
        QCOMPARE(f.project().track(track).devices[2].id, devices[0]);
        view.dropDevices({QStringLiteral("synth")}, {}, {}, 1);  // instruments go on MIDI tracks
        QCOMPARE(f.lastMessage(), kInstrumentRefused);
        QCOMPARE(f.project().track(track).devices.size(), size_t{3});
        // On a MIDI track a new instrument goes first, and the next device after the drop point.
        const QString keys = f.editor().addMidiTrack(-1, QString(), QString());
        f.selection().selectTrack(keys);
        f.editor().addDevice(keys, QStringLiteral("utility"));
        view.dropDevices({QStringLiteral("synth"), QStringLiteral("compressor")}, {}, {}, 1);
        QCOMPARE(kinds(f.project().track(keys).devices),
                 (QStringList{QStringLiteral("synth"), QStringLiteral("utility"), QStringLiteral("compressor")}));
    }

    // --- Presets ---

    void theSaveButtonSavesToTheLibraryAndTheBrowserListsIt() {
        PresetLibrary library;
        SessionFixture f;
        Session& s = f.s();
        DeviceSelection& view = *s.deviceSelection();
        const auto [track, devices] = shownTrack(f, 1);
        const QString utility = devices[0];
        f.editor().setDeviceParam(track, utility, QStringLiteral("gain"), -7.0);
        QCOMPARE(view.presetName(utility), QStringLiteral("Utility"));
        QVERIFY(view.checkPresetName(utility, QStringLiteral("Quieter")).value(QStringLiteral("exists")).isNull());
        QSignalSpy saved(&view, &DeviceSelection::presetSaved);
        const QString path = view.savePreset(utility, QStringLiteral("Quieter"));
        QCOMPARE(QFileInfo(path).absoluteFilePath(), QFileInfo(library.path(QStringLiteral("Utility"), QStringLiteral("Quieter"))).absoluteFilePath());
        QCOMPARE(saved.count(), 1);
        QCOMPARE(loadPreset(path).params.value(QStringLiteral("gain")), -7.0);
        QVERIFY(f.lastMessage().startsWith(QStringLiteral("Saved the preset Quieter")));
        // The browser lists it (Presets › Utility).
        QCOMPARE(itemNames(s.presets()->items()), QStringList{QStringLiteral("Quieter")});
        QCOMPARE(s.presets()->groups(), QStringList{QStringLiteral("Utility")});
        QCOMPARE(QFileInfo(s.browser()->presetFolder({QStringLiteral("presets"), QStringLiteral("Utility")})).absoluteFilePath(),
                 QFileInfo(path).absolutePath());

        // Saving again under that name asks first.
        f.editor().setDeviceParam(track, utility, QStringLiteral("gain"), -1.0);
        const QVariantMap check = view.checkPresetName(utility, QStringLiteral("Quieter"));
        QVERIFY(check.value(QStringLiteral("exists")).toBool());
        QCOMPARE(check.value(QStringLiteral("question")).toString(),
                 QStringLiteral("There is a Utility preset called “Quieter” already. Replace it?"));
        QCOMPARE(loadPreset(path).params.value(QStringLiteral("gain")), -7.0);  // ("No": as it was)
        view.savePreset(utility, QStringLiteral("Quieter"));  // ("Yes")
        QCOMPARE(loadPreset(path).params.value(QStringLiteral("gain")), -1.0);
        // A name that can't be a file's.
        QVERIFY(!view.checkPresetName(utility, QStringLiteral("  ")).value(QStringLiteral("error")).toString().isEmpty());
        QVERIFY(view.savePreset(utility, QStringLiteral("CON")).isEmpty());
    }

    void everyKindOfDeviceCanBeSaved() {
        PresetLibrary library;
        SessionFixture f;
        DeviceSelection& view = *f.s().deviceSelection();
        const auto [track, devices] = shownTrack(f, {QStringLiteral("utility"), QStringLiteral("compressor")});
        view.selectDevice(devices[1]);
        QVERIFY(view.groupSelected());
        const QString rack = f.project().track(track).devices[1].id;
        QVERIFY(!view.savePreset(rack, QStringLiteral("Mine")).isEmpty());
        QVERIFY(!view.savePreset(devices[0], QStringLiteral("Mine")).isEmpty());
        QVERIFY(QFileInfo::exists(library.path(QStringLiteral("Audio Effect Rack"), QStringLiteral("Mine"))));
        QVERIFY(QFileInfo::exists(library.path(QStringLiteral("Utility"), QStringLiteral("Mine"))));
        QCOMPARE(f.s().presets()->groups(), (QStringList{QStringLiteral("Audio Effect Rack"), QStringLiteral("Utility")}));
    }

    void droppingAPresetOnTheDeviceView() {
        PresetLibrary library;
        SessionFixture f;
        DeviceSelection& view = *f.s().deviceSelection();
        const auto [track, devices] = shownTrack(f, {QStringLiteral("utility"), QStringLiteral("compressor")});
        const QString source = devices[0], compressor = devices[1];
        f.editor().setDeviceParam(track, source, QStringLiteral("gain"), -11.0);
        const QString path = view.savePreset(source, QStringLiteral("Down"));
        f.editor().removeDevice(track, source);

        // Between devices (here: before the compressor): a new device.
        view.dropPresets({path}, {}, 0);
        QCOMPARE(kinds(f.project().track(track).devices), (QStringList{QStringLiteral("utility"), QStringLiteral("compressor")}));
        QCOMPARE(f.project().track(track).devices[0].params.value(QStringLiteral("gain")), -11.0);
        QCOMPARE(f.stack().undoText(), QStringLiteral("Load Preset Down"));

        // Onto a device of another kind: a new device too, where it was dropped.
        QVERIFY(!view.presetLoadsInto(path, compressor));
        view.dropPresets({path}, {}, 1, compressor);
        QCOMPARE(kinds(f.project().track(track).devices),
                 (QStringList{QStringLiteral("utility"), QStringLiteral("utility"), QStringLiteral("compressor")}));
        f.stack().undo();

        // Onto a device of its kind: loads into it, one undo step.
        const QString target = f.editor().addDevice(track, QStringLiteral("utility"));
        const int count = f.stack().count();
        QVERIFY(view.presetLoadsInto(path, target));
        view.dropPresets({path}, {}, 0, target);
        QCOMPARE(ids(f.project().track(track).devices).back(), target);
        QCOMPARE(f.project().device(track, target).params.value(QStringLiteral("gain")), -11.0);
        QCOMPARE(f.stack().count(), count + 1);
        f.stack().undo();
        QCOMPARE(f.project().device(track, target).params.value(QStringLiteral("gain")), 0.0);

        // A preset that isn't one says so.
        view.dropPresets({library.dir.path(QStringLiteral("nothing.gilpreset"))}, {}, 0);
        QVERIFY(!f.lastMessage().isEmpty());
        QVERIFY(!view.loadPresetInto(compressor, path));
        QCOMPARE(f.lastMessage(), QStringLiteral("A Utility preset can't load into Compressor."));
    }

    void doubleClickingPresets() {
        PresetLibrary library;
        SessionFixture f;
        Session& s = f.s();
        DeviceSelection& view = *s.deviceSelection();
        const auto [track, devices] = shownTrack(f, 1);
        const QString plain = view.savePreset(devices[0], QStringLiteral("Plain"));
        Q_EMIT s.browser()->presetActivated(plain);
        QCOMPARE(kinds(f.project().track(track).devices), (QStringList{QStringLiteral("utility"), QStringLiteral("utility")}));

        // An instrument preset with an audio track selected: on a new MIDI track, one undo step.
        const QString keys = f.editor().addMidiTrack();
        f.selection().selectTrack(keys);
        const QString pad = view.savePreset(f.project().track(keys).devices[0].id, QStringLiteral("Pad"));
        f.selection().selectTrack(track);
        const size_t tracks = f.project().tracks().size();
        const int count = f.stack().count();
        Q_EMIT s.browser()->presetActivated(pad);
        QCOMPARE(f.project().tracks().size(), tracks + 1);
        QCOMPARE(f.stack().count(), count + 1);
        const Track& added = f.project().track(f.selection().trackId());
        QVERIFY(added.isMidi());
        QCOMPARE(kinds(added.devices), QStringList{f.project().track(keys).devices[0].kind});
        // With nothing selected, an effect preset goes nowhere.
        f.selection().selectTrack(QString());
        s.addPresetToSelectedTrack(plain);
        QCOMPARE(f.lastMessage(), QStringLiteral("Select a track to add the preset to."));
    }

    void droppingAPresetOnATrackInTheArrangement() {
        PresetLibrary library;
        SessionFixture f;
        Session& s = f.s();
        const auto [track, devices] = shownTrack(f, 1);
        const QString path = s.deviceSelection()->savePreset(devices[0], QStringLiteral("Plain"));
        const QString other = s.insertAudioTrack();
        f.selection().selectTrack(track);
        QVERIFY(s.arrangement()->dropPresets({path}, other));
        QCOMPARE(kinds(f.project().track(other).devices), QStringList{QStringLiteral("utility")});
        QCOMPARE(f.selection().trackId(), other);
        // Below the tracks: an effect preset makes nothing.
        QVERIFY(!s.arrangement()->dropPresets({path}, {}));
    }

    void renamingPresetsInTheBrowser() {
        PresetLibrary library;
        SessionFixture f;
        Session& s = f.s();
        BrowserController& browser = *s.browser();
        const auto [track, devices] = shownTrack(f, 1);
        const QString path = s.deviceSelection()->savePreset(devices[0], QStringLiteral("Old"));
        QCOMPARE(itemNames(s.presets()->items()), QStringList{QStringLiteral("Old")});
        const QString renamed = browser.renamePreset(path, QStringLiteral("New"));
        QCOMPARE(QFileInfo(renamed).fileName(), QStringLiteral("New") + kPresetExtension);
        QVERIFY(!QFileInfo::exists(path));
        QCOMPARE(itemNames(s.presets()->items()), QStringList{QStringLiteral("New")});
        QVERIFY(browser.renamePreset(renamed, QStringLiteral(" New ")).isEmpty());  // (the same name: nothing)
        QVERIFY(browser.renamePreset(renamed, QStringLiteral("")).isEmpty());

        // Ctrl+R with the list focused renames the preset current there.
        browser.setScope({QStringLiteral("presets"), QStringLiteral("Utility")});
        QTRY_VERIFY(!browser.searching() && browser.results()->rowCount() == 1);
        browser.setCurrentRow(0);
        const QVariantMap target = s.renameTarget(true);
        QCOMPARE(target.value(QStringLiteral("kind")).toString(), QStringLiteral("preset"));
        QCOMPARE(target.value(QStringLiteral("name")).toString(), QStringLiteral("New"));
        QCOMPARE(QFileInfo(target.value(QStringLiteral("path")).toString()).fileName(), QFileInfo(renamed).fileName());
        const QString newer = browser.renamePreset(target.value(QStringLiteral("path")).toString(), QStringLiteral("Newer"));
        QVERIFY(QFileInfo::exists(newer));
        QCOMPARE(itemNames(s.presets()->items()), QStringList{QStringLiteral("Newer")});
        // Without the list focused: the selected track.
        f.selection().selectTrack(track, true);
        QCOMPARE(s.renameTarget(false).value(QStringLiteral("kind")).toString(), QStringLiteral("track"));
    }

    void saveAsDefaultPreset() {
        PresetLibrary library;
        SessionFixture f;
        Session& s = f.s();
        DeviceSelection& view = *s.deviceSelection();
        const auto [track, devices] = shownTrack(f, 1);
        f.editor().setDeviceParam(track, devices[0], QStringLiteral("gain"), -5.0);
        QVERIFY(!view.hasDefault(devices[0]));
        QVERIFY(!view.saveAsDefault(devices[0]).isEmpty());
        QVERIFY(f.lastMessage().contains(QStringLiteral("New Utility devices will start like this one")));
        QVERIFY(view.hasDefault(devices[0]));
        s.addDeviceToSelectedTrack(QStringLiteral("utility"));
        QCOMPARE(f.project().track(track).devices.back().params.value(QStringLiteral("gain")), -5.0);
        QVERIFY(s.presets()->groups().isEmpty());  // (defaults aren't listed)
        view.clearDefault(f.project().track(track).devices.back().id);
        QCOMPARE(f.lastMessage(), QStringLiteral("New Utility devices will start as they come."));
        s.addDeviceToSelectedTrack(QStringLiteral("utility"));
        QCOMPARE(f.project().track(track).devices.back().params.value(QStringLiteral("gain")), 0.0);
    }

    void aRackIsNamedAsItsPreset() {
        PresetLibrary library;
        SessionFixture f;
        DeviceSelection& view = *f.s().deviceSelection();
        const auto [track, devices] = shownTrack(f, 1);
        view.selectDevice(devices[0]);
        QVERIFY(view.groupSelected());
        const QString rack = f.project().track(track).devices[0].id;
        QCOMPARE(view.presetName(rack), QStringLiteral("Audio Effect Rack"));
        const QString path = view.savePreset(rack, QStringLiteral("Glue"));
        QCOMPARE(deviceName(f.project().device(track, rack)), QStringLiteral("Glue"));
        QCOMPARE(f.stack().undoText(), QStringLiteral("Save Preset Glue"));
        // Loaded elsewhere, it is named as the preset too.
        QVERIFY(view.insertPreset(path));
        QCOMPARE(deviceName(f.project().track(track).devices.back()), QStringLiteral("Glue"));
        // Load Preset… remembers where the file was.
        QVERIFY(view.loadPresetFile(path));
        QCOMPARE(QDir(view.presetFolder()).canonicalPath(), QDir(QFileInfo(path).absolutePath()).canonicalPath());
    }

    void thePresetIndexSeesTheLibraryMade() {
        TempDir dir;
        const QString root = dir.path(QStringLiteral("SUBstation/Presets"));
        PresetIndex index(nullptr, root);
        QVERIFY(index.items().empty() && !index.watched().isEmpty());  // the nearest folder above it
        QVERIFY(QDir().mkpath(root + QStringLiteral("/Utility")));
        Device device;
        device.id = QStringLiteral("u");
        device.kind = QStringLiteral("utility");
        savePreset(device, root + QStringLiteral("/Utility/Warm") + kPresetExtension);
        QTRY_COMPARE_WITH_TIMEOUT(itemNames(index.items()), QStringList{QStringLiteral("Warm")}, 5000);
        QCOMPARE(index.items()[0].detail, QStringLiteral("Utility"));
        QVERIFY(index.items()[0].toolTip.startsWith(QStringLiteral("Warm\nUtility preset\n")));
        // A preset straight in the library is listed as "Other".
        savePreset(device, root + QStringLiteral("/Loose") + kPresetExtension);
        QTRY_COMPARE_WITH_TIMEOUT(index.groups(), (QStringList{QStringLiteral("Other"), QStringLiteral("Utility")}), 5000);
    }
};

QTEST_GUILESS_MAIN(TestSessionDevices)
#include "test_session_devices.moc"
