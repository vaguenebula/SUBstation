// Undo commands: each puts its snapshot in place and back, the project signals
// every change, and gestures (a merge key) merge into one undo step.

#include "TestSupport.h"

#include "model/Commands.h"
#include "model/Devices.h"
#include "model/Edits.h"
#include "model/Project.h"

#include <QSignalSpy>
#include <QTest>
#include <QUndoStack>

using namespace sub::app;
namespace autom = sub::app::automation;

namespace {

const QString kGesture = QStringLiteral("{gesture-1}");
const QString kOtherGesture = QStringLiteral("{gesture-2}");

// Two audio tracks ("a" with a utility "u" and a rack "rack" holding "inner" in
// chain "c1"), a MIDI track, a return.
void fill(Project& project) {
    ProjectContents contents;
    Track a = test::makeTrack("a", "A");
    Device rack = newRack({newChain("One", {test::makeDevice("inner", "utility", {{"gain", 0.0}})})});
    rack.id = "rack";
    rack.chains[0].id = "c1";
    a.devices = {test::makeDevice("u", "utility", {{"gain", 0.0}, {"pan", 0.0}}), rack};
    contents.tracks = {a, test::makeTrack("b", "B"), test::makeTrack("m", "M", kMidiKind)};
    contents.returns = {test::makeTrack("r", "A Return", kReturnKind)};
    project.replaceContents(contents);
}

}  // namespace

