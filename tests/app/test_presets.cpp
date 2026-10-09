// Device presets without the editor or the engine bridge: the user's library
// (saving by name, listing by device, renaming), default presets, presets as
// new devices, and rack names.

#include "TestSupport.h"

#include "io/Presets.h"
#include "io/Serialization.h"
#include "model/Devices.h"
#include "model/Errors.h"
#include "model/Project.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>

using namespace sub::app;

namespace {

Device utility(const QString& id, double gain = 0.0) {
    Device device = newDevice("utility");
    device.id = id;
    device.params.insert("gain", gain);
    return device;
}

Device rackOf(std::vector<Device> devices) {
    std::vector<Chain> chains{newChain(devices.empty() ? QStringLiteral("Chain") : deviceName(devices[0]), devices)};
    return newRack(std::move(chains));
}

QString presetText(const QString& path) {
    QFile file(path);
    file.open(QIODevice::ReadOnly);
    return QString::fromUtf8(file.readAll());
}

// The device as a preset would load (a new device: fresh ids).
Device presetOf(const Device& device) { return presetDevice(deviceToPreset(device)); }

}  // namespace

class TestPresets : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { test::prepareApplication(); }

    void theLibraryIsWhereTheEnvironmentSays() {
        QCOMPARE(libraryDir(), qEnvironmentVariable("SUBSTATION_PRESETS"));
        const QByteArray before = qgetenv("SUBSTATION_PRESETS");
        qunsetenv("SUBSTATION_PRESETS");
        QCOMPARE(libraryDir(), QDir::homePath() + "/Documents/SUBstation/Presets");
        qputenv("SUBSTATION_PRESETS", before);
    }

    void presetsAreSavedByDeviceAndListedByGroupThenName() {
        test::TempDir dir;
        const QString root = dir.path();
        Device gain = utility("u1");
        Device rack = rackOf({newDevice("compressor")});
        const QString quiet = saveToLibrary(gain, "quiet", root);
        const QString loud = saveToLibrary(gain, "Loud", root);
        const QString glue = saveToLibrary(rack, "Glue", root);
        QCOMPARE(quiet, root + "/Utility/quiet" + kPresetExtension);
        QCOMPARE(glue, root + "/Audio Effect Rack/Glue" + kPresetExtension);
        QFile::copy(quiet, root + "/Loose" + kPresetExtension);  // put there by hand
        QFile notes(root + "/Utility/notes.txt");
        notes.open(QIODevice::WriteOnly);
        notes.write("not a preset");
        notes.close();
        std::vector<std::pair<QString, QString>> listed;
        for (const PresetFile& p : listPresets(root)) listed.emplace_back(p.group, p.name);
        QVERIFY((listed == std::vector<std::pair<QString, QString>>{
                               {"", "Loose"}, {"Audio Effect Rack", "Glue"}, {"Utility", "Loud"}, {"Utility", "quiet"}}));
        QVERIFY(listPresets(root + "/nothing here").empty());
        // Saving under a name used replaces that preset.
        gain.params.insert("gain", -12.0);
        saveToLibrary(gain, "Loud", root);
        QCOMPARE(loadPreset(loud).params.value("gain"), -12.0);
        QCOMPARE(listPresets(root).size(), size_t(4));
        // The default library (the tests' temporary one) works the same.
        const QString inLibrary = saveToLibrary(gain, "Default Place");
        QVERIFY(inLibrary.startsWith(libraryDir()));
        QCOMPARE(listPresets().size(), size_t(1));
    }

    void presetNamesBecomeFileNames() {
        test::TempDir dir;
        QCOMPARE(presetFileName("  Lead: \"Bright\" / Wide?  "), QStringLiteral("Lead_ _Bright_ _ Wide_"));
        for (const char* bad : {"", "   ", "...", "CON", "nul.txt", "com1"}) {
            QVERIFY_THROWS_EXCEPTION(EditError, presetFileName(QString::fromLatin1(bad)));
        }
        QCOMPARE(presetPath(test::makeDevice("d", "utility"), "a/b", dir.path()),
                 dir.path() + "/Utility/a_b" + kPresetExtension);
        // A device called as the defaults' folder groups apart from it.
        Device rack = rackOf({});
        rack.name = "Defaults";
        QCOMPARE(groupOf(rack), QStringLiteral("Audio Effect Rack"));
        PluginRef plugin{"VST3", "X", "Defaults"};
        Device fx = newDevice(kPluginKind, plugin);
        QCOMPARE(groupOf(fx), QStringLiteral("Defaults (Device)"));
    }

    void renamingAPreset() {
        test::TempDir dir;
        const Device gain = utility("d", -3.0);
        const QString first = saveToLibrary(gain, "One", dir.path());
        const QString second = saveToLibrary(gain, "Two", dir.path());
        try {
            renamePreset(first, "Two");
            QFAIL("renamed over another preset");
        } catch (const EditError& error) {
            QCOMPARE(error.message(), QStringLiteral("There is a preset called Two already"));
        }
        const QString renamed = renamePreset(first, "Three");
        QCOMPARE(renamed, dir.path() + "/Utility/Three" + kPresetExtension);
        QVERIFY(!QFileInfo::exists(first));
        QCOMPARE(QFileInfo(renamePreset(second, "two")).fileName(), "two" + kPresetExtension);  // a change of case is no clash
        QCOMPARE(renamePreset(renamed, "Three"), renamed);  // the same name: nothing to do
        QStringList names;
        for (const PresetFile& p : listPresets(dir.path())) names.append(p.name);
        QCOMPARE(names, (QStringList{"Three", "two"}));
    }

    // --- Presets as devices ---

    void presetsAreTheDeviceAsTheProjectStoresItWithoutSidechains() {
        Device inner = utility("u", -6.0);
        inner.sidechain = Sidechain{"somewhere"};
        Device rack = rackOf({inner});
        rack.macros = {MacroMapping{0, "u", "gain", 0.0, 1.0}};
        rack.sidechain = Sidechain{"elsewhere"};
        const QJsonObject preset = deviceToPreset(rack);
        QCOMPARE(preset["format"].toString(), kPresetFormat);
        QCOMPARE(preset["version"].toInt(), kPresetVersion);
        QJsonObject expected = deviceToJson(rack);
        expected.remove("sidechain");
        QJsonArray chains = expected["chains"].toArray();
        QJsonObject chain = chains[0].toObject();
        QJsonArray devices = chain["devices"].toArray();
        QJsonObject stripped = devices[0].toObject();
        stripped.remove("sidechain");
        devices[0] = stripped;
        chain["devices"] = devices;
        chains[0] = chain;
        expected["chains"] = chains;
        QCOMPARE(preset["device"].toObject(), expected);

        // Loaded, it is a new device: fresh ids for it and everything in it, its
        // macro mappings following them.
        const Device loaded = presetDevice(preset);
        const Device again = presetDevice(preset);
        QVERIFY(loaded.id != rack.id && loaded.id != again.id);
        QVERIFY(loaded.chains[0].id != rack.chains[0].id);
        const QString innerId = loaded.chains[0].devices[0].id;
        QVERIFY(innerId != "u");
        QVERIFY((loaded.macros == std::vector<MacroMapping>{{0, innerId, "gain", 0.0, 1.0}}));
        QVERIFY(!loaded.sidechain && !loaded.chains[0].devices[0].sidechain);
        QCOMPARE(loaded.chains[0].devices[0].params, inner.params);
    }

    void damagedPresetsAreProjectFileErrors() {
        const Device rack = rackOf({utility("a"), utility("b")});
        const QJsonObject good = deviceToPreset(rack);
        const QJsonObject damages[] = {
            QJsonObject{{"version", "x"}},
            QJsonObject{{"device", QJsonObject{{"kind", "utility"}, {"params", QJsonArray{}}}}},
            QJsonObject{{"device", QJsonObject{{"kind", "rack"}, {"macros", QJsonArray{"x"}}, {"chains", QJsonArray{}}}}},
        };
        for (const QJsonObject& damage : damages) {
            QJsonObject data = good;
            for (auto it = damage.begin(); it != damage.end(); ++it) data[it.key()] = it.value();
            try {
                presetDevice(data);
                QFAIL("loaded a damaged preset");
            } catch (const ProjectFileError& error) {
                QVERIFY2(error.message().startsWith("The preset is damaged: "), qPrintable(error.message()));
            }
        }
        QVERIFY_THROWS_EXCEPTION(ProjectFileError, presetDevice(QJsonObject{{"format", "gilstudio-project"}}));
        QJsonObject newer = good;
        newer["version"] = kPresetVersion + 1;
        QVERIFY_THROWS_EXCEPTION(ProjectFileError, presetDevice(newer));
    }

    void presetsNestingTooDeepAreRefused() {
        Device device = utility("u");
        for (int i = 0; i < kMaxRackDepth; ++i) device = rackOf({device});
        QCOMPARE(rackHeight(device), kMaxRackDepth);
        presetDevice(deviceToPreset(device));  // as deep as can be: fine
        const Device wrapper = rackOf({device});  // one more rack around the deepest allowed
        try {
            presetDevice(deviceToPreset(wrapper));
            QFAIL("loaded racks nested too deep");
        } catch (const ProjectFileError& error) {
            QVERIFY(error.message().contains("too deep"));
        }
    }

    void aRackIsNamedAsThePresetItComesFrom() {
        test::TempDir dir;
        Device rack = rackOf({utility("u")});
        QCOMPARE(deviceName(rack), QStringLiteral("Audio Effect Rack"));
        QString path = saveToLibrary(rack, "Glue", dir.path());
        QCOMPARE(QFileInfo(path).dir().dirName(), QStringLiteral("Audio Effect Rack"));  // (grouped by kind, whatever its name)
        const Device loaded = loadPreset(path);
        QCOMPARE(loaded.name, std::optional<QString>("Glue"));
        QCOMPARE(deviceName(loaded), QStringLiteral("Glue"));
        path = renamePreset(path, "Bus Glue");
        QCOMPARE(loadPreset(path).name, std::optional<QString>("Bus Glue"));  // (the file's name, as renamed)
        // Saved as a preset, a rack's name is kept in the file.
        Device named = rack;
        named.name = "Mine";
        QCOMPARE(saveToLibrary(named, "Other", dir.path()), dir.path() + "/Audio Effect Rack/Other" + kPresetExtension);
        QCOMPARE(deviceToPreset(named)["device"].toObject()["name"].toString(), QStringLiteral("Mine"));
        QVERIFY(!deviceToPreset(rack)["device"].toObject().contains("name"));
        // A built-in device's name is its kind's.
        QVERIFY(!loadPreset(saveToLibrary(utility("x"), "Plain", dir.path())).name);
    }

    void unreadablePresetFilesAreProjectFileErrors() {
        test::TempDir dir;
        QFile file(dir.path("bad.gilpreset"));
        file.open(QIODevice::WriteOnly);
        file.write("not a preset");
        file.close();
        try {
            loadPreset(dir.path("bad.gilpreset"));
            QFAIL("read a file that isn't a preset");
        } catch (const ProjectFileError& error) {
            QVERIFY(error.message().startsWith("Could not read bad.gilpreset"));
        }
    }

    // --- Default presets ---

    void newDevicesStartAsTheirDefaultPreset() {
        test::TempDir dir;
        const QString root = dir.path();
        Device factory = utility("f", -4.0);
        const QString path = saveDefault(factory, root);
        QCOMPARE(path, root + "/" + kDefaultsFolder + "/Utility" + kPresetExtension);
        QVERIFY(hasDefault("utility", std::nullopt, root));
        QVERIFY(listPresets(root).empty());  // (the browser doesn't list defaults)
        const auto first = defaultDevice("utility", std::nullopt, root);
        const auto second = defaultDevice("utility", std::nullopt, root);
        QVERIFY(first && second);
        QCOMPARE(first->params, factory.params);
        QVERIFY(first->id != second->id && first->id != factory.id);
        QVERIFY(!defaultDevice("compressor", std::nullopt, root));  // (no default)
        // Parameters a default was saved without stay at theirs.
        Device partial = test::makeDevice("p", "compressor", {{"threshold", -30.0}});
        saveDefault(partial, root);
        const auto compressor = defaultDevice("compressor", std::nullopt, root);
        QVERIFY(compressor);
        QMap<QString, double> expected = newDevice("compressor").params;
        expected.insert("threshold", -30.0);
        QCOMPARE(compressor->params, expected);

        QVERIFY(clearDefault("utility", std::nullopt, root));
        QVERIFY(!clearDefault("utility", std::nullopt, root));
        QVERIFY(!defaultDevice("utility", std::nullopt, root));
    }

    void aDefaultPresetIsPerPlugInAndKeepsWhereThePlugInIs() {
        test::TempDir dir;
        const PluginRef a{"VST3", "AAAA", "Same Name", "", "C:/old/A.vst3"};
        const PluginRef b{"VST3", "BBBB", "Same Name"};
        Device withState = newDevice(kPluginKind, a);
        withState.state = "c3RhdGU=";
        saveDefault(withState, dir.path());
        QCOMPARE(QFileInfo(*defaultPath(kPluginKind, a, dir.path())).fileName(), "Same Name (AAAA)" + kPresetExtension);
        const PluginRef moved{"VST3", "AAAA", "Same Name", "", "D:/new/A.vst3"};
        const auto device = defaultDevice(kPluginKind, moved, dir.path());
        QVERIFY(device);
        QCOMPARE(device->state, std::optional<QString>("c3RhdGU="));
        QVERIFY(device->plugin == moved);
        QVERIFY(!defaultDevice(kPluginKind, b, dir.path()));
        QVERIFY(!defaultPath(kRackKind, std::nullopt, dir.path()));
        QVERIFY(!defaultPath(kPluginKind, std::nullopt, dir.path()));
        QVERIFY_THROWS_EXCEPTION(EditError, saveDefault(test::makeDevice("r", kRackKind), dir.path()));
    }

    void anUnreadableDefaultPresetIsIgnored() {
        test::TempDir dir;
        const QString path = *defaultPath("utility", std::nullopt, dir.path());
        QDir().mkpath(QFileInfo(path).path());
        QFile file(path);
        file.open(QIODevice::WriteOnly);
        file.write("not a preset");
        file.close();
        QVERIFY(!defaultDevice("utility", std::nullopt, dir.path()));
        saveDefault(test::makeDevice("c", "compressor"), dir.path());
        QFile::remove(path);
        QFile::rename(*defaultPath("compressor", std::nullopt, dir.path()), path);  // a compressor where the utility's goes
        QVERIFY(!defaultDevice("utility", std::nullopt, dir.path()));
    }

    // --- Kinds of devices ---

    void builtInDevicesComeFromTheEngine() {
        QVERIFY(!builtinDevices().empty());
        QVERIFY(builtinDevice(kDefaultInstrument) != nullptr);
        QVERIFY(builtinDevice(kDefaultInstrument)->instrument);
        QVERIFY(isInstrument("synth") && !isInstrument("utility") && !isInstrument("nothing"));
        // Instruments first, then by name.
        const auto categories = builtinCategories();
        QCOMPARE(categories.front().first, QStringLiteral("Instruments"));
        QVERIFY(categories.front().second.contains("synth"));
        const Device synth = newDevice("synth");
        QCOMPARE(synth.params, builtinDevice("synth")->defaults);
        QVERIFY(builtinParamInfo("synth", "cutoff") != nullptr);
        QVERIFY(builtinParamInfo("synth", "nothing") == nullptr);
        QVERIFY_THROWS_EXCEPTION(EditError, newDevice("nothing"));
        QVERIFY_THROWS_EXCEPTION(EditError, newDevice(kPluginKind));
        QCOMPARE(kindName(test::makeDevice("x", "a device of a later version")), QStringLiteral("a device of a later version"));
    }

    void racksAndInstruments() {
        const Device instrumentRack = rackOf({newDevice("synth")});
        const Device effectRack = rackOf({utility("u")});
        QVERIFY(deviceIsInstrument(instrumentRack) && !deviceIsInstrument(effectRack));
        QCOMPARE(kindName(instrumentRack), QStringLiteral("Instrument Rack"));
        QCOMPARE(newRack({}).params.size(), kDefaultMacroCount);  // macro1..macro4, at 0
        QCOMPARE(newRack({}).params.value("macro4", -1.0), 0.0);
        QVERIFY(!newRack({}).params.contains("macro5"));
        QCOMPARE(macroCount(newRack({})), kDefaultMacroCount);
        QVERIFY(loadsInto(effectRack, effectRack) && !loadsInto(instrumentRack, effectRack));
        QVERIFY(!loadsInto(utility("u"), newDevice("compressor")));
        const PluginRef synthRef{"VST3", "A", "Synth A", "", "", true};
        const PluginRef effectRef{"VST3", "B", "Effect B"};
        QVERIFY(loadsInto(newDevice(kPluginKind, effectRef), newDevice(kPluginKind, effectRef)));
        QVERIFY(!loadsInto(newDevice(kPluginKind, effectRef), newDevice(kPluginKind, synthRef)));
        QCOMPARE(deviceIdsOf(effectRack).size(), 2);
        QCOMPARE(deviceIdsOfList({effectRack, utility("v")}).size(), 3);
        QCOMPARE(innerDeviceIds(effectRack), QSet<QString>{QStringLiteral("u")});  // (not the rack's own)
        QCOMPARE(innerDeviceIds(rackOf({effectRack})), deviceIdsOf(effectRack));  // (in racks in it too)
        QVERIFY(innerDeviceIds(utility("v")).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestPresets)
#include "test_presets.moc"
