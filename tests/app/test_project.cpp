// The project model's queries: the group tree, the routing graph, freezing,
// devices in racks (the group tree's invariant, return letters, the frozen
// clip, and the rules the editor relies on).

#include "TestSupport.h"

#include "model/Devices.h"
#include "model/Errors.h"
#include "model/Ids.h"
#include "model/Project.h"
#include "model/Routing.h"

#include <QSignalSpy>
#include <QTest>

using namespace sub::app;

namespace {

std::vector<Track> rows(std::initializer_list<std::pair<const char*, const char*>> entries) {
    std::vector<Track> tracks;
    for (const auto& [id, parent] : entries) {
        const QString name = QString::fromLatin1(id);
        tracks.push_back(test::makeTrack(name, name, name.startsWith('g') ? kGroupKind : kAudioKind,
                                         parent ? std::optional<QString>(QString::fromLatin1(parent)) : std::nullopt));
    }
    return tracks;
}

QStringList idsOf(const std::vector<const Track*>& tracks) {
    QStringList ids;
    for (const Track* t : tracks) ids.append(t->id);
    return ids;
}

Device withSidechain(const QString& id, const Sidechain& sidechain) {
    Device device = test::makeDevice(id, "compressor");
    device.sidechain = sidechain;
    return device;
}

// g1 [a, g2 [b]], c; returns r1, r2.
void fill(Project& project) {
    ProjectContents contents;
    contents.tracks = rows({{"g1", nullptr}, {"a", "g1"}, {"g2", "g1"}, {"b", "g2"}, {"c", nullptr}});
    contents.returns = {test::makeTrack("r1", "A Return", kReturnKind), test::makeTrack("r2", "B Return", kReturnKind)};
    project.replaceContents(contents);
}

}  // namespace