class TestCommands : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { test::prepareApplication(); }

    void insertingAndRemovingTracks() {
        Project project;
        QUndoStack stack;
        QSignalSpy inserted(&project, &Project::trackInserted);
        QSignalSpy removed(&project, &Project::trackRemoved);
        Track track = test::makeTrack("t", "T");
        stack.push(new InsertTrackCommand(&project, track, 5));
        QCOMPARE(project.tracks().size(), size_t(1));
        QCOMPARE(inserted.takeFirst(), (QVariantList{"t", 0}));  // (an index past the end: last)
        QCOMPARE(stack.undoText(), QStringLiteral("Insert Track"));
        // The stack's track is its own: changing the project's doesn't change it.
        project.updateTrack("t", TrackField::Name, QStringLiteral("Renamed"));
        stack.undo();
        QVERIFY(project.tracks().empty());
        QCOMPARE(removed.takeFirst(), (QVariantList{"t", 0}));
        stack.redo();
        QCOMPARE(project.track("t").name, QStringLiteral("T"));

        stack.push(new InsertTrackCommand(&project, test::makeTrack("u", "U"), 0, "Paste Track"));
        stack.push(new RemoveTrackCommand(&project, "t"));
        QCOMPARE(stack.undoText(), QStringLiteral("Delete Track"));
        QCOMPARE(removed.takeLast(), (QVariantList{"t", 1}));
        stack.undo();  // back where it was
        QCOMPARE(project.trackIndex("t"), 1);
        QCOMPARE(inserted.takeLast(), (QVariantList{"t", 1}));
    }

    void insertingAndRemovingReturns() {
        Project project;
        fill(project);
        QUndoStack stack;
        QSignalSpy inserted(&project, &Project::returnInserted);
        QSignalSpy removed(&project, &Project::returnRemoved);
        stack.push(new InsertReturnCommand(&project, test::makeTrack("r0", "Zero", kReturnKind), 0));
        QCOMPARE(project.returns()[0].id, QStringLiteral("r0"));
        QCOMPARE(inserted.takeFirst(), (QVariantList{"r0", 0}));
        QCOMPARE(stack.undoText(), QStringLiteral("Insert Return Track"));
        stack.push(new RemoveReturnCommand(&project, "r"));
        QCOMPARE(removed.takeFirst(), (QVariantList{"r", 1}));
        QCOMPARE(stack.undoText(), QStringLiteral("Delete Return Track"));
        stack.undo();
        QCOMPARE(project.returnIndex("r"), 1);
        stack.undo();
        QCOMPARE(project.returns().size(), size_t(1));
    }

    void replacingATrack() {
        Project project;
        fill(project);
        QUndoStack stack;
        QSignalSpy removed(&project, &Project::trackRemoved);
        QSignalSpy inserted(&project, &Project::trackInserted);
        Track before = project.track("m");
        Track after = before;
        after.kind = kAudioKind;
        after.devices.clear();
        stack.push(new ReplaceTrackCommand(&project, before, after, "Flatten Track"));
        QVERIFY(project.track("m").isAudio() && project.trackIndex("m") == 2);
        QCOMPARE(removed.takeFirst(), (QVariantList{"m", 2}));
        QCOMPARE(inserted.takeFirst(), (QVariantList{"m", 2}));
        stack.undo();
        QVERIFY(project.track("m").isMidi());
    }

    void freezing() {
        Project project;
        fill(project);
        QUndoStack stack;
        QSignalSpy changed(&project, &Project::freezeChanged);
        const Freeze freeze{"/f/a.wav", 2.0, 120.0};
        stack.push(new SetFreezeCommand(&project, "a", std::nullopt, freeze, "Freeze Track"));
        QVERIFY(project.track("a").frozen == freeze);
        stack.undo();
        QVERIFY(!project.track("a").frozen);
        QCOMPARE(changed.count(), 2);
    }

    void clipsAndTheirMerging() {
        Project project;
        fill(project);
        QUndoStack stack;
        QSignalSpy changed(&project, &Project::clipsChanged);
        const Clip first = Clip::audio("c1", "a.wav", "a", 4.0, 1.0);
        const Clip second = Clip::audio("c2", "b.wav", "b", 0.0, 1.0);
        stack.push(new SetClipsCommand(&project, "Add Clips", {{"a", {}}}, {{"a", {first, second}}}));
        QCOMPARE(project.track("a").clips[0].id, QStringLiteral("c2"));  // sorted by start
        QCOMPARE(changed.takeFirst(), QVariantList{"a"});
        // A gesture's edits merge (the same tracks), into one step back to before it.
        auto moved = [&](double start) {
            Clip c = first;
            c.startBeat = start;
            return ClipLists{{"a", {second, c}}};
        };
        stack.push(new SetClipsCommand(&project, "Move", {{"a", project.track("a").clips}}, moved(5.0), kGesture));
        stack.push(new SetClipsCommand(&project, "Move", {{"a", project.track("a").clips}}, moved(6.0), kGesture));
        QCOMPARE(stack.count(), 2);
        QCOMPARE(project.clip("a", "c1").startBeat, 6.0);
        const auto* merged = dynamic_cast<const SetClipsCommand*>(stack.command(1));
        QVERIFY(merged != nullptr && merged->mergeKey() == kGesture);
        QCOMPARE(merged->before().value("a")[1].startBeat, 4.0);  // the drag's baseline
        // Other tracks, or another gesture, don't merge.
        stack.push(new SetClipsCommand(&project, "Move", {{"a", project.track("a").clips}, {"b", {}}},
                                       {{"a", project.track("a").clips}, {"b", {}}}, kGesture));
        QCOMPARE(stack.count(), 3);
        stack.push(new SetClipsCommand(&project, "Move", {{"a", project.track("a").clips}}, moved(7.0), kOtherGesture));
        QCOMPARE(stack.count(), 4);
        stack.undo();
        stack.undo();
        stack.undo();
        QCOMPARE(project.clip("a", "c1").startBeat, 4.0);
    }

    void trackSettingsMergeDuringAGesture() {
        Project project;
        fill(project);
        QUndoStack stack;
        QSignalSpy changed(&project, &Project::trackChanged);
        for (double db : {-1.0, -2.0, -3.0}) {
            stack.push(new UpdateTrackCommand(&project, "a", TrackField::VolumeDb, project.track("a").volumeDb, db,
                                              "Change Volume", kGesture));
        }
        QCOMPARE(stack.count(), 1);
        QCOMPARE(project.track("a").volumeDb, -3.0);
        QCOMPARE(changed.count(), 3);
        stack.undo();
        QCOMPARE(project.track("a").volumeDb, 0.0);
        stack.redo();
        // The same gesture on another setting or track doesn't merge, nor does no gesture.
        stack.push(new UpdateTrackCommand(&project, "a", TrackField::Pan, 0.0, 0.5, "Change Pan", kGesture));
        stack.push(new UpdateTrackCommand(&project, "b", TrackField::Pan, 0.0, 0.5, "Change Pan", kGesture));
        stack.push(new UpdateTrackCommand(&project, "b", TrackField::Mute, false, true, "Toggle Track Activator"));
        stack.push(new UpdateTrackCommand(&project, "b", TrackField::Mute, true, false, "Toggle Track Activator"));
        QCOMPARE(stack.count(), 5);
        const auto* last = dynamic_cast<const UpdateTrackCommand*>(stack.command(4));
        QVERIFY(last->id() == -1 && last->trackId() == "b" && last->field() == TrackField::Mute);
        QCOMPARE(dynamic_cast<const UpdateTrackCommand*>(stack.command(0))->id(), kMergeId);

        // Sends, inputs and the MIDI input are settings too.
        stack.push(new UpdateTrackCommand(&project, "a", TrackField::Sends, SendMap{}, SendMap{{"r", Send{-6.0, true}}},
                                          "Change Send"));
        QVERIFY((project.track("a").sends == SendMap{{"r", Send{-6.0, true}}}));
        stack.push(new UpdateTrackFieldsCommand(
            &project, "a", {{TrackField::Input, std::vector<int>{}}, {TrackField::InputTrack, std::optional<QString>()}},
            {{TrackField::Input, std::vector<int>{}}, {TrackField::InputTrack, std::optional<QString>("b")}},
            "Change Track Input"));
        QCOMPARE(project.track("a").inputTrack, std::optional<QString>("b"));
        stack.push(new UpdateTrackCommand(&project, "m", TrackField::MidiInput, std::optional<MidiInput>(MidiInput{}),
                                          std::optional<MidiInput>(), "Change MIDI Input"));
        QVERIFY(!project.track("m").midiInput);
        stack.undo();
        stack.undo();
        stack.undo();
        QVERIFY(project.track("a").sends.isEmpty() && !project.track("a").inputTrack && project.track("m").midiInput);
        // The master's mixer.
        stack.push(new UpdateTrackCommand(&project, kMaster, TrackField::Pan, 0.0, -0.25, "Change Master Pan"));
        QCOMPARE(project.master().pan, -0.25);
        QCOMPARE(changed.last(), QVariantList{kMaster});
    }

    void severalTracksAtOnce() {
        Project project;
        fill(project);
        QUndoStack stack;
        stack.push(new UpdateTracksCommand(&project, TrackField::VolumeDb, {{"a", 0.0}, {"b", 0.0}},
                                           {{"a", -6.0}, {"b", -3.0}}, "Change Volume", kGesture));
        stack.push(new UpdateTracksCommand(&project, TrackField::VolumeDb, {{"a", -6.0}, {"b", -3.0}},
                                           {{"a", -7.0}, {"b", -4.0}}, "Change Volume", kGesture));
        QCOMPARE(stack.count(), 1);
        QVERIFY(project.track("a").volumeDb == -7.0 && project.track("b").volumeDb == -4.0);
        stack.undo();
        QVERIFY(project.track("a").volumeDb == 0.0 && project.track("b").volumeDb == 0.0);
    }

    void settingsAndTheTempoDragBaseline() {
        Project project;
        fill(project);
        QUndoStack stack;
        QSignalSpy settings(&project, &Project::settingsChanged);
        const Clip unwarped = Clip::audio("c1", "a.wav", "a", 0.0, 2.0);  // 4 beats at 120
        const Clip next = Clip::audio("c2", "b.wav", "b", 4.0, 1.0);
        project.setClips("a", {unwarped, next});
        // A tempo drag: each step fits from the clips as they were when it began.
        const ClipLists original{{"a", project.track("a").clips}};
        auto step = [&](double tempo) {
            const auto* last = stack.index() > 0 ? dynamic_cast<const SetTempoCommand*>(stack.command(stack.index() - 1))
                                                 : nullptr;
            const ClipLists current{{"a", project.track("a").clips}};
            const ClipLists baseline = last && last->mergeKey() == kGesture ? last->oldValue().clips : current;
            ClipLists fitted;
            for (auto it = baseline.begin(); it != baseline.end(); ++it) fitted.insert(it.key(), edits::fitToTempo(*it, tempo));
            stack.push(new SetTempoCommand(&project, {project.tempo(), current}, {tempo, fitted}, kGesture));
        };
        step(150.0);
        QVERIFY(std::abs(project.clip("a", "c1").durationSec - 1.6) < 1e-9);  // trimmed at the next clip
        step(100.0);
        QCOMPARE(project.clip("a", "c1").durationSec, 2.0);  // back as it was
        QCOMPARE(stack.count(), 1);
        QCOMPARE(stack.undoText(), QStringLiteral("Change Tempo"));
        stack.undo();
        QCOMPARE(project.tempo(), 120.0);
        QVERIFY(project.track("a").clips == original.value("a"));
        stack.redo();
        QCOMPARE(project.tempo(), 100.0);

        stack.push(new UpdateSettingsCommand(
            &project, {{SettingsField::LoopEnabled, false}, {SettingsField::LoopStart, 0.0}, {SettingsField::LoopEnd, 16.0}},
            {{SettingsField::LoopEnabled, true}, {SettingsField::LoopStart, 4.0}, {SettingsField::LoopEnd, 8.0}},
            "Change Loop", kGesture));
        stack.push(new UpdateSettingsCommand(
            &project, {{SettingsField::LoopEnabled, true}, {SettingsField::LoopStart, 4.0}, {SettingsField::LoopEnd, 8.0}},
            {{SettingsField::LoopEnabled, true}, {SettingsField::LoopStart, 5.0}, {SettingsField::LoopEnd, 9.0}},
            "Change Loop", kGesture));
        QCOMPARE(stack.count(), 2);  // (a loop drag after a tempo drag: another kind of change)
        QVERIFY(project.loopEnabled() && project.loopStart() == 5.0 && project.loopEnd() == 9.0);
        stack.push(new UpdateSettingsCommand(&project, {{SettingsField::TimeSignature, TimeSignature{}}},
                                             {{SettingsField::TimeSignature, TimeSignature{3, 4}}}, "Change Time Signature"));
        QCOMPARE(project.timeSignature().numerator, 3);
        QCOMPARE(std::get<TimeSignature>(project.setting(SettingsField::TimeSignature)).numerator, 3);
        stack.undo();
        stack.undo();
        QVERIFY(!project.loopEnabled() && project.loopEnd() == 16.0 && project.timeSignature().numerator == 4);
        QVERIFY(settings.count() >= 6);
    }

    void devicesAndChains() {
        Project project;
        fill(project);
        QUndoStack stack;
        QSignalSpy devices(&project, &Project::devicesChanged);
        const std::vector<Device> before = project.track("b").devices;
        std::vector<Device> after = before;
        after.push_back(test::makeDevice("x", "utility"));
        stack.push(new SetDevicesCommand(&project, "b", before, after, "Add Utility"));
        QVERIFY(project.hasDevice("b", "x"));
        QCOMPARE(devices.takeFirst(), QVariantList{"b"});
        stack.undo();
        QVERIFY(!project.hasDevice("b", "x"));

        // A device moving between tracks: both change before either is heard of, in order.
        DeviceLists from{{"a", project.track("a").devices}, {"b", project.track("b").devices}};
        DeviceLists to = from;
        Device* moving = findDevice(*to.find("a"), "u");
        to.find("b")->push_back(*moving);
        to.find("a")->erase(to.find("a")->begin());
        QStringList heard;
        connect(&project, &Project::devicesChanged, this, [&](const QString& trackId) {
            heard.append(trackId + (project.hasDevice("b", "u") && !project.hasDevice("a", "u") ? "+" : "-"));
        });
        stack.push(new SetChainsCommand(&project, from, to, "Move Device"));
        QCOMPARE(heard, (QStringList{"a+", "b+"}));
        QCOMPARE(project.deviceOwner("u"), std::optional<QString>("b"));
        stack.undo();
        QCOMPARE(project.deviceOwner("u"), std::optional<QString>("a"));
    }

    void deviceSettings() {
        Project project;
        fill(project);
        QUndoStack stack;
        QSignalSpy params(&project, &Project::deviceParamChanged);
        QSignalSpy devices(&project, &Project::devicesChanged);
        QSignalSpy states(&project, &Project::deviceStateChanged);
        QSignalSpy chains(&project, &Project::chainChanged);

        for (double gain : {-1.0, -2.0}) {
            stack.push(new SetDeviceParamCommand(&project, "a", "inner", "gain",
                                                 project.device("a", "inner").params.value("gain"), gain, kGesture));
        }
        QCOMPARE(stack.count(), 1);
        QCOMPARE(project.device("a", "inner").params.value("gain"), -2.0);  // (in a rack too)
        QCOMPARE(params.takeFirst(), (QVariantList{"a", "inner", "gain"}));
        QCOMPARE(stack.undoText(), QStringLiteral("Change Device Parameter"));

        // A macro and what it moves, at once.
        const QMap<DeviceParam, double> old{{{"rack", "macro1"}, 0.0}, {{"inner", "gain"}, -2.0}};
        stack.push(new SetDeviceParamsCommand(&project, "a", old, {{{"rack", "macro1"}, 0.5}, {{"inner", "gain"}, -35.0}},
                                              "Change Macro", kGesture));
        stack.push(new SetDeviceParamsCommand(&project, "a", old, {{{"rack", "macro1"}, 1.0}, {{"inner", "gain"}, -70.0}},
                                              "Change Macro", kGesture));
        QCOMPARE(stack.count(), 2);
        QCOMPARE(project.device("a", "rack").params.value("macro1"), 1.0);
        stack.undo();
        QCOMPARE(project.device("a", "rack").params.value("macro1"), 0.0);
        QCOMPARE(project.device("a", "inner").params.value("gain"), -2.0);
        stack.redo();

        stack.push(new SetDeviceEnabledCommand(&project, "a", "u", false));
        QVERIFY(!project.device("a", "u").enabled);
        QCOMPARE(stack.undoText(), QStringLiteral("Deactivate Device"));
        stack.push(new SetDeviceSidechainCommand(&project, "a", "u", std::nullopt, Sidechain{"b"}, "Change Sidechain"));
        QVERIFY((project.device("a", "u").sidechain == Sidechain{"b"}));
        stack.push(new SetDeviceStateCommand(&project, "a", "u", std::nullopt, QString("c3RhdGU="), "Load Preset"));
        QCOMPARE(project.device("a", "u").state, std::optional<QString>("c3RhdGU="));
        QCOMPARE(states.takeFirst(), (QVariantList{"a", "u"}));
        stack.push(new SetMacrosCommand(&project, "a", "rack", {}, {MacroMapping{0, "inner", "gain"}}, "Map Macro"));
        QCOMPARE(project.device("a", "rack").macros.size(), size_t(1));
        stack.push(new SetDeviceNameCommand(&project, "a", "rack", std::nullopt, QString("Glue"), "Rename Rack"));
        QCOMPARE(project.device("a", "rack").name, std::optional<QString>("Glue"));
        for (double volume : {-3.0, -6.0}) {
            stack.push(new UpdateChainCommand(&project, "a", "c1", ChainField::VolumeDb,
                                              chainValue(project.chain("a", "c1"), ChainField::VolumeDb), volume,
                                              "Change Chain Volume", kGesture));
        }
        QCOMPARE(project.chain("a", "c1").volumeDb, -6.0);
        QCOMPARE(chains.takeFirst(), (QVariantList{"a", "c1"}));
        stack.push(new UpdateChainCommand(&project, "a", "c1", ChainField::Name, QString("One"), QString("Dry"),
                                          "Rename Chain"));
        QCOMPARE(project.chain("a", "c1").name, QStringLiteral("Dry"));
        const int steps = stack.count();
        for (int i = 0; i < 7; ++i) stack.undo();
        QCOMPARE(stack.index(), steps - 7);
        const Device& u = project.device("a", "u");
        QVERIFY(u.enabled && !u.sidechain && !u.state);
        QVERIFY(project.device("a", "rack").macros.empty() && !project.device("a", "rack").name);
        QCOMPARE(project.chain("a", "c1").volumeDb, 0.0);
        QCOMPARE(project.chain("a", "c1").name, QStringLiteral("One"));
        QVERIFY(devices.count() >= 8);
    }

    void envelopesAndTheirMerging() {
        Project project;
        fill(project);
        QUndoStack stack;
        QSignalSpy changed(&project, &Project::automationChanged);
        const Envelope first{{1.0, 0.5}};
        stack.push(new SetEnvelopeCommand(&project, "a", autom::kMixerVolume, {}, first, "Add Automation Point", kGesture));
        stack.push(new SetEnvelopeCommand(&project, "a", autom::kMixerVolume, first, {{2.0, 0.25}}, "Move Automation",
                                          kGesture));
        QCOMPARE(stack.count(), 1);  // a point added and dragged at once: one step
        QCOMPARE(project.envelope("a", autom::kMixerVolume), (Envelope{{2.0, 0.25}}));
        QCOMPARE(changed.takeFirst(), (QVariantList{"a", autom::kMixerVolume}));
        stack.undo();
        QVERIFY(project.envelope("a", autom::kMixerVolume).empty());
        QVERIFY(!project.automation("a").contains(autom::kMixerVolume));  // empty: no automation
        stack.redo();

        // Several lanes at once (a range moved on several), merging per gesture.
        const QMap<LaneRef, Envelope> old{{{"a", autom::kMixerVolume}, {{2.0, 0.25}}}, {{kMaster, autom::kMixerPan}, {}}};
        stack.push(new SetEnvelopesCommand(&project, old,
                                           {{{"a", autom::kMixerVolume}, {{3.0, 0.25}}}, {{kMaster, autom::kMixerPan}, {{0.0, 1.0}}}},
                                           "Move Automation", kOtherGesture));
        stack.push(new SetEnvelopesCommand(&project, old,
                                           {{{"a", autom::kMixerVolume}, {{4.0, 0.25}}}, {{kMaster, autom::kMixerPan}, {{0.0, 0.0}}}},
                                           "Move Automation", kOtherGesture));
        QCOMPARE(stack.count(), 2);
        QCOMPARE(project.envelope(kMaster, autom::kMixerPan), (Envelope{{0.0, 0.0}}));
        stack.undo();
        QCOMPARE(project.envelope("a", autom::kMixerVolume), (Envelope{{2.0, 0.25}}));
        QVERIFY(project.envelope(kMaster, autom::kMixerPan).empty());
    }

    void arrangingTracks() {
        Project project;
        Track group = test::makeTrack("g", "G", kGroupKind);
        ProjectContents contents;
        contents.tracks = {test::makeTrack("a", "A"), group, test::makeTrack("b", "B")};
        project.replaceContents(contents);
        QUndoStack stack;
        QSignalSpy arranged(&project, &Project::tracksArranged);
        const TrackTree before = project.tree();
        const TrackTree after{{"g", std::nullopt}, {"a", "g"}, {"b", "g"}};
        stack.push(new ArrangeTracksCommand(&project, before, after, "Group Tracks"));
        QCOMPARE(project.tree(), after);
        QCOMPARE(project.descendants("g").size(), size_t(2));
        stack.undo();
        QCOMPARE(project.tree(), before);
        QCOMPARE(arranged.count(), 2);
    }

    void viewStateChangesDirectly() {
        Project project;
        fill(project);
        QSignalSpy folded(&project, &Project::devicesFolded);
        QSignalSpy views(&project, &Project::automationViewChanged);
        project.setDevicesFolded("a", {"u", "rack"}, true);
        QVERIFY(project.isDeviceFolded("u") && project.isDeviceFolded("rack"));
        project.setDevicesFolded("a", {"u"}, true);  // nothing new: no signal
        QCOMPARE(folded.count(), 1);
        project.setDevicesFolded("a", {"u"}, false);
        QVERIFY(!project.isDeviceFolded("u"));
        QCOMPARE(folded.count(), 2);
        project.addFoldedDevices({"x"});  // (devices about to come: their devicesChanged follows)
        QVERIFY(project.isDeviceFolded("x"));
        QCOMPARE(folded.count(), 2);
        project.setAutomationView("a", AutomationView{true, autom::kMixerPan, {}});
        QCOMPARE(views.takeFirst(), QVariantList{"a"});
        QCOMPARE(project.automationView("a").key, std::optional<QString>(autom::kMixerPan));
    }

    void clearingAndReplacing() {
        Project project;
        fill(project);
        project.setPath("/somewhere/song.gilproj");
        QSignalSpy reset(&project, &Project::reset);
        QSignalSpy path(&project, &Project::pathChanged);
        project.clear();
        QCOMPARE(reset.count(), 1);
        QCOMPARE(path.count(), 1);
        QVERIFY(project.tracks().empty() && project.returns().empty() && project.path().isEmpty());
        QVERIFY(project.master().isMaster() && project.tempo() == 120.0 && project.loopEnd() == 16.0);
        QCOMPARE(project.property("tempo").toDouble(), 120.0);
        QCOMPARE(project.property("timeSignatureText").toString(), QStringLiteral("4/4"));
    }
};

QTEST_GUILESS_MAIN(TestCommands)
#include "test_commands.moc"
