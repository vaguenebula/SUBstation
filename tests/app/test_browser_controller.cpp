// The browser's logic without its widgets (BrowserController and its models):
// places and their settings, searching as you type, the sort, the sidebar's
// entries and what each lists, the folder tree for a place, using items
// (activation, drops) and their use counts, preview requests, Enter selecting
// the first result, keeping the list's place when the index changes, plug-ins
// and their failures, presets, and what a drag carries.
//
// Plug-ins come from the scan's cache here (files whose signature matches are
// not read again), so no scanner process runs.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

#include <memory>
#include <vector>

#include "browser/BrowserController.h"
#include "browser/BrowserMime.h"
#include "browser/ItemListModel.h"
#include "browser/PathKeys.h"
#include "browser/SidebarModel.h"
#include "plugins/PluginPaths.h"

using namespace sub::app;

namespace {

void touch(const QString& path, const QByteArray& content = {}) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly | QIODevice::Truncate), qPrintable(path));
    file.write(content);
}

QStringList names(const ItemListModel& model) {
    QStringList out;
    for (int row = 0; row < model.rowCount(); ++row) out << model.item(row)->name;
    return out;
}

int rowOf(const ItemListModel& model, const QString& name) {
    for (int row = 0; row < model.rowCount(); ++row)
        if (model.item(row)->name == name) return row;
    return -1;
}

QStringList sidebarTitles(const SidebarModel& sidebar) {
    QStringList out;
    for (const auto& entry : sidebar.entries()) out << entry.title;
    return out;
}

QJsonObject pluginJson(const QString& name, const QString& vendor, const QString& category, bool instrument, const QString& uid) {
    return {{QStringLiteral("uid"), uid},           {QStringLiteral("name"), name},
            {QStringLiteral("vendor"), vendor},     {QStringLiteral("version"), QStringLiteral("1.0")},
            {QStringLiteral("category"), category}, {QStringLiteral("instrument"), instrument}};
}

}  // namespace

