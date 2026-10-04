// Project files: saving and loading, paths that move with the project, files of
// other versions, and what an edited file can't have (tests/test_serialization.py,
// and the saving parts of the groups, sends, resampling, sidechain, racks and
// freeze model tests). Files as the Python version writes them load and save
// unchanged.

#include "TestSupport.h"

#include "io/Serialization.h"
#include "model/Automation.h"
#include "model/Errors.h"
#include "model/Project.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTest>

using namespace sub::app;

namespace {

QJsonObject readJson(const QString& path) {
    QFile file(path);
    file.open(QIODevice::ReadOnly);
    return QJsonDocument::fromJson(file.readAll()).object();
}

void writeJson(const QString& path, const QJsonObject& data) {
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.write(QJsonDocument(data).toJson());
}

void touch(const QString& path) {
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    file.open(QIODevice::WriteOnly);
}

// A copy of `object` with `key` set to `value`.
QJsonObject with(QJsonObject object, const QString& key, const QJsonValue& value) {
    object[key] = value;
    return object;
}

// A JSON value at a path of keys and indices ("tracks", 0, "devices", ...), set.
void setAt(QJsonValue& root, const QVariantList& path, const QJsonValue& value) {
    if (path.isEmpty()) {
        root = value;
        return;
    }
    const QVariant step = path.first();
    const QVariantList rest = path.mid(1);
    if (step.typeId() == QMetaType::QString) {
        QJsonObject object = root.toObject();
        QJsonValue child = object.value(step.toString());
        setAt(child, rest, value);
        object[step.toString()] = child;
        root = object;
    } else {
        QJsonArray array = root.toArray();
        QJsonValue child = array.at(step.toInt());
        setAt(child, rest, value);
        array[step.toInt()] = child;
        root = array;
    }
}

QJsonObject setIn(const QJsonObject& data, const QVariantList& path, const QJsonValue& value) {
    QJsonValue root = data;
    setAt(root, path, value);
    return root.toObject();
}

ProjectContents makeContents(const QString& audioPath) {
    ProjectContents p;
    p.tempo = 97.5;
    p.timeSignature = {6, 8};
    p.loopEnabled = true;
    p.loopStart = 3.0;
    p.loopEnd = 9.0;
    Track master = newMaster();
    master.volumeDb = -3.0;
    master.devices = {test::makeDevice("m1", "utility", {{"gain", -1.0}, {"pan", 0.0}, {"width", 100.0}})};
    master.automation.insert(automation::deviceKey("m1", "gain"), {{0.0, 0.5}});
    p.master = master;
    Track drums = test::makeTrack("t1", "Drums");
    drums.volumeDb = -6.0;
    drums.pan = 0.25;
    drums.mute = true;
    drums.height = 90;
    drums.devices = {test::makeDevice("d1", "utility", {{"gain", -2.0}, {"pan", 0.0}, {"width", 50.0}})};
    Clip kick = Clip::audio("c1", audioPath, "kick", 1.5, 2.0, 0.25, 4.0);
    kick.gainDb = -1.0;
    kick.warp = true;
    kick.warpMode = "Formants";
    kick.segmentBpm = 128.0;
    kick.transpose = -3;
    kick.detune = 12.0;
    kick.pan = -0.5;
    drums.clips = {kick};
    Track empty = test::makeTrack("t2", "Empty");
    empty.color = "#8bc5ff";
    empty.solo = true;
    p.tracks = {drums, empty};
    return p;
}

std::unique_ptr<Project> makeProject(const QString& audioPath) {
    auto project = std::make_unique<Project>();
    project->replaceContents(makeContents(audioPath));
    return project;
}

QJsonObject view(bool shown = false, const QJsonValue& key = QJsonValue::Null, const QJsonArray& lanes = {}) {
    return {{"shown", shown}, {"key", key}, {"lanes", lanes}};
}

QJsonObject utility(const QString& id, double gain) {
    return {{"id", id}, {"kind", "utility"}, {"enabled", true},
            {"params", QJsonObject{{"gain", gain}, {"pan", 0.0}, {"width", 100.0}}}};
}

}  // namespace

