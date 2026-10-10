// The File Manager panel (FileManagerPanel.qml) beside the browser panel, on
// a real session: its rows (the missing first) and summary, the missing
// files' box, the filter, a row's menu, Show in File Manager selecting a row,
// and hot swaps: a row's button starts one (lit while it runs), the browser
// shows its bar and lists the sounds most like the file, a sample clicked in
// the browser swaps in, and Esc in the browser, the bar's ✕, the button again
// or a press anywhere but the browser and the File Manager end it. Runs on a
// display. With $SUBSTATION_SCREENS set, it saves screenshots.

#include <QDir>
#include <QFileInfo>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QTest>
#include <QtQuickTest/quicktest.h>

#include <memory>
#include <utility>
#include <vector>

#include "UiTestSupport.h"
#include "browser/BrowserController.h"
#include "editor/ProjectEditor.h"
#include "files/FileManager.h"
#include "files/HotSwap.h"
#include "model/Project.h"

namespace test = sub::app::test;
using sub::app::Clip;

namespace {

// The window (at the end of the file: moc skips what follows a raw string).
extern const char* const kWindow;

std::vector<float> tone(double seconds) { return std::vector<float>(size_t(seconds * test::kSampleRate), 0.2f); }

}  // namespace

class TestUiFileManager : public QObject {
    Q_OBJECT

    std::unique_ptr<test::TempDir> dir_;
    std::unique_ptr<test::UiSession> ui_;
    QQuickWindow* window_ = nullptr;
    QList<QQmlError> warnings_;

    sub::app::Session& session() { return ui_->session(); }
    sub::app::FileManager& files() { return *session().files(); }
    sub::app::HotSwap& hotSwap() { return *session().hotSwap(); }
    QQuickItem* item(const char* name) { return window_->findChild<QQuickItem*>(QString::fromLatin1(name)); }
    // An item shown in the window, by its name (delegates too, which aren't the window's QObject children).
    static QQuickItem* shownItem(QQuickItem* root, const QString& name) {
        if (root->objectName() == name && root->isVisible()) return root;
        for (QQuickItem* child : root->childItems())
            if (QQuickItem* found = shownItem(child, name)) return found;
        return nullptr;
    }
    QString place() const { return dir_->path(QStringLiteral("Place")); }
    QString kick() const { return place() + QStringLiteral("/Kick.wav"); }
    QQuickItem* list() { return item("fileList"); }
    int rows() { return list()->property("count").toInt(); }
    QString text(const char* name) { return item(name)->property("text").toString(); }

    // An audio track playing `path` from `start` (half a second of it).
    QString clipTrack(const QString& path, const QString& name, double start = 0.0) {
        const QString track = session().editor()->addAudioTrack(-1, name);
        const Clip clip = Clip::audio(name + QStringLiteral("c"), path, QFileInfo(path).completeBaseName(), start, 0.5,
                                      0.0, 0.5);
        session().editor()->commitClips(QStringLiteral("Add"), {{track, {clip}}});
        files().update();
        return track;
    }
    // The point of a row of the list, in the window.
    QPoint rowPoint(int row) {
        QQuickItem* delegate = nullptr;
        QMetaObject::invokeMethod(list(), "itemAtIndex", Q_RETURN_ARG(QQuickItem*, delegate), Q_ARG(int, row));
        if (!delegate) return {};
        return test::at(delegate, QPointF(60, delegate->height() / 2));
    }

private Q_SLOTS:
    void initTestCase() {
        test::prepareApplication();
        if (!test::haveDisplay()) QSKIP("needs a display: Qt Quick's software renderer draws none of this geometry");
        dir_ = std::make_unique<test::TempDir>();
        QDir().mkpath(place());
        test::writeWav(kick(), tone(0.5));
        test::writeWav(place() + QStringLiteral("/Snare.wav"), tone(0.5));
        test::writeWav(place() + QStringLiteral("/Kick 2.wav"), tone(0.3));
        QSettings().setValue(QStringLiteral("browser/places"), QStringList{place()});
        sub::ui::setUpApplication();
        ui_ = std::make_unique<test::UiSession>();
        connect(&ui_->qml(), &QQmlEngine::warnings, this, [this](const QList<QQmlError>& list) { warnings_ += list; });
        window_ = ui_->show(kWindow);
        QVERIFY(window_);
    }