class TestBrowserController : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName(QStringLiteral("SUBstation Tests"));
        QCoreApplication::setApplicationName(QStringLiteral("SUBstation Tests"));
    }

    void init() {
        tmp_ = std::make_unique<QTemporaryDir>();
        QVERIFY(tmp_->isValid());
        clearSettings();
        qputenv("SUBSTATION_LIBRARY", path(QStringLiteral("library.json")).toUtf8());
        qputenv("SUBSTATION_BROWSER_INDEX", path(QStringLiteral("index/browser-index.bin")).toUtf8());
        qputenv("SUBSTATION_PLUGIN_CACHE", path(QStringLiteral("vst3-cache.json")).toUtf8());
        qputenv("SUBSTATION_VST3_PATH", path(QStringLiteral("VST3")).toUtf8());
        qputenv("SUBSTATION_SCANNER", path(QStringLiteral("no-scanner")).toUtf8());  // never needed: all in the cache
        lib_ = path(QStringLiteral("lib"));
        touch(lib_ + QStringLiteral("/Drums/Kick.wav"));
        touch(lib_ + QStringLiteral("/Drums/Snare.wav"));
        touch(lib_ + QStringLiteral("/Loops/Kick Loop.wav"));
        touch(lib_ + QStringLiteral("/Loops/Bass Loop.flac"));
        touch(lib_ + QStringLiteral("/readme.txt"));
    }

    void cleanup() {
        clearSettings();
        tmp_.reset();
    }

    void theFirstStartHasMusicOrHome() {
        const QString home = QDir::homePath();
        const QString music = QDir(home).filePath(QStringLiteral("Music"));
        QCOMPARE(BrowserController::defaultPlaces(), QStringList{normalPath(QFileInfo(music).isDir() ? music : home)});
#ifndef _WIN32
        const QByteArray oldHome = qgetenv("HOME");
        qputenv("HOME", path(QStringLiteral("home")).toUtf8());
        QDir().mkpath(path(QStringLiteral("home/Music")));
        {
            BrowserController browser(nullptr, options());
            QCOMPARE(browser.places(), QStringList{path(QStringLiteral("home/Music"))});
            QCOMPARE(browser.scope(), QStringList{QStringLiteral("samples")});
            QCOMPARE(browser.sort(), QStringLiteral("rank"));
            QStringList expected{QStringLiteral("CATEGORIES"), QStringLiteral("All"), QStringLiteral("Samples"),
                                 QStringLiteral("Built-in")};
            expected << builtinCategories();
            expected << QStringLiteral("Plug-ins") << QStringLiteral("Instruments") << QStringLiteral("Audio Effects")
                     << QStringLiteral("Presets") << QStringLiteral("PLACES") << QStringLiteral("Music")
                     << QStringLiteral("Add Folder…");
            QCOMPARE(sidebarTitles(*browser.sidebar()), expected);
        }
        qputenv("HOME", oldHome);
#endif
    }

    void samplesAreListedAndSearched() {
        auto browser = make();
        QVERIFY(settle(*browser));
        ItemListModel& results = *browser->results();
        QCOMPARE(names(results), QStringList({QStringLiteral("Bass Loop.flac"), QStringLiteral("Kick Loop.wav"),
                                              QStringLiteral("Kick.wav"), QStringLiteral("Snare.wav")}));
        QCOMPARE(browser->statusText(), QStringLiteral("4 items"));
        QCOMPARE(browser->fileCount(), 4);
        const BrowserItem* kick = results.item(rowOf(results, QStringLiteral("Kick.wav")));
        QCOMPARE(kick->path, lib_ + QStringLiteral("/Drums/Kick.wav"));
        QCOMPARE(kick->detail, QStringLiteral("Drums"));
        QCOMPARE(kick->kind, ItemKind::Audio);

        // Searching as you type: the name that is the search first, then names that start with it.
        QSignalSpy shown(browser.get(), &BrowserController::resultsShown);
        browser->setSearchText(QStringLiteral("kick"));
        QVERIFY(browser->searching());
        QVERIFY(settle(*browser));
        QCOMPARE(names(results), QStringList({QStringLiteral("Kick.wav"), QStringLiteral("Kick Loop.wav")}));
        QCOMPARE(browser->statusText(), QStringLiteral("2 items"));
        QVERIFY(shown.count() >= 1);
        // A detail (the folder) matches too.
        browser->setSearchText(QStringLiteral("drums"));
        QVERIFY(settle(*browser));
        QCOMPARE(names(results), QStringList({QStringLiteral("Kick.wav"), QStringLiteral("Snare.wav")}));

        // The sort is kept.
        browser->setSearchText(QStringLiteral("kick"));
        browser->setSort(QStringLiteral("name"));
        QVERIFY(settle(*browser));
        QCOMPARE(names(results), QStringList({QStringLiteral("Kick Loop.wav"), QStringLiteral("Kick.wav")}));
        QCOMPARE(QSettings().value(QStringLiteral("browser/sort")).toString(), QStringLiteral("name"));
        browser->setSort(QStringLiteral("size"));  // not a sort
        QCOMPARE(browser->sort(), QStringLiteral("name"));
        browser.reset();
        auto again = make();
        QCOMPARE(again->sort(), QStringLiteral("name"));
    }

    void placesAreAddedAndRemoved() {
        auto browser = make();
        QVERIFY(settle(*browser));
        const QString drums = lib_ + QStringLiteral("/Drums");
        QSignalSpy placesChanged(browser.get(), &BrowserController::placesChanged);
        browser->addPlace(drums + QLatin1Char('/'));
        QCOMPARE(browser->places(), QStringList({lib_, drums}));
        QCOMPARE(placesChanged.count(), 1);
        QCOMPARE(QSettings().value(QStringLiteral("browser/places")).toStringList(), QStringList({lib_, drums}));
        // It is shown: with no search text, as its folder tree.
        QCOMPARE(browser->scope(), QStringList({QStringLiteral("place"), drums}));
        QVERIFY(browser->showingTree());
        QCOMPARE(browser->treeRoot(), drums);
        QCOMPARE(browser->statusText(), QDir::toNativeSeparators(drums));
        const int row = browser->sidebar()->find(QStringList{QStringLiteral("place"), drums});
        QVERIFY(row >= 0);
        QCOMPARE(browser->sidebar()->entries()[size_t(row)].title, QStringLiteral("Drums"));
        QCOMPARE(browser->sidebar()->entries()[size_t(row)].toolTip, QDir::toNativeSeparators(drums));
        // With a search, its files (not "Kick Loop.wav", in another folder).
        browser->setSearchText(QStringLiteral("kick"));
        QVERIFY(settle(*browser));
        QVERIFY(!browser->showingTree());
        QCOMPARE(names(*browser->results()), QStringList({QStringLiteral("Kick.wav")}));
        // The same folder again: nothing changes.
        browser->addPlace(drums);
        QCOMPARE(browser->places(), QStringList({lib_, drums}));
        // "Add Folder…" asks the UI which folder.
        QSignalSpy asked(browser.get(), &BrowserController::addPlaceRequested);
        browser->setScope({QStringLiteral("add")});
        QCOMPARE(asked.count(), 1);
        QCOMPARE(browser->scope(), QStringList({QStringLiteral("place"), drums}));
        // Removed: the list goes back to the samples.
        browser->removePlace(drums);
        QCOMPARE(browser->places(), QStringList{lib_});
        QCOMPARE(browser->scope(), QStringList{QStringLiteral("samples")});
        QCOMPARE(browser->sidebar()->find(QStringList{QStringLiteral("place"), drums}), -1);
        QCOMPARE(QSettings().value(QStringLiteral("browser/places")).toStringList(), QStringList{lib_});
    }

    void resultsAfterTheTreeWasShownAreDropped() {
        auto browser = make();
        QVERIFY(settle(*browser));
        const QString drums = lib_ + QStringLiteral("/Drums");
        browser->addPlace(drums);
        QVERIFY(settle(*browser));
        browser->setScope({QStringLiteral("samples")});
        QVERIFY(browser->searching());
        browser->setScope({QStringLiteral("place"), drums});  // before the samples came
        QVERIFY(browser->showingTree());
        QVERIFY(!browser->searching());
        QSignalSpy shown(browser.get(), &BrowserController::resultsShown);
        QTest::qWait(300);
        QCOMPARE(shown.count(), 0);
        QVERIFY(browser->showingTree());
        // The tree's model, on demand.
        QAbstractItemModel* tree = browser->folderModel();
        QVERIFY(tree);
        QCOMPARE(tree, browser->folderModel());
        QVERIFY(browser->treeRootIndex().isValid());
    }

    void usedItemsCountAndRankFirst() {
        auto browser = make();
        QVERIFY(settle(*browser));
        ItemListModel& results = *browser->results();
        const QString snare = lib_ + QStringLiteral("/Drums/Snare.wav");
        QSignalSpy files(browser.get(), &BrowserController::fileActivated);
        browser->activate(rowOf(results, QStringLiteral("Snare.wav")));
        QCOMPARE(files.count(), 1);
        QCOMPARE(files.at(0).at(0).toString(), snare);
        QCOMPARE(browser->library().uses(audioKey(snare)), 1);
        QVERIFY(results.data(results.index(rowOf(results, QStringLiteral("Snare.wav"))), ItemListModel::ToolTipRole)
                    .toString()
                    .endsWith(QStringLiteral("\nUsed 1 time")));
        // Not re-sorted then (the selection stays put)...
        QCOMPARE(names(results).last(), QStringLiteral("Snare.wav"));
        // ...but the next search puts it first.
        browser->refresh();
        QVERIFY(settle(*browser));
        QCOMPARE(names(results).first(), QStringLiteral("Snare.wav"));
        // From the folder tree, and by drops.
        browser->activateFile(snare);
        QCOMPARE(files.count(), 2);
        browser->activateFile(lib_ + QStringLiteral("/Drums"));  // a folder: nothing
        QCOMPARE(files.count(), 2);
        browser->droppedFiles({lib_ + QStringLiteral("/Drums"), snare});
        browser->dropped({rowOf(results, QStringLiteral("Snare.wav")), 99});
        QCOMPARE(browser->library().uses(audioKey(snare)), 4);
        QCOMPARE(results.data(results.index(0), ItemListModel::UsesRole).toInt(), 4);
        QVERIFY(results.data(results.index(0), ItemListModel::ToolTipRole).toString().endsWith(QStringLiteral("\nUsed 4 times")));
        // Kept in library.json.
        QVERIFY(QFileInfo::exists(path(QStringLiteral("library.json"))));
        QCOMPARE(Library().uses(audioKey(snare)), 4);
    }

    void previewing() {
        auto browser = make();
        QVERIFY(settle(*browser));
        ItemListModel& results = *browser->results();
        QSignalSpy requested(browser.get(), &BrowserController::previewRequested);
        QSignalSpy stopped(browser.get(), &BrowserController::previewStopped);
        QVERIFY(browser->previewEnabled());
        browser->setCurrentRow(rowOf(results, QStringLiteral("Kick.wav")));
        QCOMPARE(requested.count(), 1);
        QCOMPARE(requested.at(0).at(0).toString(), lib_ + QStringLiteral("/Drums/Kick.wav"));
        QVERIFY(browser->previewing());
        browser->stopPreview();  // a click outside the browser
        QCOMPARE(stopped.count(), 1);
        QVERIFY(!browser->previewing());
        browser->setPreviewEnabled(false);
        browser->setCurrentRow(rowOf(results, QStringLiteral("Snare.wav")));
        QCOMPARE(requested.count(), 1);
        browser->setPreviewEnabled(true);
        browser->treeCurrentChanged(lib_ + QStringLiteral("/Loops/Bass Loop.flac"));
        QCOMPARE(requested.count(), 2);
        browser->treeCurrentChanged(lib_ + QStringLiteral("/Loops"));  // a folder
        browser->treeCurrentChanged(lib_ + QStringLiteral("/readme.txt"));  // not audio
        QCOMPARE(requested.count(), 2);
    }

    void enterSelectsTheFirstResult() {
        auto browser = make();
        QVERIFY(settle(*browser));
        QSignalSpy select(browser.get(), &BrowserController::selectRowRequested);
        QSignalSpy requested(browser.get(), &BrowserController::previewRequested);
        browser->setSearchText(QStringLiteral("snare"));
        browser->selectFirstResult();  // typed before the results came: selected when they do
        QCOMPARE(select.count(), 0);
        QTRY_COMPARE(select.count(), 1);
        QCOMPARE(select.at(0).at(0).toInt(), 0);
        QCOMPARE(browser->currentRow(), 0);
        QCOMPARE(requested.count(), 1);
        QCOMPARE(requested.at(0).at(0).toString(), lib_ + QStringLiteral("/Drums/Snare.wav"));
        browser->selectFirstResult();  // the results are there: at once
        QCOMPARE(select.count(), 2);
    }

    void theListKeepsItsPlaceWhenTheIndexChanges() {
#if !defined(_WIN32) && !defined(__linux__)
        QSKIP("places are watched on Windows and Linux only");
#endif
        for (int i = 0; i < 30; ++i) touch(lib_ + QStringLiteral("/Many/File %1.wav").arg(i, 2, 10, QLatin1Char('0')));
        auto browser = make();
        QVERIFY(settle(*browser));
        ItemListModel& results = *browser->results();
        const int row = rowOf(results, QStringLiteral("File 10.wav"));
        QVERIFY(row > 5);
        browser->setCurrentRow(row);
        browser->setTopRow(row - 3);
        QSignalSpy restored(browser.get(), &BrowserController::positionRestored);
        QSignalSpy requested(browser.get(), &BrowserController::previewRequested);
        touch(lib_ + QStringLiteral("/Many/AAA.wav"));  // listed before it
        QTRY_VERIFY_WITH_TIMEOUT(rowOf(results, QStringLiteral("AAA.wav")) == 0, 10000);
        QVERIFY(restored.count() >= 1);
        QCOMPARE(restored.last().at(0).toInt(), row + 1);
        QCOMPARE(restored.last().at(1).toInt(), row - 2);
        QCOMPARE(browser->currentRow(), row + 1);
        QCOMPARE(results.item(browser->currentRow())->name, QStringLiteral("File 10.wav"));
        QCOMPARE(requested.count(), 0);  // putting it back previews nothing
    }

    void pluginsAreListedFromTheScan() {
        writePlugins();
        QSignalSpy* messages = nullptr;
        auto browser = make(true, [&](BrowserController* b) {
            messages = new QSignalSpy(b, &BrowserController::statusMessage);
        });
        std::unique_ptr<QSignalSpy> owned(messages);
        QVERIFY(settle(*browser));
        QCOMPARE(browser->pluginIndex()->pluginCount(), 2);
        QCOMPARE(messages->count(), 1);
        QCOMPARE(messages->at(0).at(0).toString(),
                 QStringLiteral("1 plug-in file could not be read (hover over Plug-ins in the browser for details)."));
        QCOMPARE(browser->pluginsToolTip(),
                 QStringLiteral("VST3 plug-ins\n\nCould not be read:\nBroken.vst3: Windows could not load it: it is not a "
                                "64-bit Windows plug-in."));
        const int entry = browser->sidebar()->find(QStringList{QStringLiteral("plugins")});
        QCOMPARE(browser->sidebar()->entries()[size_t(entry)].toolTip, browser->pluginsToolTip());

        ItemListModel& results = *browser->results();
        browser->setScope({QStringLiteral("plugins")});
        QVERIFY(settle(*browser));
        QCOMPARE(names(results), QStringList({QStringLiteral("Delay"), QStringLiteral("Synth")}));
        QCOMPARE(browser->statusText(), QStringLiteral("2 plug-ins, 1 could not be read"));
        browser->setScope({QStringLiteral("plugins"), QStringLiteral("Instruments")});
        QVERIFY(settle(*browser));
        QCOMPARE(names(results), QStringList{QStringLiteral("Synth")});
        QCOMPARE(browser->statusText(), QStringLiteral("1 plug-in, 1 could not be read"));
        const QModelIndex synth = results.index(0);
        QCOMPARE(synth.data(ItemListModel::DisplayRole).toString(), QStringLiteral("Synth   (V)"));
        QCOMPARE(synth.data(ItemListModel::KindRole).toString(), QStringLiteral("plugin"));
        QCOMPARE(synth.data(ItemListModel::IconRole).toString(), QStringLiteral("plugin"));
        QVERIFY(synth.data(ItemListModel::InstrumentRole).toBool());
        QCOMPARE(synth.data(ItemListModel::ToolTipRole).toString(),
                 QStringLiteral("Synth (VST3 Instrument)\nV\nInstrument, Synth\n") + path(QStringLiteral("VST3/Synth.vst3")));
        browser->setSearchText(QStringLiteral("syn"));
        QVERIFY(settle(*browser));
        QCOMPARE(browser->statusText(), QStringLiteral("1 plug-in"));  // no note while searching

        // Activated: the plug-in to add; dragged: its PluginRef fields.
        QSignalSpy activated(browser.get(), &BrowserController::pluginActivated);
        browser->activate(0);
        QCOMPARE(activated.count(), 1);
        const QVariantMap ref = activated.at(0).at(0).toMap();
        QCOMPARE(ref.value(QStringLiteral("uid")).toString(), QStringLiteral("S1"));
        QCOMPARE(ref.value(QStringLiteral("format")).toString(), QStringLiteral("VST3"));
        QCOMPARE(ref.value(QStringLiteral("instrument")).toBool(), true);
        QCOMPARE(browser->library().uses(QStringLiteral("plugin:VST3:S1")), 1);
        const QVariantMap drag = browser->dragData({0});
        QCOMPARE(drag.keys(), QStringList{QString::fromLatin1(kPluginMime)});
        const auto refs = pluginRefs(drag.value(QString::fromLatin1(kPluginMime)).toString().toUtf8());
        QCOMPARE(refs.size(), size_t(1));
        QCOMPARE(refs[0].uid, QStringLiteral("S1"));
        QCOMPARE(refs[0].name, QStringLiteral("Synth"));
        QCOMPARE(refs[0].vendor, QStringLiteral("V"));
        QCOMPARE(refs[0].path, path(QStringLiteral("VST3/Synth.vst3")));
        QVERIFY(refs[0].instrument);

        // Rescan Plug-ins reads every file again (here: the scanner can't start, which the status line says).
        browser->rescanPlugins();
        QTRY_VERIFY_WITH_TIMEOUT(!browser->pluginIndex()->scanning(), 20000);
        QStringList said;
        for (const QList<QVariant>& message : *messages) said << message.at(0).toString();
        QVERIFY(said.contains(QStringLiteral("The plug-in scanner could not start.")));
        QCOMPARE(browser->pluginIndex()->pluginCount(), 2);  // what it had stays
    }

    void allListsEverything() {
        writePlugins();
        auto browser = make();
        QVERIFY(settle(*browser));
        browser->setPresets({{QStringLiteral("Warm"), path(QStringLiteral("Presets/EQ/Warm.gilpreset")), ItemKind::Preset,
                              QStringLiteral("EQ"), std::nullopt, {}}},
                            {QStringLiteral("EQ")}, path(QStringLiteral("Presets")));
        browser->setScope({QStringLiteral("all")});
        QVERIFY(settle(*browser));
        // The list's own order: built-in devices, plug-ins, presets, samples.
        QStringList expected;
        for (const BrowserItem& item : builtinItems()) expected << item.name;
        expected << QStringLiteral("Delay") << QStringLiteral("Synth") << QStringLiteral("Warm") << QStringLiteral("Bass Loop.flac")
                 << QStringLiteral("Kick Loop.wav") << QStringLiteral("Kick.wav") << QStringLiteral("Snare.wav");
        QCOMPARE(names(*browser->results()), expected);
        QCOMPARE(browser->statusText(), QStringLiteral("%1 items").arg(expected.size()));
        // Ctrl+F searches here.
        browser->setScope({QStringLiteral("samples")});
        QSignalSpy focus(browser.get(), &BrowserController::searchFocusRequested);
        browser->focusSearch();
        QCOMPARE(browser->scope(), QStringList{QStringLiteral("all")});
        QCOMPARE(focus.count(), 1);
    }

    void builtInDevices() {
        auto browser = make();
        QVERIFY(settle(*browser));
        browser->setScope({QStringLiteral("builtin")});
        QVERIFY(settle(*browser));
        QStringList expected;
        for (const BrowserItem& item : builtinItems()) expected << item.name;
        QCOMPARE(names(*browser->results()), expected);
        for (const QString& category : builtinCategories()) {
            browser->setScope({QStringLiteral("builtin"), category});
            QVERIFY(settle(*browser));
            QCOMPARE(browser->results()->total(), int(builtinItems(category).size()));
        }
        if (!expected.isEmpty()) {
            QSignalSpy devices(browser.get(), &BrowserController::deviceActivated);
            browser->setScope({QStringLiteral("builtin")});
            QVERIFY(settle(*browser));
            browser->activate(0);
            QCOMPARE(devices.count(), 1);
            QCOMPARE(devices.at(0).at(0).toString(), builtinItems().front().path);
            const QVariantMap drag = browser->dragData({0});
            QCOMPARE(deviceKinds(drag.value(QString::fromLatin1(kDeviceMime)).toString().toUtf8()),
                     QStringList{builtinItems().front().path});
        }
    }

    void presetsAreListedByDevice() {
        auto browser = make();
        QVERIFY(settle(*browser));
        browser->setScope({QStringLiteral("presets")});
        QVERIFY(settle(*browser));
        QCOMPARE(browser->statusText(), QStringLiteral("No presets yet: save one with a device's save button"));
        const QString root = path(QStringLiteral("Presets"));
        const BrowserItem warm{QStringLiteral("Warm"), root + QStringLiteral("/EQ/Warm.gilpreset"), ItemKind::Preset,
                               QStringLiteral("EQ"), std::nullopt, QStringLiteral("Warm\nEQ preset")};
        const BrowserItem loud{QStringLiteral("Loud"), root + QStringLiteral("/Loud.gilpreset"), ItemKind::Preset,
                               QStringLiteral("Other"), std::nullopt, {}};
        browser->setPresets({warm, loud}, {QStringLiteral("EQ"), QStringLiteral("Other")}, root);
        QCOMPARE(browser->sidebar()->children(Scope{QStringLiteral("presets"), {}}),
                 QStringList({QStringLiteral("EQ"), QStringLiteral("Other")}));
        QCOMPARE(browser->scope(), QStringList{QStringLiteral("presets")});
        QVERIFY(settle(*browser));
        QCOMPARE(names(*browser->results()), QStringList({QStringLiteral("Warm"), QStringLiteral("Loud")}));
        QCOMPARE(browser->statusText(), QStringLiteral("2 presets"));
        browser->setScope({QStringLiteral("presets"), QStringLiteral("EQ")});
        QVERIFY(settle(*browser));
        QCOMPARE(names(*browser->results()), QStringList{QStringLiteral("Warm")});
        QCOMPARE(browser->statusText(), QStringLiteral("1 preset"));
        const QModelIndex first = browser->results()->index(0);
        QCOMPARE(first.data(ItemListModel::DisplayRole).toString(), QStringLiteral("Warm   (EQ)"));
        QCOMPARE(first.data(ItemListModel::IconRole).toString(), QStringLiteral("preset"));
        QCOMPARE(first.data(ItemListModel::ToolTipRole).toString(), QStringLiteral("Warm\nEQ preset"));
        QCOMPARE(browser->presetFolder({QStringLiteral("presets"), QStringLiteral("EQ")}), root + QStringLiteral("/EQ"));
        QCOMPARE(browser->presetFolder({QStringLiteral("presets")}), root);
        QSignalSpy activated(browser.get(), &BrowserController::presetActivated);
        browser->activate(0);
        QCOMPARE(activated.at(0).at(0).toString(), warm.path);
        QCOMPARE(presetPaths(browser->dragData({0}).value(QString::fromLatin1(kPresetMime)).toString().toUtf8()),
                 QStringList{warm.path});
        // Its group went: the list goes to all presets.
        browser->setPresets({loud}, {QStringLiteral("Other")}, root);
        QCOMPARE(browser->scope(), QStringList{QStringLiteral("presets")});
        QVERIFY(settle(*browser));
        QCOMPARE(names(*browser->results()), QStringList{QStringLiteral("Loud")});

        // Its menu: rename, delete (to the trash), show in folder.
        auto actions = [](const QVariantList& list) {
            QStringList ids;
            for (const QVariant& a : list) ids << a.toMap().value(QStringLiteral("action")).toString();
            return ids;
        };
        QCOMPARE(actions(browser->resultActions(0)),
                 QStringList({QStringLiteral("renamePreset"), QStringLiteral("deletePreset"), QString(), QStringLiteral("showInFolder")}));
        QCOMPARE(browser->resultActions(0)[0].toMap().value(QStringLiteral("label")).toString(), QStringLiteral("Rename…"));
        QSignalSpy changed(browser.get(), &BrowserController::presetsChanged);
        QSignalSpy messages(browser.get(), &BrowserController::statusMessage);
        QVERIFY(!browser->deletePreset(root + QStringLiteral("/Missing.gilpreset")));
        QCOMPARE(messages.at(0).at(0).toString(), QStringLiteral("Could not delete the preset Missing."));
        QCOMPARE(changed.count(), 0);
        // (A preset that is there goes to the user's own trash, which a test leaves alone.)
        // The sidebar's menus.
        QCOMPARE(actions(browser->sidebarActions({QStringLiteral("presets"), QStringLiteral("EQ")})),
                 QStringList({QStringLiteral("showPresetFolder"), QString(), QStringLiteral("addPlace"), QStringLiteral("rescan")}));
        QCOMPARE(actions(browser->sidebarActions({QStringLiteral("plugins")})),
                 QStringList({QStringLiteral("rescanPlugins"), QString(), QStringLiteral("addPlace"), QStringLiteral("rescan")}));
        QCOMPARE(actions(browser->sidebarActions({QStringLiteral("place"), lib_})),
                 QStringList({QStringLiteral("removePlace"), QStringLiteral("addPlace"), QStringLiteral("rescan")}));
        QCOMPARE(actions(browser->sidebarActions({QStringLiteral("samples")})),
                 QStringList({QStringLiteral("addPlace"), QStringLiteral("rescan")}));
        QVERIFY(browser->resultActions(5).isEmpty());
    }

    void theListModelPagesAndDrags() {
        std::vector<BrowserItem> items;
        for (int i = 0; i < 600; ++i)
            items.push_back({QStringLiteral("File %1.wav").arg(i), path(QStringLiteral("x/File %1.wav").arg(i)), ItemKind::Audio,
                             QStringLiteral("x"), std::nullopt, {}});
        ItemListModel model;
        model.setItems(items);
        QCOMPARE(model.total(), 600);
        QCOMPARE(model.rowCount(), ItemListModel::kPage);
        QVERIFY(model.canFetchMore(QModelIndex()));
        model.fetchMore(QModelIndex());
        model.fetchMore(QModelIndex());
        QCOMPARE(model.rowCount(), 600);
        QVERIFY(!model.canFetchMore(QModelIndex()));
        const QModelIndex first = model.index(0);
        QVERIFY(model.flags(first) & Qt::ItemIsDragEnabled);
        QCOMPARE(first.data(ItemListModel::DisplayRole).toString(), QStringLiteral("File 0.wav"));
        QCOMPARE(first.data(ItemListModel::IconRole).toString(), QStringLiteral("waveform"));
        QCOMPARE(first.data(ItemListModel::ToolTipRole).toString(), items[0].path);  // no tooltip of its own: the path
        QCOMPARE(model.get(1).value(QStringLiteral("name")).toString(), QStringLiteral("File 1.wav"));
        const QHash<int, QByteArray> roles = model.roleNames();
        for (const char* role : {"name", "path", "kind", "detail", "key", "display", "toolTip", "icon", "uses", "instrument", "plugin"})
            QVERIFY2(roles.values().contains(QByteArray(role)), role);
        QCOMPARE(model.mimeTypes(), browserMimeTypes());
        std::unique_ptr<QMimeData> mime(model.mimeData({model.index(0), model.index(1)}));
        QCOMPARE(mime->urls(), QList<QUrl>({QUrl::fromLocalFile(items[0].path), QUrl::fromLocalFile(items[1].path)}));
        QVERIFY(!mime->hasFormat(QString::fromLatin1(kPluginMime)));
    }

    void dragsAreReadWhereTheyAreDropped() {
        PluginInfo synth;
        synth.format = QStringLiteral("VST3");
        synth.uid = QStringLiteral("S1");
        synth.name = QStringLiteral("Synth");
        synth.vendor = QStringLiteral("V");
        synth.path = QStringLiteral("/p/Synth.vst3");
        synth.instrument = true;
        const std::vector<BrowserItem> items{pluginItem(synth),
                                             {QStringLiteral("Utility"), QStringLiteral("utility"), ItemKind::Device},
                                             {QStringLiteral("Warm"), QStringLiteral("/p/Warm.gilpreset"), ItemKind::Preset},
                                             {QStringLiteral("Kick.wav"), QStringLiteral("/s/Kick.wav"), ItemKind::Audio}};
        const auto mime = browserMimeData(items);
        QCOMPARE(mime->urls(), QList<QUrl>{QUrl::fromLocalFile(QStringLiteral("/s/Kick.wav"))});
        const auto refs = pluginRefs(mime.get());
        QCOMPARE(refs.size(), size_t(1));
        QCOMPARE(refs[0].toRef(), synth.toRef());
        QCOMPARE(deviceKinds(mime.get()), QStringList{QStringLiteral("utility")});
        QCOMPARE(presetPaths(mime.get()), QStringList{QStringLiteral("/p/Warm.gilpreset")});
        // Not from the browser, or not as written: nothing.
        QMimeData other;
        other.setText(QStringLiteral("hello"));
        QVERIFY(pluginRefs(&other).empty());
        QVERIFY(deviceKinds(&other).isEmpty());
        QVERIFY(presetPaths(&other).isEmpty());
        QVERIFY(pluginRefs(QByteArray("{not json")).empty());
        QVERIFY(pluginRefs(QByteArray(R"([{"format": "VST3", "name": "x"}])")).empty());  // no uid
        QCOMPARE(deviceKinds(QByteArray(R"(["a", 1, "b"])")), QStringList({QStringLiteral("a"), QStringLiteral("b")}));
        QVERIFY(presetPaths(QByteArray(R"({"a": 1})")).isEmpty());
        // As QML's Drag takes it: text by type.
        const QVariantMap drag = browserDragData(items);
        QCOMPARE(drag.value(QStringLiteral("text/uri-list")).toString(),
                 QUrl::fromLocalFile(QStringLiteral("/s/Kick.wav")).toString(QUrl::FullyEncoded) + QStringLiteral("\r\n"));
        QCOMPARE(deviceKinds(drag.value(QString::fromLatin1(kDeviceMime)).toString().toUtf8()), QStringList{QStringLiteral("utility")});
    }