class TestSerialization : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { test::prepareApplication(); }

    void roundTrip() {
        test::TempDir dir;
        const QString audio = dir.path("audio/kick.wav");
        touch(audio);
        auto original = makeProject(audio);
        const QString target = dir.path("song.gilproj");
        QSignalSpy pathChanged(original.get(), &Project::pathChanged);
        saveProject(*original, target);
        QCOMPARE(original->path(), target);
        QCOMPARE(pathChanged.count(), 1);
        QVERIFY(!QFileInfo::exists(target + ".tmp"));

        Project loaded;
        QSignalSpy reset(&loaded, &Project::reset);
        loadProject(loaded, target);
        QCOMPARE(reset.count(), 1);
        QCOMPARE(projectToJson(loaded, target), projectToJson(*original, target));
        QVERIFY((loaded.timeSignature() == TimeSignature{6, 8}));
        QCOMPARE(loaded.tracks()[0].clips[0].path, audio);
        QCOMPARE(loaded.path(), target);
        const QJsonObject clip = readJson(target)["tracks"].toArray()[0].toObject()["clips"].toArray()[0].toObject();
        QCOMPARE(clip["relative_path"].toString(), QStringLiteral("audio/kick.wav"));
        QVERIFY(!clip.contains("reversed_from"));
    }

    void relativePathFallbackWhenFolderMoves() {
        test::TempDir dir;
        const QString folder = dir.path("old");
        const QString audio = folder + "/audio/kick.wav";
        touch(audio);
        auto project = makeProject(audio);
        saveProject(*project, folder + "/song.gilproj");

        const QString moved = dir.path("new");
        QVERIFY(QDir().rename(folder, moved));
        Project loaded;
        loadProject(loaded, moved + "/song.gilproj");
        QCOMPARE(loaded.tracks()[0].clips[0].path, moved + "/audio/kick.wav");
    }

    void aReversedClipKeepsTheFileItCameFrom() {
        test::TempDir dir;
        const QString folder = dir.path("old");
        const QString original = folder + "/kick.wav";
        const QString backwards = folder + "/Reversed/kick R.wav";
        touch(original);
        touch(backwards);
        Track drums = test::makeTrack("t1", "Drums");
        Clip clip = Clip::audio("c1", backwards, "kick", 0.0, 1.0);
        clip.reversedFrom = original;
        drums.clips = {clip};
        Project project;
        ProjectContents contents;
        contents.tracks = {drums};
        project.replaceContents(contents);
        saveProject(project, folder + "/song.gilproj");
        const QJsonObject saved = readJson(folder + "/song.gilproj")["tracks"].toArray()[0].toObject()["clips"].toArray()[0].toObject();
        QCOMPARE(saved["reversed_from_relative"].toString(), QStringLiteral("kick.wav"));

        const QString moved = dir.path("new");
        QVERIFY(QDir().rename(folder, moved));  // (both files are found again, relative to the project)
        Project loaded;
        loadProject(loaded, moved + "/song.gilproj");
        const Clip& again = loaded.tracks()[0].clips[0];
        QCOMPARE(again.path, moved + "/Reversed/kick R.wav");
        QCOMPARE(again.reversedFrom, moved + "/kick.wav");
        const QJsonObject data = projectToJson(loaded);
        QCOMPARE(data["tracks"].toArray()[0].toObject()["clips"].toArray()[0].toObject()["reversed_from"].toString(),
                 moved + "/kick.wav");
        QVERIFY(data["tracks"].toArray()[0].toObject()["clips"].toArray()[0].toObject()["reversed_from_relative"].isNull());
    }

    void rejectsForeignFiles() {
        test::TempDir dir;
        const QString bad = dir.path("x.gilproj");
        writeJson(bad, QJsonObject{{"format", "something-else"}});
        Project project;
        QSignalSpy reset(&project, &Project::reset);
        QVERIFY_THROWS_EXCEPTION(ProjectFileError, loadProject(project, bad));
        QFile file(bad);
        file.open(QIODevice::WriteOnly);
        file.write("{not json");
        file.close();
        try {
            loadProject(project, bad);
            QFAIL("read a file that isn't JSON");
        } catch (const ProjectFileError& error) {
            QVERIFY(error.message().startsWith("Could not read x.gilproj"));
        }
        QVERIFY_THROWS_EXCEPTION(ProjectFileError, loadProject(project, dir.path("missing.gilproj")));
        QCOMPARE(reset.count(), 0);
    }

    void newerVersionsAndDamagedFilesLeaveTheProjectAsItWas() {
        test::TempDir dir;
        auto project = makeProject(dir.path("kick.wav"));
        const QJsonObject data = projectToJson(*project);
        Project loaded;
        loadInto(loaded, data);
        QSignalSpy reset(&loaded, &Project::reset);
        try {
            loadInto(loaded, with(data, "version", kProjectVersion + 1));
            QFAIL("loaded a file from a newer version");
        } catch (const ProjectFileError& error) {
            QCOMPARE(error.message(), QStringLiteral("This project was saved by a newer version of SUBstation"));
        }
        const QString target = dir.path("broken.gilproj");
        QJsonObject broken = data;
        QJsonArray tracks = broken["tracks"].toArray();
        QJsonObject track = tracks[0].toObject();
        track.remove("color");
        tracks[0] = track;
        broken["tracks"] = tracks;
        writeJson(target, broken);
        try {
            loadProject(loaded, target);
            QFAIL("loaded a damaged file");
        } catch (const ProjectFileError& error) {
            QCOMPARE(error.message(), QStringLiteral("broken.gilproj is damaged: 'color'"));
        }
        writeJson(target, with(data, "tempo", "fast"));
        QVERIFY_THROWS_EXCEPTION(ProjectFileError, loadProject(loaded, target));
        QCOMPARE(reset.count(), 0);
        QCOMPARE(loaded.tempo(), 97.5);
        // Numbers written as text read as they would have.
        loadInto(loaded, with(data, "tempo", " 140.5 "));
        QCOMPARE(loaded.tempo(), 140.5);
    }

    void oldWarpModeNamesLoadAsTheirEquivalents() {
        test::TempDir dir;
        const QString audio = dir.path("kick.wav");
        touch(audio);
        const QString target = dir.path("song.gilproj");
        auto project = makeProject(audio);
        saveProject(*project, target);
        const QJsonObject data = readJson(target);
        const std::pair<const char*, const char*> names[] = {
            {"Beats", "Transients"}, {"Tones", "Standard"},  {"Complex", "Standard"}, {"Texture", "Smooth"},
            {"Complex Pro", "Formants"}, {"Re-Pitch", "Re-Pitch"}, {"Nonsense", "Standard"}};
        for (const auto& [old, now] : names) {
            writeJson(target, setIn(data, {"tracks", 0, "clips", 0, "warp_mode"}, QString::fromLatin1(old)));
            Project loaded;
            loadProject(loaded, target);
            QCOMPARE(loaded.tracks()[0].clips[0].warpMode, QString::fromLatin1(now));
        }
    }

    void roundTripKeepsTheMastersDevices() {
        test::TempDir dir;
        const QString audio = dir.path("kick.wav");
        touch(audio);
        const QString target = dir.path("song.gilproj");
        auto project = makeProject(audio);
        saveProject(*project, target);
        Project loaded;
        loadProject(loaded, target);
        const Track& master = loaded.track(kMaster);
        QVERIFY(&master == &loaded.master() && master.isMaster());
        QVERIFY(!loaded.hasTrack(kMaster));
        QCOMPARE(master.devices.size(), size_t(1));
        QVERIFY(master.devices[0].id == "m1" && master.devices[0].kind == "utility" &&
                master.devices[0].params.value("gain") == -1.0);
        QCOMPARE(master.volumeDb, -3.0);
        QVERIFY((master.automation == EnvelopeMap{{automation::deviceKey("m1", "gain"), {{0.0, 0.5}}}}));
    }

    // A project saved before the master had devices (version 4).
    void oldProjectFilesLoadUnchanged() {
        const QJsonObject masterData{{"volume_db", -4.5},
                                     {"pan", 0.25},
                                     {"automation", QJsonObject{{"mixer:volume", QJsonArray{QJsonArray{1.0, 0.75, 0.0}}}}},
                                     {"automation_view", view(true, "mixer:volume")}};
        const QJsonObject trackData{{"id", "t1"},       {"kind", "audio"},  {"name", "Drums"},     {"color", "#ff94a6"},
                                    {"volume_db", 0.0}, {"pan", 0.0},       {"mute", false},       {"solo", false},
                                    {"height", 80},     {"devices", QJsonArray{}}, {"clips", QJsonArray{}},
                                    {"automation", QJsonObject{}}, {"automation_view", view()}};
        QJsonObject old{{"format", "gilstudio-project"},
                        {"version", 4},
                        {"tempo", 128.0},
                        {"time_signature", QJsonArray{4, 4}},
                        {"loop", QJsonObject{{"enabled", false}, {"start", 0.0}, {"end", 16.0}}},
                        {"automation_locked", false},
                        {"master", masterData},
                        {"tracks", QJsonArray{trackData}}};
        Project project;
        loadInto(project, old);
        const Track& master = project.master();
        QVERIFY(master.volumeDb == -4.5 && master.pan == 0.25 && master.devices.empty());
        QVERIFY((master.automation == EnvelopeMap{{automation::kMixerVolume, {{1.0, 0.75}}}}));
        QVERIFY((master.automationView == AutomationView{true, automation::kMixerVolume, {}}));
        QCOMPARE(project.tracks().size(), size_t(1));
        QCOMPARE(project.tracks()[0].id, QStringLiteral("t1"));
        QCOMPARE(project.tempo(), 128.0);
        // Saved again, it says the same, and that the master has no devices.
        const QJsonObject saved = projectToJson(project);
        QCOMPARE(saved["version"].toInt(), kProjectVersion);
        QCOMPARE(saved["master"].toObject(), with(masterData, "devices", QJsonArray{}));
        // Tracks from before version 6 have no input, Auto monitoring and aren't armed; before 8, no group;
        // before 9, no sends (nor returns).
        QJsonObject expected = trackData;
        expected["input"] = QJsonArray{};
        expected["monitor"] = "auto";
        expected["armed"] = false;
        expected["parent"] = QJsonValue::Null;
        expected["folded"] = false;
        expected["sends"] = QJsonObject{};
        QCOMPARE(saved["tracks"].toArray(), QJsonArray{expected});
        QCOMPARE(saved["returns"].toArray(), QJsonArray{});
        // Very old files have no master at all.
        old.remove("master");
        loadInto(project, old);
        QVERIFY(project.master().volumeDb == 0.0 && project.master().devices.empty() &&
                project.master().automation.isEmpty());
    }

    void roundTripKeepsInputsMonitoringAndArming() {
        test::TempDir dir;
        ProjectContents contents = makeContents(dir.path("kick.wav"));
        contents.tracks[0].input = {2, 3};
        contents.tracks[0].monitor = "in";
        contents.tracks[0].armed = true;
        contents.tracks[1].input = {0};
        Project project;
        project.replaceContents(contents);
        const QString target = dir.path("song.gilproj");
        saveProject(project, target);
        Project loaded;
        loadProject(loaded, target);
        QVERIFY((loaded.tracks()[0].input == std::vector<int>{2, 3}) && loaded.tracks()[0].monitor == "in" &&
                loaded.tracks()[0].armed);
        QVERIFY((loaded.tracks()[1].input == std::vector<int>{0}) && loaded.tracks()[1].monitor == "auto" &&
                !loaded.tracks()[1].armed);
        // Anything this version doesn't know comes back as no input, Auto.
        QJsonObject data = setIn(readJson(target), {"tracks", 0, "input"}, QJsonArray{0, 1, 2});
        data = setIn(data, {"tracks", 0, "monitor"}, "sometimes");
        loadInto(loaded, data);
        QVERIFY(loaded.tracks()[0].input.empty() && loaded.tracks()[0].monitor == "auto");
    }

    // A file using every feature, as the Python version writes it (key for key):
    // it loads, and saves the same.
    void aFileAsThePythonVersionWritesItLoadsAndSavesTheSame() {
        test::TempDir dir;
        const QString kick = dir.path("audio/kick.wav");
        const QString reversed = dir.path("Reversed/kick R.wav");
        touch(kick);
        touch(reversed);
        const QString projectFile = dir.path("song.gilproj");

        QJsonObject macros;
        for (int i = 1; i <= 8; ++i) macros[QStringLiteral("macro%1").arg(i)] = i == 1 ? 0.25 : 0.0;
        const QJsonObject rack{
            {"id", "rk"},
            {"kind", "rack"},
            {"enabled", true},
            {"params", macros},
            {"chains", QJsonArray{QJsonObject{{"id", "ch1"},
                                              {"name", "Dry"},
                                              {"volume_db", -3.0},
                                              {"pan", 0.5},
                                              {"mute", false},
                                              {"solo", true},
                                              {"devices", QJsonArray{utility("u2", -6.0)}}},
                                  QJsonObject{{"id", "ch2"},
                                              {"name", "Wet"},
                                              {"volume_db", 0.0},
                                              {"pan", 0.0},
                                              {"mute", true},
                                              {"solo", false},
                                              {"devices", QJsonArray{}}}}},
            {"macros", QJsonArray{QJsonObject{{"macro", 0}, {"device", "u2"}, {"param", "gain"}, {"low", 0.25}, {"high", 0.75}}}},
            {"name", "Glue"}};
        const QJsonObject sampler{{"id", "s1"}, {"kind", "sampler"}, {"enabled", false},
                                  {"params", QJsonObject{{"gain", -2.0}}}, {"state", "c2FtcGxlPS90bXAveC53YXYK"}};
        const QJsonObject plugin{{"id", "p1"},
                                 {"kind", "plugin"},
                                 {"enabled", true},
                                 {"params", QJsonObject{}},
                                 {"plugin", QJsonObject{{"format", "VST3"}, {"uid", "ABCDEF0123456789"}, {"name", "Verb"},
                                                        {"vendor", "Someone"}, {"path", "/plugins/Verb.vst3"},
                                                        {"instrument", false}}},
                                 {"state", "c3RhdGU="}};
        const QJsonObject missingState{{"id", "p2"},
                                       {"kind", "plugin"},
                                       {"enabled", true},
                                       {"params", QJsonObject{{"0", 0.5}}},
                                       {"plugin", QJsonObject{{"format", "VST3"}, {"uid", "FEDCBA"}, {"name", "Synth"},
                                                              {"vendor", ""}, {"path", ""}, {"instrument", true}}},
                                       {"state", QJsonValue::Null}};
        const QJsonObject compressor{{"id", "c1"}, {"kind", "compressor"}, {"enabled", true},
                                     {"params", QJsonObject{{"threshold", -18.0}}},
                                     {"sidechain", QJsonObject{{"track", "t1"}, {"tap", "post"}}}};
        const QJsonObject tapping{{"id", "c2"}, {"kind", "compressor"}, {"enabled", true},
                                  {"params", QJsonObject{{"threshold", -12.0}}},
                                  {"sidechain", QJsonObject{{"track", "t1"}, {"tap", "d1"}}}};
        auto track = [](const QString& id, const QString& kind, const QString& name) {
            return QJsonObject{{"id", id},          {"kind", kind},       {"name", name},           {"color", "#ffa529"},
                               {"volume_db", 0.0},  {"pan", 0.0},         {"mute", false},          {"solo", false},
                               {"height", 80},      {"devices", QJsonArray{}}, {"clips", QJsonArray{}},
                               {"automation", QJsonObject{}}, {"automation_view", view()},  {"input", QJsonArray{}},
                               {"monitor", "auto"}, {"armed", false},     {"parent", QJsonValue::Null},
                               {"folded", false},   {"sends", QJsonObject{}}};
        };
        QJsonObject group = track("g1", "group", "Drums");
        group["sends"] = QJsonObject{{"r1", QJsonObject{{"level_db", -6.0}, {"pre_fader", false}}}};
        QJsonObject kickTrack = track("t1", "audio", "Kick");
        kickTrack["volume_db"] = -2.5;
        kickTrack["pan"] = -0.25;
        kickTrack["height"] = 120;
        kickTrack["devices"] = QJsonArray{utility("d1", -1.0), rack, sampler};
        kickTrack["clips"] = QJsonArray{
            QJsonObject{{"id", "k1"}, {"name", "kick"}, {"path", kick}, {"relative_path", "audio/kick.wav"},
                        {"start_beat", 0.0}, {"duration_sec", 1.5}, {"offset_sec", 0.0}, {"source_duration_sec", 2.0},
                        {"gain_db", -1.0}, {"warp", true}, {"warp_mode", "Transients"}, {"segment_bpm", 128.0},
                        {"transpose", -3}, {"detune", 12.5}, {"pan", 0.25}},
            QJsonObject{{"id", "k2"}, {"name", "kick"}, {"path", reversed}, {"relative_path", "Reversed/kick R.wav"},
                        {"start_beat", 4.0}, {"duration_sec", 0.5}, {"offset_sec", 1.0}, {"source_duration_sec", 2.0},
                        {"gain_db", 0.0}, {"warp", false}, {"warp_mode", "Standard"}, {"segment_bpm", 0.0},
                        {"transpose", 0}, {"detune", 0.0}, {"pan", 0.0}, {"reversed_from", kick},
                        {"reversed_from_relative", "audio/kick.wav"}}};
        kickTrack["automation"] = QJsonObject{
            {"device:d1:gain", QJsonArray{QJsonArray{0.0, 0.5, 0.0}, QJsonArray{2.0, 0.25, 0.5}}},
            {"send:r1", QJsonArray{QJsonArray{1.0, 0.3, 0.0}}},
            {"device:rk:chain:ch1:volume", QJsonArray{QJsonArray{0.0, 0.8, -1.0}}}};
        kickTrack["automation_view"] = view(true, "device:d1:gain", QJsonArray{"send:r1"});
        kickTrack["input"] = QJsonArray{0, 1};
        kickTrack["monitor"] = "in";
        kickTrack["armed"] = true;
        kickTrack["parent"] = "g1";
        kickTrack["folded"] = true;
        kickTrack["sends"] = QJsonObject{{"r1", QJsonObject{{"level_db", -12.0}, {"pre_fader", true}}}};
        QJsonObject keys = track("t2", "midi", "Keys");
        keys["devices"] = QJsonArray{missingState};
        keys["clips"] = QJsonArray{QJsonObject{
            {"id", "m1"}, {"name", "Keys"}, {"start_beat", 8.0}, {"duration_beats", 4.0}, {"offset_beats", 1.0},
            {"notes", QJsonArray{QJsonArray{60, 0.0, 1.0, 100}, QJsonArray{64, 1.5, 0.25, 90}}}}};
        keys["midi_input"] = QJsonObject{{"device", "Keys"}, {"channel", 3}};
        keys["frozen"] = QJsonObject{{"path", dir.path("Freeze/t2.wav")}, {"relative_path", "Freeze/t2.wav"},
                                     {"duration_sec", 4.0}, {"tempo", 128.0}};
        QJsonObject resampling = track("t3", "audio", "Resampled");
        resampling["input_track"] = "master";
        resampling["devices"] = QJsonArray{tapping};
        QJsonObject noInput = track("t4", "midi", "Silent");
        noInput["midi_input"] = QJsonValue::Null;
        QJsonObject returnTrack{{"id", "r1"},         {"kind", "return"}, {"name", "A Return"}, {"color", "#5480e4"},
                                {"volume_db", -1.0},  {"pan", 0.0},       {"mute", false},      {"solo", false},
                                {"height", 80},       {"devices", QJsonArray{plugin}},
                                {"automation", QJsonObject{{"mixer:pan", QJsonArray{QJsonArray{0.0, 0.25, 0.0}}}}},
                                {"automation_view", view()}, {"sends", QJsonObject{}}};
        const QJsonObject data{
            {"format", "gilstudio-project"},
            {"version", 15},
            {"tempo", 128.0},
            {"key", "F#m"},
            {"time_signature", QJsonArray{7, 8}},
            {"loop", QJsonObject{{"enabled", true}, {"start", 4.0}, {"end", 12.0}}},
            {"automation_locked", true},
            {"master",
             QJsonObject{{"volume_db", -1.5},
                         {"pan", 0.0},
                         {"devices", QJsonArray{compressor}},
                         {"automation", QJsonObject{{"mixer:volume", QJsonArray{QJsonArray{0.0, 0.7, 0.0},
                                                                                QJsonArray{4.0, 0.5, -0.25}}}}},
                         {"automation_view", view(true, "mixer:volume")}}},
            {"folded_devices", QJsonArray{"d1", "u2"}},
            {"tracks", QJsonArray{group, kickTrack, keys, resampling, noInput}},
            {"returns", QJsonArray{returnTrack}}};

        Project project;
        loadInto(project, data, projectFile);
        QCOMPARE(project.tempo(), 128.0);
        QVERIFY((project.key() == Key{6, true}));
        QVERIFY((project.timeSignature() == TimeSignature{7, 8}));
        QVERIFY(project.automationLocked() && project.loopEnabled());
        QCOMPARE(project.tracks().size(), size_t(5));
        const Track& loadedKick = project.track("t1");
        QCOMPARE(loadedKick.parent, std::optional<QString>("g1"));
        QCOMPARE(loadedKick.devices.size(), size_t(3));
        const Device& loadedRack = loadedKick.devices[1];
        QVERIFY(loadedRack.isRack() && loadedRack.name == "Glue" && loadedRack.chains.size() == 2);
        QVERIFY((loadedRack.macros == std::vector<MacroMapping>{{0, "u2", "gain", 0.25, 0.75}}));
        QVERIFY(loadedRack.chains[0].solo && loadedRack.chains[0].volumeDb == -3.0);
        QCOMPARE(loadedKick.devices[2].state, std::optional<QString>("c2FtcGxlPS90bXAveC53YXYK"));
        QVERIFY(!loadedKick.devices[2].enabled);
        QCOMPARE(loadedKick.clips[1].reversedFrom, kick);
        QVERIFY((loadedKick.sends == SendMap{{"r1", Send{-12.0, true}}}));
        QVERIFY(project.track("t2").frozen.has_value());
        QVERIFY((project.track("t2").midiInput == MidiInput{"Keys", 3}));
        QVERIFY(!project.track("t4").midiInput);
        QCOMPARE(project.track("t3").inputTrack, std::optional<QString>(kMaster));
        QVERIFY((project.master().devices[0].sidechain == Sidechain{"t1", kPostFader}));
        QVERIFY((project.track("t3").devices[0].sidechain == Sidechain{"t1", "d1"}));
        QCOMPARE(project.track("t3").devices[0].sidechain->tapDevice(), std::optional<QString>("d1"));
        QCOMPARE(project.returns()[0].devices[0].plugin->uid, QStringLiteral("ABCDEF0123456789"));
        QVERIFY(project.isDeviceFolded("u2") && project.isDeviceFolded("d1"));

        QCOMPARE(projectToJson(project, projectFile), data);
    }

    // --- What an edited file can't have ---

    void groupsThatDontHoldTogetherLoadOutOfThem() {
        Project project;
        ProjectContents contents;
        contents.tracks = {test::makeTrack("g1", "G", kGroupKind), test::makeTrack("a", "A", kAudioKind, "g1"),
                           test::makeTrack("g2", "Inner", kGroupKind, "g1"), test::makeTrack("b", "B", kAudioKind, "g2"),
                           test::makeTrack("c", "C")};
        contents.tracks[2].folded = true;
        project.replaceContents(contents);
        QJsonObject data = projectToJson(project);
        Project loaded;
        loadInto(loaded, data);
        QCOMPARE(loaded.tree(), project.tree());
        QVERIFY(loaded.track("g2").folded && loaded.track("g1").isGroup());
        // A file whose groups don't hold together (edited by hand) loads with the strays out of them.
        QJsonArray tracks = data["tracks"].toArray();
        QJsonObject stray = tracks.takeAt(tracks.size() - 1).toObject();  // C, put first and in the group after it
        stray["parent"] = "g1";
        tracks.prepend(stray);
        data["tracks"] = tracks;
        loadInto(loaded, data);
        QCOMPARE(loaded.tracks()[0].id, QStringLiteral("c"));
        QVERIFY(!loaded.tracks()[0].parent);
        QVERIFY(!treeProblem(loaded.tracks()));
        // Groups aren't armed.
        loadInto(loaded, setIn(data, {"tracks", 1, "armed"}, true));
        QVERIFY(!loaded.track("g1").armed);
    }

    void sendsAProjectCantHaveAreDropped() {
        Track track = test::makeTrack("t", "T");
        track.sends = {{"a", Send{-6.0, true}}};
        Track group = test::makeTrack("g", "G", kGroupKind);
        group.sends = {{"b", Send{-3.0, false}}};
        track.parent = "g";
        Track a = test::makeTrack("a", "A Return", kReturnKind);
        a.sends = {{"b", Send{-1.5, false}}};
        Track b = test::makeTrack("b", "B Return", kReturnKind);
        b.volumeDb = -2.0;
        ProjectContents contents;
        contents.tracks = {group, track};
        contents.returns = {a, b};
        Project project;
        project.replaceContents(contents);
        QJsonObject data = projectToJson(project);
        Project loaded;
        loadInto(loaded, data);
        QCOMPARE(loaded.returns().size(), size_t(2));
        QVERIFY((loaded.track("t").sends == SendMap{{"a", Send{-6.0, true}}}));
        QVERIFY((loaded.track("g").sends == SendMap{{"b", Send{-3.0, false}}}));
        QVERIFY((loaded.track("a").sends == SendMap{{"b", Send{-1.5, false}}}));
        QCOMPARE(loaded.track("b").volumeDb, -2.0);
        QCOMPARE(loaded.returnLetter("b"), QStringLiteral("B"));
        // Sends a project can't have (the file was edited) are dropped: to a return
        // that isn't there, and the one closing a cycle.
        data = setIn(data, {"returns", 1, "sends"}, QJsonObject{{"a", QJsonObject{{"level_db", 0.0}, {"pre_fader", false}}}});
        data = setIn(data, {"tracks", 1, "sends", "gone"}, QJsonObject{{"level_db", 0.0}});
        data = setIn(data, {"tracks", 1, "sends", "a", "level_db"}, 20.0);  // (clamped to the fader's range)
        loadInto(loaded, data);
        QVERIFY(loaded.track("b").sends.isEmpty());
        QVERIFY((loaded.track("t").sends == SendMap{{"a", Send{6.0, true}}}));
        // Older files have neither.
        data.remove("returns");
        QJsonArray tracks = data["tracks"].toArray();
        for (qsizetype i = 0; i < tracks.size(); ++i) {
            QJsonObject t = tracks[i].toObject();
            t.remove("sends");
            tracks[i] = t;
        }
        data["tracks"] = tracks;
        loadInto(loaded, data);
        QVERIFY(loaded.returns().empty());
        for (const Track& t : loaded.tracks()) QVERIFY(t.sends.isEmpty());
    }

    void inputsAProjectCantHaveAreDropped() {
        ProjectContents contents;
        contents.tracks = {test::makeTrack("a", "A"), test::makeTrack("b", "B"), test::makeTrack("c", "C")};
        contents.returns = {test::makeTrack("r", "R", kReturnKind)};
        contents.tracks[0].inputTrack = "c";  // a track listed after it
        contents.tracks[1].inputTrack = kMaster;
        contents.tracks[2].inputTrack = "r";
        Project project;
        project.replaceContents(contents);
        QJsonObject data = projectToJson(project);
        QCOMPARE(data["tracks"].toArray()[0].toObject()["input_track"].toString(), QStringLiteral("c"));
        Project loaded;
        loadInto(loaded, data);
        QCOMPARE(loaded.tracks()[0].inputTrack, std::optional<QString>("c"));
        QCOMPARE(loaded.tracks()[1].inputTrack, std::optional<QString>(kMaster));
        QCOMPARE(loaded.tracks()[2].inputTrack, std::optional<QString>("r"));
        // Edited files: an input from a track that isn't there, or one closing a cycle, is dropped.
        data = setIn(data, {"tracks", 0, "input_track"}, "gone");
        data = setIn(data, {"tracks", 2, "input_track"}, "b");
        data = setIn(data, {"tracks", 1, "input_track"}, "c");  // b <- c <- b: the later one goes
        data = setIn(data, {"tracks", 1, "input"}, QJsonArray{0, 1});
        loadInto(loaded, data);
        QVERIFY(!loaded.tracks()[0].inputTrack);
        QCOMPARE(loaded.tracks()[1].inputTrack, std::optional<QString>("c"));
        QVERIFY(!loaded.tracks()[2].inputTrack);
        QVERIFY(loaded.tracks()[1].input.empty());
        QJsonObject second = data["tracks"].toArray()[1].toObject();
        second.remove("input_track");  // (older files have none)
        QJsonArray tracks = data["tracks"].toArray();
        tracks[1] = second;
        data["tracks"] = tracks;
        loadInto(loaded, data);
        QVERIFY((loaded.tracks()[1].input == std::vector<int>{0, 1}));
    }

    void sidechainsAProjectCantHaveAreDropped() {
        auto device = [](const QString& id, const std::optional<Sidechain>& sidechain = std::nullopt) {
            Device d = test::makeDevice(id, "compressor");
            d.sidechain = sidechain;
            return d;
        };
        ProjectContents contents;
        contents.tracks = {test::makeTrack("a", "A"), test::makeTrack("b", "B"), test::makeTrack("c", "C")};
        contents.returns = {test::makeTrack("r", "R", kReturnKind)};
        contents.tracks[2].devices = {device("tapped")};
        contents.tracks[0].devices = {device("onA", Sidechain{"c", "tapped"})};  // a track listed after it
        contents.tracks[1].devices = {device("onB", Sidechain{"r", kPreFader})};
        contents.returns[0].devices = {device("onR", Sidechain{"a"})};
        Track master = newMaster();
        master.devices = {device("onM", Sidechain{"r"})};
        contents.master = master;
        Project project;
        project.replaceContents(contents);
        QJsonObject data = projectToJson(project);
        QCOMPARE(data["tracks"].toArray()[0].toObject()["devices"].toArray()[0].toObject()["sidechain"].toObject(),
                 (QJsonObject{{"track", "c"}, {"tap", "tapped"}}));
        Project loaded;
        loadInto(loaded, data);
        auto found = [&](const QString& owner) { return loaded.track(owner).devices.back().sidechain; };
        QVERIFY((found("a") == Sidechain{"c", "tapped"}) && (found("b") == Sidechain{"r", kPreFader}) &&
                (found("r") == Sidechain{"a"}) && (found(kMaster) == Sidechain{"r"}));
        // Edited files: a sidechain from a track that isn't there, the master, or one closing a cycle, is dropped.
        data = setIn(data, {"tracks", 0, "devices", 0, "sidechain", "track"}, "gone");
        data = setIn(data, {"master", "devices", 0, "sidechain", "track"}, kMaster);
        data = setIn(data, {"returns", 0, "devices", 0, "sidechain", "track"}, "b");  // b <- r <- b: the later one goes
        loadInto(loaded, data);
        QVERIFY(!found("a") && (found("b") == Sidechain{"r", kPreFader}) && !found("r") && !found(kMaster));
    }

    void racksAreSavedAndLoaded() {
        Device a = test::makeDevice("a", "utility", {{"gain", 0.0}});
        Device b = test::makeDevice("b", "utility", {{"pan", 0.0}});
        a.sidechain = Sidechain{"source"};
        Device rack;
        rack.id = "rack";
        rack.kind = kRackKind;
        for (int i = 0; i < kMacroCount; ++i) rack.params.insert(macroParam(i), 0.0);
        rack.chains = {Chain{"c1", "Chain 1", {a}, 0.0, 0.0, false, false},
                       Chain{"wet", "Wet", {b}, -3.0, 0.0, false, true}};
        rack.macros = {MacroMapping{2, "b", "pan", 0.25, 0.75}};
        Track track = test::makeTrack("t", "T");
        track.devices = {rack};
        track.automation.insert(automation::chainKey("rack", "wet", automation::kChainPan), {{1.0, 0.25}});
        ProjectContents contents;
        contents.tracks = {track, test::makeTrack("source", "Source")};
        Project project;
        project.replaceContents(contents);
        QJsonObject data = projectToJson(project);
        Project loaded;
        loadInto(loaded, data);
        QCOMPARE(projectToJson(loaded), data);
        const Device& again = loaded.device("t", "rack");
        QVERIFY(again.chains[1].volumeDb == -3.0 && again.chains[1].solo && again.chains[1].name == "Wet");
        QVERIFY((again.macros == std::vector<MacroMapping>{{2, "b", "pan", 0.25, 0.75}}));
        QVERIFY((loaded.device("t", "a").sidechain == Sidechain{"source"}));
        QVERIFY(!again.name);  // (none before version 13)
        // A mapping to a device not in its rack (an edited file) goes; so does a sidechain closing a cycle.
        QJsonArray mappings = data["tracks"].toArray()[0].toObject()["devices"].toArray()[0].toObject()["macros"].toArray();
        mappings.append(QJsonObject{{"macro", 0}, {"device", "elsewhere"}, {"param", "gain"}});
        mappings.append(QJsonObject{{"macro", 9}, {"device", "a"}, {"param", "gain"}});  // (no such macro)
        data = setIn(data, {"tracks", 0, "devices", 0, "macros"}, mappings);
        data = setIn(data, {"tracks", 0, "devices", 0, "chains", 0, "devices", 0, "sidechain"},
                     QJsonObject{{"track", "t"}, {"tap", "post"}});
        loadInto(loaded, data);
        QCOMPARE(loaded.device("t", "rack").macros.size(), size_t(1));
        QVERIFY(!loaded.device("t", "a").sidechain);
    }

    void frozenTracksAreSaved() {
        test::TempDir dir;
        const QString projectFile = dir.path("x.gilproj");
        const Freeze frozen{dir.path("Freeze/a.wav"), 3.5, 98.0};
        const Freeze other{dir.path("Freeze/r.wav"), 2.0, 120.0};
        ProjectContents contents;
        contents.tracks = {test::makeTrack("a", "A")};
        contents.tracks[0].frozen = frozen;
        contents.returns = {test::makeTrack("r", "R", kReturnKind)};
        contents.returns[0].frozen = other;
        Project project;
        project.replaceContents(contents);
        QJsonObject data = projectToJson(project, projectFile);
        QCOMPARE(data["tracks"].toArray()[0].toObject()["frozen"].toObject()["relative_path"].toString(),
                 QStringLiteral("Freeze/a.wav"));
        Project loaded;
        loadInto(loaded, data, projectFile);
        QVERIFY(loaded.track("a").frozen == frozen && loaded.track("r").frozen == other);
        // An incomplete one loads unfrozen.
        data = setIn(data, {"tracks", 0, "frozen", "tempo"}, QJsonValue::Null);
        loadInto(loaded, data, projectFile);
        QVERIFY(!loaded.track("a").frozen);
    }

    void onlyFoldedDevicesThatExistAreSaved() {
        ProjectContents contents;
        contents.tracks = {test::makeTrack("t", "T")};
        contents.tracks[0].devices = {test::makeDevice("d", "utility")};
        contents.foldedDevices = {"d", "gone"};
        Project project;
        project.replaceContents(contents);
        QCOMPARE(projectToJson(project)["folded_devices"].toArray(), QJsonArray{"d"});
    }
};

QTEST_GUILESS_MAIN(TestSerialization)
#include "test_serialization.moc"