class TestProject : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { test::prepareApplication(); }

    void theTreeInvariant() {
        QVERIFY(!treeProblem(rows({{"g1", nullptr}, {"a", "g1"}, {"g2", "g1"}, {"b", "g2"}, {"c", "g1"}, {"d", nullptr}})));
        QVERIFY(treeProblem(rows({{"a", "g1"}, {"g1", nullptr}})));  // its group comes later
        QVERIFY(treeProblem(rows({{"g1", nullptr}, {"a", "g1"}, {"b", nullptr}, {"c", "g1"}})));  // not together
        QVERIFY(treeProblem(rows({{"g1", nullptr}, {"a", "g1"}, {"b", "a"}})));  // not in a group
        QCOMPARE(treeProblem(rows({{"g1", nullptr}, {"g1", nullptr}})), std::optional<QString>("g1 is listed twice"));
        std::vector<Track> strays = rows({{"c", "g1"}, {"g1", nullptr}, {"a", "g1"}, {"b", "a"}});
        repairTree(strays);
        QVERIFY(!strays[0].parent && strays[2].parent == "g1" && !strays[3].parent);
        QVERIFY(!treeProblem(strays));
    }

    void returnLetters() {
        QStringList letters;
        for (int i : {0, 1, 25, 26, 27, 52}) letters.append(returnLetter(i));
        QCOMPARE(letters, (QStringList{"A", "B", "Z", "AA", "AB", "BA"}));
    }

    void aFrozenTrackPlaysItsAudioWarpedAtOtherTempos() {
        const Freeze freeze{"/x/Freeze/t.wav", 2.0, 120.0};
        const Clip played = freeze.clip("t", "Track");
        QCOMPARE(played.id, QStringLiteral("frozen-t"));
        QVERIFY(played.startBeat == 0.0 && played.path == freeze.path && played.durationSec == 2.0);
        QVERIFY(played.sourceDurationSec == 2.0 && played.warpMode == kDefaultWarpMode);
        QVERIFY(played.isWarped() && played.segmentBpm == 120.0);
        QVERIFY(std::abs(played.lengthBeats(60.0) - 4.0) < 1e-9);  // its length in beats is fixed
    }

    void ids() {
        const QString id = newId();
        QCOMPARE(id.size(), 12);
        QVERIFY(id != newId());
        for (QChar c : id) QVERIFY((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }

    void tracksReturnsAndTheMaster() {
        Project project;
        fill(project);
        QCOMPARE(project.track(kMaster).id, kMaster);
        QVERIFY(project.track("r1").isReturn() && !project.hasTrack("r1") && project.hasReturn("r1"));
        QVERIFY(project.hasOwner("r1") && project.hasOwner(kMaster) && project.hasOwner("a") && !project.hasOwner("zz"));
        QVERIFY_THROWS_EXCEPTION(std::out_of_range, project.track("zz"));
        QVERIFY(project.findTrack("zz") == nullptr);
        QCOMPARE(project.owners(), (QStringList{"g1", "a", "g2", "b", "c", "r1", "r2", kMaster}));
        QCOMPARE(idsOf(project.allTracks()), (QStringList{"g1", "a", "g2", "b", "c", "r1", "r2", kMaster}));
        QCOMPARE(idsOf(project.senders()), (QStringList{"g1", "a", "g2", "b", "c", "r1", "r2"}));
        QCOMPARE(project.returnIndex("r2"), 1);
        QCOMPARE(project.returnLetter("r2"), QStringLiteral("B"));
        QCOMPARE(project.inputName(kMaster), QStringLiteral("Resampling"));
        QCOMPARE(project.inputName("a"), QStringLiteral("a"));
        QCOMPARE(idsOf(project.inputSources("a")), (QStringList{"g1", "g2", "b", "c", "r1", "r2"}));
        QCOMPARE(project.uniqueTrackName("a"), QStringLiteral("a 2"));
        QCOMPARE(project.uniqueTrackName("A Return"), QStringLiteral("A Return 2"));
        QCOMPARE(project.uniqueTrackName("new"), QStringLiteral("new"));
        QCOMPARE(project.nextColor(), kTrackColors[5]);
    }

    void theGroupTree() {
        Project project;
        fill(project);
        QCOMPARE(project.subtreeEnd(0), 4);
        QCOMPARE(project.subtreeEnd(1), 2);
        QCOMPARE(idsOf(project.descendants("g1")), (QStringList{"a", "g2", "b"}));
        QCOMPARE(idsOf(project.children("g1")), (QStringList{"a", "g2"}));
        QCOMPARE(project.ancestors("b"), (QStringList{"g2", "g1"}));
        QVERIFY(project.isDescendant("b", "g1") && !project.isDescendant("c", "g1"));
        QCOMPARE(project.depth("b"), 2);
        QCOMPARE(project.withContents({"g2", "c", "nothing"}), (QStringList{"g2", "b", "c"}));
        QCOMPARE(project.withContents({"b", "g1"}), (QStringList{"g1", "a", "g2", "b"}));  // in order, once
        QCOMPARE(project.parentAt(1), std::optional<QString>("g1"));
        QVERIFY(!project.parentAt(4) && !project.parentAt(5) && !project.parentAt(-1));
        QVERIFY(!project.isHidden("b"));
        project.updateTrack("g2", TrackField::Folded, true);
        QVERIFY(project.isHidden("b") && !project.isHidden("g2"));
        QCOMPARE(project.tree()[3], (TreeEntry{"b", "g2"}));
    }

    void arrangingTheTracks() {
        Project project;
        fill(project);
        QSignalSpy arranged(&project, &Project::tracksArranged);
        TrackTree tree = project.tree();
        std::rotate(tree.begin(), tree.begin() + 4, tree.end());  // c first
        project.arrangeTracks(tree);
        QCOMPARE(project.tracks()[0].id, QStringLiteral("c"));
        QCOMPARE(arranged.count(), 1);
        // A tree that isn't one changes nothing.
        const TrackTree before = project.tree();
        TrackTree bad = before;
        bad[0].parent = "g1";  // c in a group that comes after it
        QVERIFY_THROWS_EXCEPTION(EditError, project.arrangeTracks(bad));
        QCOMPARE(project.tree(), before);
        TrackTree missing(before.begin(), before.end() - 1);
        QVERIFY_THROWS_EXCEPTION(EditError, project.arrangeTracks(missing));
        QCOMPARE(project.tree(), before);
        QCOMPARE(arranged.count(), 1);
    }

    void theRoutingGraph() {
        Project project;
        ProjectContents contents;
        contents.tracks = rows({{"g", nullptr}, {"a", "g"}, {"b", nullptr}, {"c", nullptr}});
        contents.returns = {test::makeTrack("r1", "R1", kReturnKind), test::makeTrack("r2", "R2", kReturnKind)};
        contents.tracks[1].sends = {{"r1", Send{}}};
        contents.returns[0].sends = {{"r2", Send{}}};
        contents.tracks[2].inputTrack = "g";  // b takes the group's output
        contents.tracks[3].devices = {withSidechain("sc", Sidechain{"b"})};  // c hears b
        project.replaceContents(contents);
        const RoutingGraph graph = routingGraph(project.tracks(), project.returns());
        QVERIFY(graph.value("a").contains("g") && graph.value("a").contains("r1"));
        QVERIFY(graph.value("g").contains("b") && graph.value("b").contains("c"));
        QVERIFY(feeds(graph, "a", "c") && feeds(graph, "a", "a") && !feeds(graph, "c", "a"));
        // Sends: a return that feeds the track (or is it) would close a cycle.
        QVERIFY(project.wouldCycle("r2", "r1") && project.wouldCycle("r1", "r1") && !project.wouldCycle("c", "r1"));
        QVERIFY(project.sendTargets("r2").empty());  // (r1 feeds it, and it is itself)
        QCOMPARE(idsOf(project.sendTargets("r1")), QStringList{"r2"});
        QCOMPARE(idsOf(project.sendTargets("c")), (QStringList{"r1", "r2"}));
        QVERIFY(project.sendTargets(kMaster).empty());
        // Inputs: never the master's; not from what the track feeds.
        QVERIFY(!project.inputWouldCycle("a", kMaster));
        QVERIFY(project.inputWouldCycle("a", "c") && project.inputWouldCycle("a", "a") && !project.inputWouldCycle("c", "a"));
        // Sidechains: never from the master; anything on the master.
        QVERIFY(project.sidechainWouldCycle("a", kMaster) && !project.sidechainWouldCycle(kMaster, "c"));
        QVERIFY(project.sidechainWouldCycle("b", "c") && !project.sidechainWouldCycle("c", "a"));
        QCOMPARE(idsOf(project.sidechainSources("c")), (QStringList{"g", "a", "b", "r1", "r2"}));
    }

    void outputsThatGoWhereTheDefaultDoes() {
        const std::optional<QString> none;
        const std::optional<QString> group = QStringLiteral("g");
        QVERIFY(isDefaultOutput(Output::group(), none) && isDefaultOutput(Output::group(), group));
        QVERIFY(isDefaultOutput(Output::master(), none));  // (outside a group, its group is the master)
        QVERIFY(!isDefaultOutput(Output::master(), group));
        QVERIFY(isDefaultOutput(Output::track("g"), group));
        QVERIFY(!isDefaultOutput(Output::track("a"), group) && !isDefaultOutput(Output::track("g"), none));
        QVERIFY(!isDefaultOutput(Output::none(), none) && !isDefaultOutput(Output::sidechain("g"), group));
    }

    void freezing() {
        Project project;
        fill(project);
        QVERIFY(!project.frozenBy("b") && !project.isFrozen("b"));
        project.setFrozen("g2", Freeze{"/f/g2.wav", 1.0, 120.0});
        QCOMPARE(project.frozenBy("b"), std::optional<QString>("g2"));
        QCOMPARE(project.frozenBy("g2"), std::optional<QString>("g2"));
        project.setFrozen("g1", Freeze{"/f/g1.wav", 1.0, 120.0});
        QCOMPARE(project.frozenBy("b"), std::optional<QString>("g1"));  // the outermost
        QVERIFY(!project.frozenBy(kMaster) && !project.frozenBy("c") && !project.frozenBy("zz"));
        QCOMPARE(project.freezeProblem(kMaster), std::optional<QString>("The master can't be frozen"));
        QCOMPARE(project.freezeProblem("g1"), std::optional<QString>("g1 is frozen already"));
        QCOMPARE(project.freezeProblem("a"), std::optional<QString>("a is in g1, which is frozen"));
        QVERIFY(!project.freezeProblem("c"));
        // A sidechain taking a track's signal before its devices, or after one of them, isn't in its frozen audio.
        Track master = project.master();
        master.devices = {withSidechain("sc", Sidechain{"c", kPreFx})};
        ProjectContents contents;
        contents.tracks = project.tracks();
        contents.master = master;
        project.replaceContents(contents);
        QVERIFY(project.freezeProblem("c")->startsWith("Master takes c's signal before its devices"));
        QCOMPARE(project.flattenProblem("g1"), std::optional<QString>("g1 can't be flattened: only audio and MIDI tracks can"));
        QCOMPARE(project.flattenProblem("c"), std::optional<QString>("Freeze c first"));
    }

    void devicesInRacks() {
        Device inner = test::makeDevice("inner", "utility");
        Device deep = newRack({newChain("Deep", {inner})});
        deep.id = "deep";
        deep.chains[0].id = "deepChain";
        Device rack = newRack({newChain("One", {test::makeDevice("u1", "utility"), deep}), newChain("Two")});
        rack.id = "rack";
        rack.chains[0].id = "c1";
        rack.chains[1].id = "c2";
        std::vector<Device> devices{test::makeDevice("first", "utility"), rack};
        QStringList order;
        for (const Device* d : iterDevices(devices)) order.append(d->id);
        QCOMPARE(order, (QStringList{"first", "rack", "u1", "deep", "inner"}));  // depth first
        QStringList chains;
        for (const ConstRackChain& rc : iterChains(std::as_const(devices))) chains.append(rc.rack->id + ":" + rc.chain->id);
        QCOMPARE(chains, (QStringList{"rack:c1", "deep:deepChain", "rack:c2"}));
        QCOMPARE(devicePath(devices, "inner"), (std::optional<std::vector<int>>{{1, 0, 1, 0, 0}}));
        QVERIFY(!devicePath(devices, "nowhere"));
        QCOMPARE(deviceAt(devices, {1, 0, 1}).id, QStringLiteral("deep"));
        QVERIFY(findDevice(devices, "inner") != nullptr && findDevice(devices, "nowhere") == nullptr);
        QCOMPARE(containerOf(devices, "inner"), std::optional<QString>("deepChain"));
        QVERIFY(!containerOf(devices, "first") && !containerOf(devices, "nowhere"));
        const ConstRackChain found = findChain(std::as_const(devices), "deepChain");
        QVERIFY(found.rack == &devices[1].chains[0].devices[1] && found.chain == &found.rack->chains[0]);
        QCOMPARE(findChain(devices, "c2").chain, &devices[1].chains[1]);  // (one to change in place)
        QVERIFY(findChain(devices, "nowhere").rack == nullptr && findChain(devices, "nowhere").chain == nullptr);
        QCOMPARE(chainIndex(rack, "c2"), 1);
        QCOMPARE(chainIndex(rack, "deepChain"), -1);  // (a chain of a rack in it, not its own)
        QCOMPARE(chainDevices(devices, std::nullopt), &devices);
        QCOMPARE(chainDevices(devices, QString("c2"))->size(), size_t(0));
        QVERIFY(chainDevices(devices, QString("nowhere")) == nullptr);
        QCOMPARE(rackDepth(devices, std::nullopt), 0);
        QCOMPARE(rackDepth(devices, QString("c1")), 1);
        QCOMPARE(rackDepth(devices, QString("deepChain")), 2);
        QCOMPARE(rackHeight(rack), 2);
        QCOMPARE(rackHeight(inner), 0);

        // New ids for a copy: its macro mappings follow; mappings to devices not in it go.
        Device copy = rack;
        copy.macros = {MacroMapping{0, "inner", "gain"}, MacroMapping{1, "elsewhere", "gain"}};
        refreshIds(copy);
        QVERIFY(copy.id != "rack" && copy.chains[0].id != "c1");
        const QString innerCopy = copy.chains[0].devices[1].chains[0].devices[0].id;
        QVERIFY(innerCopy != "inner");
        QVERIFY((copy.macros == std::vector<MacroMapping>{{0, innerCopy, "gain"}}));

        Project project;
        ProjectContents contents;
        contents.tracks = {test::makeTrack("t", "T")};
        contents.tracks[0].devices = devices;
        project.replaceContents(contents);
        QCOMPARE(project.device("t", "inner").id, QStringLiteral("inner"));
        QVERIFY(project.hasDevice("t", "inner") && !project.hasDevice("t", "nowhere") && !project.hasDevice("zz", "inner"));
        QCOMPARE(project.deviceOwner("inner"), std::optional<QString>("t"));
        QVERIFY(!project.deviceOwner("nowhere"));
        QCOMPARE(project.chain("t", "deepChain").name, QStringLiteral("Deep"));
        QCOMPARE(project.chainRack("t", "deepChain").id, QStringLiteral("deep"));
        QVERIFY_THROWS_EXCEPTION(std::out_of_range, project.chain("t", "nowhere"));
        QVERIFY_THROWS_EXCEPTION(std::out_of_range, project.device("t", "nowhere"));
    }

    void tracksHaveInputsAndClips() {
        Track audio = test::makeTrack("a", "A");
        QVERIFY(!audio.hasInput());
        audio.input = {0};
        QVERIFY(audio.hasInput() && audio.hasClips());
        audio.input.clear();
        audio.inputTrack = kMaster;
        QVERIFY(audio.hasInput());
        Track midi = test::makeTrack("m", "M", kMidiKind);
        QVERIFY(midi.hasInput());  // every MIDI input, as a new MIDI track hears
        midi.midiInput.reset();
        QVERIFY(!midi.hasInput());
        QVERIFY(!test::makeTrack("g", "G", kGroupKind).hasClips() && !newMaster().hasClips());
        QVERIFY(newMaster().isMaster() && newMaster().id == kMaster && newMaster().name == "Master");
        // Settings by field, as commands set them.
        QCOMPARE(std::get<double>(audio.value(TrackField::VolumeDb)), 0.0);
        audio.setValue(TrackField::VolumeDb, 3);  // a whole number does for a level
        QCOMPARE(audio.volumeDb, 3.0);
        audio.setValue(TrackField::Sends, SendMap{{"r", Send{-3.0, true}}});
        QVERIFY((audio.sends == SendMap{{"r", Send{-3.0, true}}}));
        QCOMPARE(trackFieldName(TrackField::InputTrack), QStringLiteral("input_track"));
        QCOMPARE(trackFieldFromName("volume_db"), std::optional<TrackField>(TrackField::VolumeDb));
        QVERIFY(!trackFieldFromName("kind"));  // (not a setting: fixed)
    }

    void endBeat() {
        Project project;
        QCOMPARE(project.endBeat(), 0.0);
        ProjectContents contents;
        contents.tracks = {test::makeTrack("a", "A"), test::makeTrack("m", "M", kMidiKind)};
        contents.tracks[0].clips = {Clip::audio("c", "a.wav", "a", 2.0, 1.0)};  // 2 beats at 120
        contents.tracks[1].clips = {Clip::midi("m", "m", 1.0, 2.0)};
        project.replaceContents(contents);
        QCOMPARE(project.endBeat(), 4.0);
        QCOMPARE(project.clip("m", "m").endBeat(), 3.0);
        QVERIFY(project.findClip("m", "zz") == nullptr);
    }
};

QTEST_GUILESS_MAIN(TestProject)
#include "test_project.moc"