    void cleanupTestCase() { ui_.reset(); }

    void init() {
        hotSwap().stop();
        session().newProject();
        files().setFilter(QString());
        session().browser()->clearSimilar();
    }

    void cleanup() {
        const QList<QQmlError> warnings = std::exchange(warnings_, {});
        for (const QQmlError& warning : warnings) qWarning() << warning.toString();
        QVERIFY(warnings.isEmpty());
    }

    // The rows, the missing first; the box over them while files are missing; the filter; a row's menu.
    void rowsAndMissingFiles() {
        QVERIFY(!item("missingBox")->isVisible());
        clipTrack(kick(), QStringLiteral("A"));
        clipTrack(dir_->path(QStringLiteral("Gone/Missing.wav")), QStringLiteral("B"), 4.0);
        QTRY_COMPARE(rows(), 2);
        QCOMPARE(files().files()->get(0).value(QStringLiteral("name")).toString(), QStringLiteral("Missing.wav"));
        QVERIFY(item("missingBox")->isVisible());
        QCOMPARE(text("missingLabel"), QStringLiteral("1 file is missing"));
        QCOMPARE(text("fileSummary"), QStringLiteral("2 files, 1 missing"));
        QVERIFY(item("searchMissing")->isEnabled());
        test::screenshot(window_, QStringLiteral("file-manager"));

        // Typing filters the list.
        QQuickItem* filter = item("fileFilter");
        filter->forceActiveFocus();
        for (const char c : {'k', 'i', 'c', 'k'}) QTest::keyClick(window_, c);
        QCOMPARE(files().filter(), QStringLiteral("kick"));
        QTRY_COMPARE(rows(), 1);
        QMetaObject::invokeMethod(filter, "clear");
        files().setFilter(QString());
        QTRY_COMPARE(rows(), 2);

        // A missing file's menu has Locate….
        QTest::mouseClick(window_, Qt::RightButton, {}, rowPoint(0));
        QObject* menu = window_->findChild<QObject*>(QStringLiteral("fileMenu"));
        QVERIFY(menu);
        QTRY_VERIFY(menu->property("visible").toBool());
        QVERIFY(menu->findChild<QObject*>(QStringLiteral("menu_locate")));
        QVERIFY(menu->findChild<QObject*>(QStringLiteral("menu_hotSwap")));
        QVERIFY(!menu->findChild<QObject*>(QStringLiteral("menu_showInFolder")));
        QMetaObject::invokeMethod(menu, "close");
        QTRY_VERIFY(!menu->property("visible").toBool());
        QCOMPARE(list()->property("currentIndex").toInt(), 0);
    }

    // Show in File Manager: the file's row is current.
    void revealSelectsTheRow() {
        clipTrack(kick(), QStringLiteral("A"));
        clipTrack(place() + QStringLiteral("/Snare.wav"), QStringLiteral("B"));
        QTRY_COMPARE(rows(), 2);
        QVariant shown;
        QMetaObject::invokeMethod(item("fileManager"), "reveal", Q_RETURN_ARG(QVariant, shown),
                                  Q_ARG(QVariant, place() + QStringLiteral("/Snare.wav")));
        QVERIFY(shown.toBool());
        QCOMPARE(list()->property("currentIndex").toInt(), 1);
    }

    // A row's button hot-swaps its file: the browser's bar shows, the similar sounds are listed;
    // Esc in the browser, the bar's ✕ and the button again end it.
    // A sample clicked in the browser's list (let go of where it was pressed) swaps in.
    void aClickInTheBrowserSwaps() {
        const QString a = clipTrack(kick(), QStringLiteral("A"));
        sub::app::BrowserController& browser = *session().browser();
        QTRY_VERIFY(!browser.indexing() && browser.fileCount() >= 3);
        QVERIFY(hotSwap().startClip({a, QStringLiteral("Ac")}));
        browser.clearSimilar();
        browser.setSearchText(QStringLiteral("Kick 2"));
        QTRY_VERIFY(!browser.searching());
        QQuickItem* results = item("resultsList");
        QTRY_COMPARE(results->property("count").toInt(), 1);
        QQuickItem* row = nullptr;
        QTRY_VERIFY(QMetaObject::invokeMethod(results, "itemAtIndex", Q_RETURN_ARG(QQuickItem*, row), Q_ARG(int, 0)) && row);
        test::click(window_, test::at(row, QPointF(60, row->height() / 2)));
        QCOMPARE(session().project()->clip(a, QStringLiteral("Ac")).path, place() + QStringLiteral("/Kick 2.wav"));
        QVERIFY(hotSwap().active());
        browser.setSearchText(QString());
    }

