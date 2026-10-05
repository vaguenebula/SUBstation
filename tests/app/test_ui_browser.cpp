// The browser panel (BrowserPanel.qml) on a real session (the browser parts
// of tests/test_ui_smoke.py, tests/test_ui_presets.py's and the panel's side
// of tests/test_browser_*.py): the sidebar, searching, Enter and Down from the
// search field, previews (stopped by a press outside the browser), activating
// results, the selection and what a drag carries, keeping the list's place,
// paging, the sort, used items ranking first, a place's folder tree, the
// context menus, adding places, renaming and deleting presets. Runs on a
// display (xvfb here). With $SUBSTATION_SCREENS set, it saves screenshots.

#include <QDir>
#include <QFileInfo>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>
#include <QUndoStack>

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "UiTestSupport.h"
#include "audio/EngineBridge.h"
#include "browser/BrowserController.h"
#include "browser/BrowserMime.h"
#include "browser/ItemListModel.h"
#include "editor/ProjectEditor.h"
#include "mainwindow/FolderTreeModel.h"
#include "model/Project.h"
#include "session/DeviceSelection.h"
#include "session/Selection.h"

namespace test = sub::app::test;
using sub::app::BrowserController;

namespace {

// The window (at the end of the file: moc skips what follows a raw string).
extern const char* const kWindow;

std::vector<float> tone(double seconds) { return std::vector<float>(size_t(seconds * test::kSampleRate), 0.2f); }

}  // namespace

class TestUiBrowser : public QObject {
    Q_OBJECT

    std::unique_ptr<test::TempDir> dir_;
    std::unique_ptr<test::UiSession> ui_;
    QQuickWindow* window_ = nullptr;
    QQuickItem* panel_ = nullptr;
    QList<QQmlError> warnings_;

    sub::app::Session& session() { return ui_->session(); }
    sub::app::Project& project() { return *session().project(); }
    BrowserController& browser() { return *session().browser(); }
    sub::app::ItemListModel& results() { return *browser().results(); }
    QQuickItem* item(const char* name) { return window_->findChild<QQuickItem*>(QString::fromLatin1(name)); }
    // An item shown in the window, by its name (delegates too, which aren't the window's QObject children).
    static QQuickItem* shownItem(QQuickItem* root, const QString& name) {
        if (root->objectName() == name && root->isVisible()) return root;
        for (QQuickItem* child : root->childItems())
            if (QQuickItem* found = shownItem(child, name)) return found;
        return nullptr;
    }
    QQuickItem* renameField() { return shownItem(window_->contentItem(), QStringLiteral("renameField")); }
    QObject* object(const char* name) { return window_->findChild<QObject*>(QString::fromLatin1(name)); }
    QQuickItem* list() { return item("resultsList"); }
    QQuickItem* tree() { return item("folderTree"); }
    QString place() const { return dir_->path(QStringLiteral("Place")); }

    // Until the browser shows its latest search's results.
    bool settle() {
        const bool done = QTest::qWaitFor([this] { return !browser().searching(); }, 5000);
        QTest::qWait(10);
        return done && !browser().searching();
    }
    // Until the index is done and has `count` files at least.
    bool indexed(int count) {
        return QTest::qWaitFor([this, count] { return !browser().indexing() && browser().fileCount() >= count; }, 10000);
    }
    void search(const QString& text) {
        browser().setSearchText(text);
        QVERIFY(settle());
    }
    QStringList names() {
        QStringList out;
        for (int row = 0; row < results().rowCount(); ++row) out << results().get(row).value(QStringLiteral("name")).toString();
        return out;
    }
    void typeInSearch(const QString& text) {
        auto* field = item("searchField");
        field->forceActiveFocus();
        for (const QChar c : text) QTest::keyClick(window_, c.toLatin1());
    }
    // The point of a row of a list (results or tree), in the window.
    QPoint rowPoint(QQuickItem* view, int row) {
        QQuickItem* delegate = nullptr;
        QMetaObject::invokeMethod(view, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, delegate), Q_ARG(int, row));
        if (!delegate) return {};
        return test::at(delegate, QPointF(60, delegate->height() / 2));
    }
    QVariantList selected(QQuickItem* view) { return view->property("selectedRows").toList(); }
    int sidebarRow(const QStringList& scope) { return browser().sidebar()->find(scope); }