private:
    QString path(const QString& relative) const { return tmp_->filePath(relative); }

    static void clearSettings() {
        QSettings settings;
        settings.remove(QStringLiteral("browser"));
        settings.remove(QStringLiteral("plugins"));
    }

    BrowserController::Options options(bool scanPlugins = true) const {
        BrowserController::Options o;
        o.scanPlugins = scanPlugins;
        return o;
    }

    // A browser with the test library as its place.
    std::unique_ptr<BrowserController> make(bool scanPlugins = true,
                                            const std::function<void(BrowserController*)>& before = {}) {
        QSettings().setValue(QStringLiteral("browser/places"), QStringList{lib_});
        auto browser = std::make_unique<BrowserController>(nullptr, options(scanPlugins));
        if (before) before(browser.get());
        return browser;
    }

    // Until nothing is on its way: indexing, scanning plug-ins, searching.
    static bool settle(BrowserController& browser) {
        auto quiet = [&] {
            return !browser.searching() && !browser.indexing() && !browser.pluginIndex()->scanning();
        };
        for (int i = 0; i < 2; ++i) {
            if (!QTest::qWaitFor(quiet, 20000)) return false;
            QTest::qWait(50);  // what the backend says next (its wake is queued)
        }
        return quiet();
    }

    // Two plug-ins and a broken file in the standard folder, all in the scan's
    // cache (so they are not read: the scanner never starts).
    void writePlugins() {
        const QString synth = path(QStringLiteral("VST3/Synth.vst3"));
        const QString delay = path(QStringLiteral("VST3/Effects/Delay.vst3"));
        const QString broken = path(QStringLiteral("VST3/Broken.vst3"));
        touch(synth, "synth");
        touch(delay, "delay");
        touch(broken, "broken");
        QJsonObject files;
        auto add = [&](const QString& file, const QJsonArray& plugins, const QJsonValue& error) {
            const auto signature = pluginSignature(file);
            QVERIFY(signature);
            files.insert(caseKey(file), QJsonObject{{QStringLiteral("path"), file},
                                                    {QStringLiteral("signature"), QJsonArray{qint64((*signature)[0]), qint64((*signature)[1])}},
                                                    {QStringLiteral("plugins"), plugins},
                                                    {QStringLiteral("error"), error}});
        };
        add(synth, {pluginJson(QStringLiteral("Synth"), QStringLiteral("V"), QStringLiteral("Instrument|Synth"), true, QStringLiteral("S1"))},
            QJsonValue());
        add(delay, {pluginJson(QStringLiteral("Delay"), QStringLiteral("V"), QStringLiteral("Fx|Delay"), false, QStringLiteral("D1"))},
            QJsonValue());
        add(broken, {}, QStringLiteral("LoadLibraryW failed with error number: 193 for path x"));
        touch(path(QStringLiteral("vst3-cache.json")),
              QJsonDocument(QJsonObject{{QStringLiteral("version"), 1}, {QStringLiteral("files"), files}}).toJson());
    }

    std::unique_ptr<QTemporaryDir> tmp_;
    QString lib_;
};

QTEST_GUILESS_MAIN(TestBrowserController)
#include "test_browser_controller.moc"