    // A press in the File Manager keeps a hot swap going; one anywhere else ends it.
    void pressesElsewhereEndIt() {
        clipTrack(kick(), QStringLiteral("A"));
        QTRY_COMPARE(rows(), 1);
        QVERIFY(files().hotSwap(kick()));
        test::click(window_, test::at(item("fileSummary"), QPointF(2, 2)));
        QVERIFY(hotSwap().active());
        test::click(window_, test::centerOf(item("outside")));
        QVERIFY(!hotSwap().active());
    }

    void hotSwapFromARow() {
        const QString a = clipTrack(kick(), QStringLiteral("A"));
        QTRY_COMPARE(rows(), 1);
        QQuickItem* button = shownItem(window_->contentItem(), QStringLiteral("hotSwapButton"));
        QVERIFY(button);
        test::click(window_, test::centerOf(button));
        QVERIFY(hotSwap().active());
        QCOMPARE(session().browser()->similarTo(), kick());
        QVERIFY(button->property("lit").toBool());
        QQuickItem* bar = item("hotSwapBar");
        QTRY_VERIFY(bar->isVisible());
        QCOMPARE(text("hotSwapLabel"), QStringLiteral("Hot-Swap Kick  (1 clip)"));
        test::screenshot(window_, QStringLiteral("file-manager-hot-swap"));
        // What the user chooses in the browser swaps in.
        session().browser()->chooseFile(place() + QStringLiteral("/Kick 2.wav"));
        QCOMPARE(session().project()->clip(a, QStringLiteral("Ac")).path, place() + QStringLiteral("/Kick 2.wav"));
        QTRY_COMPARE(text("hotSwapLabel"), QStringLiteral("Hot-Swap Kick 2  (1 clip)"));
        // Esc in the browser's list ends it.
        item("resultsList")->forceActiveFocus();
        QTest::keyClick(window_, Qt::Key_Escape);
        QVERIFY(!hotSwap().active());
        QTRY_VERIFY(!bar->isVisible());
        button = shownItem(window_->contentItem(), QStringLiteral("hotSwapButton"));  // (the list was made again)
        QVERIFY(button && !button->property("lit").toBool());

        // The bar's ✕, and the row's button again.
        session().undoStack()->undo();
        files().update();
        button = shownItem(window_->contentItem(), QStringLiteral("hotSwapButton"));
        test::click(window_, test::centerOf(button));
        QVERIFY(hotSwap().active());
        QTRY_VERIFY(bar->isVisible());
        // (Laid out again first: until then the similar sounds' bar, which took this bar's place while no hot swap
        // ran, still lies over it, with its own ✕ (Back to the list) about where this one is.)
        QVERIFY(QQuickTest::qWaitForPolish(window_));
        test::click(window_, test::centerOf(item("stopHotSwap")));
        QVERIFY(!hotSwap().active());
        test::click(window_, test::centerOf(button));
        QVERIFY(hotSwap().active());
        test::click(window_, test::centerOf(button));
        QVERIFY(!hotSwap().active());
    }
};

namespace {

const char* const kWindow = R"(
import QtQuick
import SUBstation

Window {
    width: 900
    height: 700
    color: Theme.window

    BrowserPanel {
        id: browser
        width: 500
        height: 640
        hotSwapKeepers: [files]
    }
    FileManagerPanel {
        id: files
        x: 500
        width: 400
        height: 640
    }
    // Somewhere else in the window (a press here ends a hot swap).
    Rectangle {
        objectName: "outside"
        y: 640
        width: parent.width
        height: 60
        color: Theme.emptyArea
    }
}
)";

}  // namespace

QTEST_MAIN(TestUiFileManager)
#include "test_ui_file_manager.moc"