private Q_SLOTS:
    void initTestCase() {
        test::prepareApplication();
        if (!test::haveDisplay()) QSKIP("needs a display: Qt Quick's software renderer draws none of this geometry");
        dir_ = std::make_unique<test::TempDir>();
        QDir().mkpath(place() + QStringLiteral("/Drums"));
        test::writeWav(place() + QStringLiteral("/Drums/Kick Deep.wav"), tone(0.2));
        test::writeWav(place() + QStringLiteral("/Drums/Snare Tight.wav"), tone(0.2));
        test::writeWav(place() + QStringLiteral("/Utility Hit.wav"), tone(0.2));
        QFile notes(place() + QStringLiteral("/Drums/notes.txt"));
        QVERIFY(notes.open(QIODevice::WriteOnly));
        notes.write("not audio");
        notes.close();
        QSettings().setValue(QStringLiteral("browser/places"), QStringList{place()});
        sub::ui::setUpApplication();
        ui_ = std::make_unique<test::UiSession>();
        connect(&ui_->qml(), &QQmlEngine::warnings, this, [this](const QList<QQmlError>& list) { warnings_ += list; });
        window_ = ui_->show(kWindow);
        QVERIFY(window_);
        panel_ = item("browser");
        QVERIFY(panel_);
        QVERIFY(indexed(3));
    }

    void cleanupTestCase() { ui_.reset(); }

    void init() {
        session().newProject();
        browser().setPreviewEnabled(true);
        browser().stopPreview();
        browser().setSearchText(QString());
        browser().setScope({QStringLiteral("samples")});
        QVERIFY(settle());
    }

    void cleanup() {
        const QList<QQmlError> warnings = std::exchange(warnings_, {});
        for (const QQmlError& warning : warnings) qWarning() << warning.toString();
        QVERIFY(warnings.isEmpty());
    }

    // The sidebar: CATEGORIES and PLACES, entries clicked show what they list.
    void sidebar() {
        QVERIFY(sidebarRow({QStringLiteral("all")}) > 0);
        QVERIFY(sidebarRow({QStringLiteral("builtin"), QStringLiteral("Audio Effects")}) > 0);
        QVERIFY(sidebarRow({QStringLiteral("place"), place()}) > 0);
        QCOMPARE(results().total(), 3);
        QCOMPARE(item("browserStatus")->property("text").toString(), QStringLiteral("3 items"));

        // Built-in › Audio Effects (test_builtin_devices_in_browser).
        QQuickItem* sidebarList = item("sidebarList");
        const int effects = sidebarRow({QStringLiteral("builtin"), QStringLiteral("Audio Effects")});
        QQuickItem* entry = nullptr;
        QMetaObject::invokeMethod(sidebarList, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, entry), Q_ARG(int, effects));
        QVERIFY(entry);
        test::click(window_, test::centerOf(entry));
        QCOMPARE(browser().scope(), (QStringList{QStringLiteral("builtin"), QStringLiteral("Audio Effects")}));
        QVERIFY(settle());
        QStringList effectNames = names();
        effectNames.sort();
        QCOMPARE(effectNames, (QStringList{QStringLiteral("Compressor"), QStringLiteral("Delay"), QStringLiteral("EQ"),
                                           QStringLiteral("Over The Top"), QStringLiteral("Sidechain"),
                                           QStringLiteral("Utility")}));
        const int utility = int(names().indexOf(QStringLiteral("Utility")));
        QVariant data;
        QMetaObject::invokeMethod(panel_, "dragPayload", Q_RETURN_ARG(QVariant, data),
                                  Q_ARG(QVariant, QVariantList{utility}), Q_ARG(QVariant, false));
        const QByteArray kinds = data.toMap().value(QString::fromLatin1(sub::app::kDeviceMime)).toString().toUtf8();
        QCOMPARE(sub::app::deviceKinds(kinds), QStringList{QStringLiteral("utility")});

        // A double-click adds it to the selected track.
        const QString track = session().insertAudioTrack();
        test::doubleClick(window_, rowPoint(list(), utility));
        QCOMPARE(project().track(track).devices.size(), size_t(1));
        QCOMPARE(project().track(track).devices[0].kind, QStringLiteral("utility"));
        test::screenshot(window_, QStringLiteral("browser-builtin"), QRect(0, 0, 400, window_->height()));

        // The Plug-ins entry's tooltip lists what couldn't be read.
        QVERIFY(browser().pluginsToolTip().startsWith(QStringLiteral("VST3 plug-ins")));
    }

    // Searching (test_browser_indexes_and_searches): words in the name or its
    // folder; a result's drag carries its file; added at the insert marker.
    void searchAndActivate() {
        search(QStringLiteral("kick"));
        QCOMPARE(names(), QStringList{QStringLiteral("Kick Deep.wav")});
        QVariant data;
        QMetaObject::invokeMethod(panel_, "dragPayload", Q_RETURN_ARG(QVariant, data), Q_ARG(QVariant, QVariantList{0}),
                                  Q_ARG(QVariant, false));
        QVERIFY(data.toMap().value(QStringLiteral("text/uri-list")).toString().contains(QStringLiteral("Kick%20Deep.wav")));
        search(QStringLiteral("drums"));  // the folder's name
        QCOMPARE(names().size(), 2);
        browser().setPreviewEnabled(false);
        list()->forceActiveFocus();
        QMetaObject::invokeMethod(list(), "moveCurrent", Q_ARG(QVariant, 0), Q_ARG(QVariant, false));
        QTest::keyClick(window_, Qt::Key_Return);
        QCOMPARE(project().tracks().size(), size_t(1));
        QCOMPARE(project().tracks()[0].clips[0].name, QFileInfo(names()[0]).completeBaseName());
    }

    // Down in the search field selects the first result and previews it, until
    // a press outside the browser (test_down_from_search_previews...).
    void downPreviewsUntilAPressOutside() {
        QSignalSpy previewed(&browser(), &BrowserController::previewRequested);
        QSignalSpy stopped(&browser(), &BrowserController::previewStopped);
        typeInSearch(QStringLiteral("snare tight"));
        QTest::keyClick(window_, Qt::Key_Down);
        QVERIFY(settle());
        QTRY_COMPARE(list()->property("currentIndex").toInt(), 0);
        QTRY_VERIFY(list()->hasActiveFocus());
        QVERIFY(!previewed.isEmpty());
        QVERIFY(previewed.last().at(0).toString().endsWith(QStringLiteral("Snare Tight.wav")));
        QVERIFY(browser().previewing());
        test::click(window_, test::at(list(), QPointF(100, list()->height() - 20)));  // in the browser: it plays on
        QCOMPARE(stopped.size(), 0);
        test::click(window_, test::centerOf(item("outside")));
        QCOMPARE(stopped.size(), 1);
        test::click(window_, test::centerOf(item("outside")) + QPoint(10, 0));
        QCOMPARE(stopped.size(), 1);  // nothing playing any more

        // The headphones off: selecting previews nothing.
        test::click(window_, test::centerOf(item("preview")));
        QVERIFY(!browser().previewEnabled());
        previewed.clear();
        search(QStringLiteral("kick"));
        test::click(window_, rowPoint(list(), 0));
        QCOMPARE(list()->property("currentIndex").toInt(), 0);
        QVERIFY(previewed.isEmpty());
        test::click(window_, test::centerOf(item("preview")));
        QVERIFY(browser().previewEnabled());
    }

    // Ctrl+F searches everything (test_find_searches_all); Enter before the
    // results came selects the first when they do, Enter again adds it.
    void findAndEnter() {
        const QString track = session().insertAudioTrack();
        browser().focusSearch();
        QCOMPARE(browser().scope(), QStringList{QStringLiteral("all")});
        QTRY_VERIFY(item("searchField")->hasActiveFocus());
        typeInSearch(QStringLiteral("utility"));
        QCOMPARE(item("searchField")->property("text").toString(), QStringLiteral("utility"));
        QTest::keyClick(window_, Qt::Key_Return);
        QVERIFY(settle());
        QSet<QString> kinds;
        for (int row = 0; row < results().rowCount(); ++row) kinds << results().get(row).value(QStringLiteral("kind")).toString();
        QCOMPARE(kinds, (QSet<QString>{QStringLiteral("device"), QStringLiteral("audio")}));
        QTRY_COMPARE(list()->property("currentIndex").toInt(), 0);
        QCOMPARE(results().get(0).value(QStringLiteral("name")).toString(), QStringLiteral("Utility"));
        QTRY_VERIFY(list()->hasActiveFocus());
        QVERIFY(project().track(track).devices.empty());
        QTest::keyClick(window_, Qt::Key_Return);
        QCOMPARE(project().track(track).devices.size(), size_t(1));
    }

    // The selection: Ctrl toggles, Shift a range, keys move it; a drag carries every selected row.
    void selectionAndDragPayload() {
        browser().setPreviewEnabled(false);
        browser().setScope({QStringLiteral("all")});
        QVERIFY(settle());
        QVERIFY(results().rowCount() >= 5);
        test::click(window_, rowPoint(list(), 1));
        QCOMPARE(selected(list()), QVariantList{1});
        test::click(window_, rowPoint(list(), 3), Qt::ControlModifier);
        QCOMPARE(selected(list()).size(), 2);
        QVariant data;
        QVariant rows;
        QMetaObject::invokeMethod(list(), "sortedSelection", Q_RETURN_ARG(QVariant, rows));
        QCOMPARE(rows.toList(), (QVariantList{1, 3}));
        QMetaObject::invokeMethod(panel_, "dragPayload", Q_RETURN_ARG(QVariant, data), Q_ARG(QVariant, rows),
                                  Q_ARG(QVariant, false));
        QCOMPARE(data.toMap(), browser().dragData({1, 3}));
        test::click(window_, rowPoint(list(), 4), Qt::ShiftModifier);
        QCOMPARE(selected(list()).size(), 2);  // 3 to 4 (from the last clicked)
        QTest::keyClick(window_, Qt::Key_Up, Qt::ShiftModifier);
        QCOMPARE(list()->property("currentIndex").toInt(), 3);
        QTest::keyClick(window_, Qt::Key_Down);
        QCOMPARE(selected(list()), QVariantList{4});
        // A drop that took them: they count as used.
        QObject* source = panel_->property("dragSource").value<QObject*>();
        QVERIFY(source);
        source->setProperty("rows", QVariantList{4});
        source->setProperty("fromTree", false);
        const QString key = results().get(4).value(QStringLiteral("key")).toString();
        const int before = browser().library().uses(key);
        QMetaObject::invokeMethod(panel_, "dragFinished", Q_ARG(QVariant, int(Qt::CopyAction)));
        QCOMPARE(browser().library().uses(key), before + 1);
        QMetaObject::invokeMethod(panel_, "dragFinished", Q_ARG(QVariant, int(Qt::IgnoreAction)));
        QCOMPARE(browser().library().uses(key), before + 1);
    }

    // A real drag (the platform's) out of the list onto a drop area: it
    // carries the selected rows' data; dropped, they count as used.
    void dragAndDrop() {
        browser().setPreviewEnabled(false);
        browser().setScope({QStringLiteral("builtin"), QStringLiteral("Audio Effects")});
        QVERIFY(settle());
        const int utility = int(names().indexOf(QStringLiteral("Utility")));
        const QString key = results().get(utility).value(QStringLiteral("key")).toString();
        const int before = browser().library().uses(key);
        const QPoint from = rowPoint(list(), utility);
        const QPoint to = test::centerOf(item("outside"));
        // The drag runs an event loop of its own: the mouse moves and lets go meanwhile.
        QTimer::singleShot(100, this, [&] {
            test::moveTo(window_, from + QPoint(60, 0));
            QTest::qWait(20);
            test::moveTo(window_, to);
            QTest::qWait(20);
            test::release(window_, to);
        });
        test::press(window_, from);
        test::moveTo(window_, from + QPoint(30, 0));  // past the drag distance: it starts
        QObject* drop = object("dropArea");
        QTRY_VERIFY_WITH_TIMEOUT(!drop->property("devices").toString().isEmpty(), 5000);
        QCOMPARE(sub::app::deviceKinds(drop->property("devices").toString().toUtf8()), QStringList{QStringLiteral("utility")});
        QTRY_COMPARE(browser().library().uses(key), before + 1);
    }

    // Used items rank first; Name sorts by name (test_used_items_rank_first).
    void usedItemsRankFirst() {
        browser().setScope({QStringLiteral("all")});
        search(QStringLiteral("e"));
        const QStringList before = names();
        const QString last = before.last();
        for (int i = 0; i < 3; ++i) browser().activate(int(before.size()) - 1);  // double-clicks (more than any other's)
        search(QStringLiteral("e "));
        QCOMPARE(names().first(), last);
        QMetaObject::invokeMethod(item("sort"), "activated", Q_ARG(int, 1));
        QCOMPARE(browser().sort(), QStringLiteral("name"));
        QVERIFY(settle());
        QStringList sorted = before;
        std::sort(sorted.begin(), sorted.end(), [](const QString& a, const QString& b) { return a.compare(b, Qt::CaseInsensitive) < 0; });
        QCOMPARE(names(), sorted);
        QCOMPARE(QSettings().value(QStringLiteral("browser/sort")).toString(), QStringLiteral("name"));
        QCOMPARE(item("sort")->property("displayText").toString(), QStringLiteral("Name"));
        QMetaObject::invokeMethod(item("sort"), "activated", Q_ARG(int, 0));
        QCOMPARE(browser().sort(), QStringLiteral("rank"));
    }

    // A list searched again for a change elsewhere keeps its current item
    // (test_browser_keeps_its_place_when_files_change).
    void keepsItsPlace() {
        browser().setPreviewEnabled(false);
        search(QStringLiteral("wav"));
        QCOMPARE(names().size(), 3);
        const int snare = int(names().indexOf(QStringLiteral("Snare Tight.wav")));
        test::click(window_, rowPoint(list(), snare));
        test::writeWav(place() + QStringLiteral("/Drums/Big Kick.wav"), tone(0.1));
        browser().rescan();
        QTRY_VERIFY_WITH_TIMEOUT(results().total() == 4 && !browser().searching(), 10000);
        QTRY_COMPARE(results().get(list()->property("currentIndex").toInt()).value(QStringLiteral("name")).toString(),
                     QStringLiteral("Snare Tight.wav"));
        QFile::remove(place() + QStringLiteral("/Drums/Big Kick.wav"));
        browser().rescan();
        QTRY_VERIFY_WITH_TIMEOUT(results().total() == 3 && !browser().searching(), 10000);
    }

    // A long list comes a page at a time, more as it scrolls to its end.
    void paging() {
        const QString many = dir_->path(QStringLiteral("Many"));
        QDir().mkpath(many);
        for (int i = 0; i < 300; ++i) test::writeWav(many + QStringLiteral("/Hit %1.wav").arg(i, 3, 10, QLatin1Char('0')), tone(0.01));
        browser().addPlace(many);
        QTRY_VERIFY_WITH_TIMEOUT(!browser().indexing() && browser().fileCount() >= 303, 20000);
        browser().setScope({QStringLiteral("samples")});
        search(QStringLiteral("hit"));
        QCOMPARE(results().total(), 301);
        QTest::qWait(50);
        QCOMPARE(results().rowCount(), sub::app::ItemListModel::kPage);  // (the view doesn't fetch them all)
        QCOMPARE(list()->property("count").toInt(), sub::app::ItemListModel::kPage);
        QMetaObject::invokeMethod(list(), "positionViewAtEnd");
        QTRY_COMPARE(results().rowCount(), 301);
        QTRY_COMPARE(list()->property("count").toInt(), 301);
        browser().removePlace(browser().places().last());
        QVERIFY(settle());
    }

    // A place with no search text shows its folder tree; results that come
    // after it was shown are dropped (test_results_after_the_tree_was_shown_are_dropped).
    void folderTree() {
        browser().setScope({QStringLiteral("place"), place()});
        QVERIFY(browser().showingTree());
        QVERIFY(tree()->isVisible() && !list()->isVisible());
        auto* folders = tree()->property("folders").value<sub::ui::FolderTreeModel*>();
        QVERIFY(folders);
        QTRY_COMPARE(folders->count(), 2);  // Drums, Utility Hit.wav (folders first)
        QCOMPARE(folders->data(folders->index(0), sub::ui::FolderTreeModel::NameRole).toString(), QStringLiteral("Drums"));
        QVERIFY(folders->isDir(0) && !folders->isDir(1));
        QCOMPARE(item("browserStatus")->property("text").toString(), QDir::toNativeSeparators(place()));

        // Opened: its audio files (not notes.txt) follow it.
        tree()->forceActiveFocus();
        QMetaObject::invokeMethod(tree(), "moveCurrent", Q_ARG(QVariant, 0), Q_ARG(QVariant, false));
        QTest::keyClick(window_, Qt::Key_Right);
        QTRY_COMPARE(folders->count(), 4);
        QCOMPARE(folders->depth(1), 1);
        const QStringList drums{place() + QStringLiteral("/Drums/Kick Deep.wav"),
                                place() + QStringLiteral("/Drums/Snare Tight.wav")};
        QCOMPARE(folders->paths({1, 2}), drums);
        test::screenshot(window_, QStringLiteral("browser-folder-tree"), QRect(0, 0, 400, window_->height()));

        // Selecting a file previews it; Enter adds it; a drag carries its file.
        QSignalSpy previewed(&browser(), &BrowserController::previewRequested);
        QTest::keyClick(window_, Qt::Key_Down);
        QCOMPARE(tree()->property("currentIndex").toInt(), 1);
        QCOMPARE(previewed.size(), 1);
        QTest::keyClick(window_, Qt::Key_Return);
        QCOMPARE(project().tracks().size(), size_t(1));
        QCOMPARE(project().tracks()[0].clips[0].name, QStringLiteral("Kick Deep"));
        QVariant data;
        const QVariantList files{1, 2};
        QMetaObject::invokeMethod(panel_, "dragPayload", Q_RETURN_ARG(QVariant, data), Q_ARG(QVariant, files),
                                  Q_ARG(QVariant, true));
        const QString uris = data.toMap().value(QStringLiteral("text/uri-list")).toString();
        QVERIFY(uris.contains(QStringLiteral("Kick%20Deep.wav")) && uris.contains(QStringLiteral("Snare%20Tight.wav")));
        // Left goes to the folder, then closes it; a double-click opens it again.
        QTest::keyClick(window_, Qt::Key_Left);
        QCOMPARE(tree()->property("currentIndex").toInt(), 0);
        QTest::keyClick(window_, Qt::Key_Left);
        QTRY_COMPARE(folders->count(), 2);
        test::doubleClick(window_, rowPoint(tree(), 0));
        QTRY_COMPARE(folders->count(), 4);

        // A search on its way, and the folder shown again before it came: dropped.
        browser().setSearchText(QStringLiteral("kick"));
        browser().refresh();
        browser().setSearchText(QString());
        browser().refresh();
        QTest::qWait(100);
        QVERIFY(browser().showingTree() && !browser().searching());
        QVERIFY(tree()->isVisible());
    }

    // The menus: the sidebar's (a place: Remove from Places; Add Folder… asks
    // which); a preset's (Rename… in place, Delete after asking).
    void menusPlacesAndPresets() {
        QVariantList actions = browser().sidebarActions({QStringLiteral("place"), place()});
        QCOMPARE(actions.first().toMap().value(QStringLiteral("action")).toString(), QStringLiteral("removePlace"));
        auto* sidebarMenu = object("sidebarMenu");
        QMetaObject::invokeMethod(sidebarMenu, "popupWith", Q_ARG(QVariant, actions));
        QTRY_VERIFY(sidebarMenu->property("visible").toBool());
        QCOMPARE(sidebarMenu->property("count").toInt(), 3);
        test::screenshot(window_, QStringLiteral("browser-sidebar-menu"), QRect(0, 0, 400, window_->height()));
        QMetaObject::invokeMethod(sidebarMenu, "close");

        // Add Folder…: the folder dialog; the folder chosen is listed and shown.
        const int add = sidebarRow({QStringLiteral("add")});
        QQuickItem* entry = nullptr;
        QMetaObject::invokeMethod(item("sidebarList"), "itemAtIndex", Q_RETURN_ARG(QQuickItem*, entry), Q_ARG(int, add));
        test::click(window_, test::centerOf(entry));
        QTRY_VERIFY(object("placeDialog")->property("visible").toBool());
        QMetaObject::invokeMethod(object("placeDialog"), "close");
        QCOMPARE(browser().scope(), QStringList{QStringLiteral("samples")});  // (still)
        const QString other = dir_->path(QStringLiteral("Other"));
        QDir().mkpath(other);
        browser().addPlace(other);
        QCOMPARE(browser().scope(), (QStringList{QStringLiteral("place"), other}));
        const QStringList otherScope{QStringLiteral("place"), other};
        QMetaObject::invokeMethod(panel_, "runSidebarAction", Q_ARG(QVariant, QStringLiteral("removePlace")),
                                  Q_ARG(QVariant, otherScope));
        QCOMPARE(browser().places(), QStringList{place()});

        // A preset saved from a device is listed under Presets › its device.
        const QString track = session().insertAudioTrack();
        const QString device = session().editor()->addDevice(track, QStringLiteral("utility"));
        session().selection()->selectTrack(track, true);
        const QString saved = session().deviceSelection()->savePreset(device, QStringLiteral("Wide Utility"));
        QVERIFY(QFileInfo::exists(saved));
        browser().setScope({QStringLiteral("presets")});
        QTRY_VERIFY_WITH_TIMEOUT(results().total() == 1 && !browser().searching(), 5000);
        QCOMPARE(names(), QStringList{QStringLiteral("Wide Utility")});
        QCOMPARE(browser().resultActions(0).size(), 4);

        // Rename… in place: typed, Enter.
        QVERIFY(QMetaObject::invokeMethod(panel_, "runResultAction", Q_ARG(QVariant, QStringLiteral("renamePreset")),
                                          Q_ARG(QVariant, 0)));
        QQuickItem* field = nullptr;
        QTRY_VERIFY((field = renameField()) != nullptr);
        QTRY_VERIFY(field->hasActiveFocus());
        QCOMPARE(field->property("text").toString(), QStringLiteral("Wide Utility"));
        test::screenshot(window_, QStringLiteral("browser-preset-rename"), QRect(0, 0, 400, window_->height()));
        QMetaObject::invokeMethod(field, "selectAll");
        for (const char c : QByteArray("Narrow")) if (c) QTest::keyClick(window_, c);
        QTest::keyClick(window_, Qt::Key_Return);
        QTRY_COMPARE(names(), QStringList{QStringLiteral("Narrow")});
        QVERIFY(!QFileInfo::exists(saved));
        const QString renamed = results().get(0).value(QStringLiteral("path")).toString();
        QVERIFY(QFileInfo::exists(renamed));

        // ...and through Edit › Rename's way in (startRename), Esc cancels.
        QVariant started;
        QMetaObject::invokeMethod(panel_, "startRename", Q_RETURN_ARG(QVariant, started), Q_ARG(QVariant, renamed));
        QVERIFY(started.toBool());
        QTRY_VERIFY(renameField() != nullptr && renameField()->hasActiveFocus());
        QTest::keyClick(window_, Qt::Key_Escape);
        QTRY_VERIFY(renameField() == nullptr);
        QVERIFY(QFileInfo::exists(renamed));

        // Delete: asked first; No keeps it, Yes moves it to the trash.
        QMetaObject::invokeMethod(panel_, "runResultAction", Q_ARG(QVariant, QStringLiteral("deletePreset")),
                                  Q_ARG(QVariant, 0));
        auto* question = object("deletePresetQuestion");
        QTRY_VERIFY(question->property("visible").toBool());
        QCOMPARE(question->property("text").toString(), QStringLiteral("Move the preset “Narrow” to the Recycle Bin?"));
        QMetaObject::invokeMethod(question, "answer", Q_ARG(QVariant, QStringLiteral("no")));
        QVERIFY(QFileInfo::exists(renamed));
        QMetaObject::invokeMethod(panel_, "runResultAction", Q_ARG(QVariant, QStringLiteral("deletePreset")),
                                  Q_ARG(QVariant, 0));
        QTRY_VERIFY(question->property("visible").toBool());
        QMetaObject::invokeMethod(question, "answer", Q_ARG(QVariant, QStringLiteral("yes")));
        if (QFileInfo::exists(renamed))
            QSKIP("this system has no trash for the temporary folder");
        QTRY_VERIFY_WITH_TIMEOUT(results().total() == 0 && !browser().searching(), 5000);
    }
};

namespace {

const char* const kWindow = R"(
import QtQuick
import SUBstation

Window {
    width: 900
    height: 640
    color: Theme.window

    BrowserPanel {
        id: panel
        width: 400
        height: parent.height
    }
    // Somewhere else in the window (a press here stops a preview).
    Rectangle {
        objectName: "outside"
        x: 400
        width: 500
        height: parent.height
        color: Theme.emptyArea

        // Takes what is dropped from the browser (as the arrangement and the device view do).
        DropArea {
            objectName: "dropArea"
            anchors.fill: parent
            property var formats: []
            property string uris
            property string devices
            onDropped: drop => {
                formats = drop.formats
                uris = drop.getDataAsString("text/uri-list")
                devices = drop.getDataAsString("application/x-substation-device")
                drop.acceptProposedAction()
            }
        }
    }
}
)";

}  // namespace

QTEST_MAIN(TestUiBrowser)
#include "test_ui_browser.moc"
