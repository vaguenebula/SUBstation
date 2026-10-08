// The main window (Main.qml) on a real session (the views have tests of their
// own): the layout and its look, every menu action calling the session, the
// shortcuts, the checkable actions kept in step with the model, Open Recent,
// the files' flows with the unsaved-changes question, closing (a render
// running, unsaved changes), the title bar (the title, the status line, what
// drags the window, its buttons, the frame), the info view saying the
// tooltips, the session's warnings,
// Export Audio, the clip view over the arrangement, nothing scrolled out of
// the arrangement or the piano roll drawn over the browser, Edit › Rename, the
// window's place kept, and the rules of the shortcuts taken from plug-ins'
// editors. Runs on a display (xvfb here). With $SUBSTATION_SCREENS set, it
// saves screenshots there.

#include <QDir>
#include <QFileInfo>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScreen>
#include <QSettings>
#include <QSignalSpy>
#include <QTest>
#include <QUndoStack>

#include <cmath>
#include <memory>
#include <utility>
#include <vector>

#include "UiTestSupport.h"
#include "arrangement/Arrangement.h"
#include "audio/EngineBridge.h"
#include "browser/BrowserController.h"
#include "editor/ProjectEditor.h"
#include "intelligence/Harmony.h"
#include "model/Project.h"
#include "mainwindow/WindowState.h"
#include "pianoroll/PianoRoll.h"
#include "platform/PluginEditorKeys.h"
#include "platform/WindowFrame.h"
#include "session/ArrangementActions.h"
#include "session/ComputerKeyboard.h"
#include "session/RenderProgress.h"
#include "session/Selection.h"
#include "theme/Theme.h"

namespace test = sub::app::test;
using sub::ui::Theme;

namespace {

// The window (at the end of the file: moc skips what follows a raw string).
extern const char* const kWindow;

bool near(QRgb pixel, const QColor& color, int tolerance = 6) {
    return std::abs(qRed(pixel) - color.red()) <= tolerance && std::abs(qGreen(pixel) - color.green()) <= tolerance &&
           std::abs(qBlue(pixel) - color.blue()) <= tolerance;
}

// Windows' virtual-key codes the plug-in editors' keys come as.
constexpr int kVkSpace = 0x20;
constexpr int kVkDelete = 0x2E;
int vk(char letter) { return int(letter); }

}  // namespace

class TestUiMainWindow : public QObject {
    Q_OBJECT

    std::unique_ptr<test::TempDir> dir_;
    std::unique_ptr<test::UiSession> ui_;
    QQuickWindow* window_ = nullptr;
    QList<QQmlError> warnings_;

    sub::app::Session& session() { return ui_->session(); }
    sub::app::Project& project() { return *session().project(); }
    sub::app::ProjectEditor& editor() { return *session().editor(); }
    sub::app::Selection& selection() { return *session().selection(); }
    sub::app::EngineBridge& bridge() { return *session().bridge(); }
    QUndoStack& undo() { return *session().undoStack(); }

    // The object of this name (an Action before an item of the same name: the
    // transport's Loop button and Edit › Loop...).
    QObject* find(const QString& name) {
        const QList<QObject*> found = window_->findChildren<QObject*>(name);
        for (QObject* object : found)
            if (object->inherits("QQuickAction")) return object;
        return found.isEmpty() ? nullptr : found.front();
    }
    QQuickItem* item(const QString& name) { return window_->findChild<QQuickItem*>(name); }
    QVariant prop(const QString& name, const char* property) { return find(name)->property(property); }

    // Triggers a menu action, as clicking it would.
    void trigger(const QString& name) {
        QObject* action = find(name);
        QVERIFY2(action, qPrintable(name));
        QVERIFY(QMetaObject::invokeMethod(action, "trigger"));
    }
    void key(Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) { QTest::keyClick(window_, key, modifiers); }
    QString status() { return prop(QStringLiteral("statusText"), "text").toString(); }
    bool shown(const QString& dialog) { return prop(dialog, "visible").toBool(); }
    // Answers a message box (its value: "save", "discard", "cancel", "ok"...).
    void answer(const QString& dialog, const QString& value) {
        QVERIFY(shown(dialog));
        QVERIFY(QMetaObject::invokeMethod(find(dialog), "answer", Q_ARG(QVariant, value)));
        QTRY_VERIFY(!shown(dialog));
    }
    QVariant call(const char* function, const QVariant& a = QVariant(), const QVariant& b = QVariant()) {
        QVariant result;
        if (!b.isValid() && !a.isValid())
            QMetaObject::invokeMethod(window_, function, Q_RETURN_ARG(QVariant, result));
        else if (!b.isValid())
            QMetaObject::invokeMethod(window_, function, Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, a));
        else
            QMetaObject::invokeMethod(window_, function, Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, a),
                                      Q_ARG(QVariant, b));
        return result;
    }

    // A stereo file of `seconds` of a quiet tone, decoded by the bridge.
    QString wav(const QString& name, double seconds) {
        const int frames = int(seconds * test::kSampleRate);
        std::vector<float> samples(size_t(2 * frames), 0.25f);
        const QString path = test::writeWav(dir_->path(name), samples, 2);
        bridge().requestSource(path);
        return path;
    }
    // An audio track playing `path` from `startBeat`; its id.
    QString audioTrack(const QString& path, double seconds, double startBeat = 0.0) {
        const sub::app::ClipRefs refs =
            editor().addClips(QString(), startBeat, {{path, seconds}}, int(project().tracks().size()));
        return refs.isEmpty() ? QString() : refs.front().trackId;
    }

private Q_SLOTS:
    void initTestCase() {
        test::prepareApplication();
        if (!test::haveDisplay()) QSKIP("needs a display: Qt Quick's software renderer draws none of this geometry");
        dir_ = std::make_unique<test::TempDir>();
        QDir().mkpath(dir_->path(QStringLiteral("place")));
        QSettings().setValue(QStringLiteral("browser/places"), QStringList{dir_->path(QStringLiteral("place"))});
        sub::ui::setUpApplication();
        ui_ = std::make_unique<test::UiSession>();
        connect(&ui_->qml(), &QQmlEngine::warnings, this, [this](const QList<QQmlError>& list) { warnings_ += list; });
        window_ = ui_->show(kWindow);
        QVERIFY(window_);
        QTest::qWait(100);
    }

    void cleanupTestCase() { ui_.reset(); }

    void init() {
        session().newProject();
        session().computerKeyboard()->setEnabled(false);
        window_->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window_));
    }

    void cleanup() {
        const QList<QQmlError> warnings = std::exchange(warnings_, {});
        for (const QQmlError& warning : warnings) qWarning() << warning.toString();
        QVERIFY(warnings.isEmpty());
    }

    // The layout, in the theme's colours: the title bar on top, the transport
    // under it, the browser beside the arrangement, and along the window's
    // bottom (no status bar), as in Ableton, the info view at the left and the
    // device view across the rest; nothing said in the status line yet.
    void layoutAndLook() {
        auto* titleBar = item(QStringLiteral("titleBar"));
        auto* browser = item(QStringLiteral("browser"));
        auto* arrangement = item(QStringLiteral("arrangement"));
        auto* devices = item(QStringLiteral("devicePanel"));
        auto* info = item(QStringLiteral("infoView"));
        auto* transport = item(QStringLiteral("transportBar"));
        QVERIFY(titleBar && browser && arrangement && devices && info && transport);
        const QRectF tb = titleBar->mapRectToScene(titleBar->boundingRect());
        const QRectF b = browser->mapRectToScene(browser->boundingRect());
        const QRectF a = arrangement->mapRectToScene(arrangement->boundingRect());
        const QRectF d = devices->mapRectToScene(devices->boundingRect());
        const QRectF i = info->mapRectToScene(info->boundingRect());
        const QRectF t = transport->mapRectToScene(transport->boundingRect());
        QCOMPARE(tb, QRectF(0, 0, window_->width(), 32));
        QCOMPARE(t.top(), tb.bottom());
        QCOMPARE(t.height(), 40.0);
        QVERIFY(t.bottom() <= b.top() + 1);
        QVERIFY(b.right() < a.left());
        QCOMPARE(b.width(), 300.0);
        QCOMPARE(a.bottom(), b.bottom());
        QVERIFY(b.bottom() < i.top() && a.bottom() < d.top());
        QCOMPARE(i.left(), 8.0);  // (the device view's margin)
        QCOMPARE(i.top(), d.top() + 8);
        QCOMPARE(d.left(), i.right());
        QCOMPARE(d.right(), qreal(window_->width()));
        QCOMPARE(d.bottom(), qreal(window_->height()));
        QCOMPARE(status(), QString());
        QCOMPARE(window_->title(), QStringLiteral("Untitled - SUBstation"));

        const QImage image = window_->grabWindow();
        test::screenshot(window_, QStringLiteral("main-window"));
        const qreal dpr = window_->effectiveDevicePixelRatio();
        auto pixel = [&](qreal x, qreal y) { return image.pixel(int(x * dpr), int(y * dpr)); };
        auto* windowTitle = item(QStringLiteral("windowTitle"));
        const QRectF title = windowTitle->mapRectToScene(windowTitle->boundingRect());
        QVERIFY(near(pixel(title.left() - 8, 5), Theme::kPanel));                 // the title bar
        QVERIFY(near(pixel(tb.center().x(), tb.bottom() - 1), Theme::kBorder));   // its line
        QVERIFY(near(pixel(t.center().x(), t.bottom() - 1), Theme::kBorder));     // the transport's line
        QVERIFY(near(pixel((b.right() + a.left()) / 2, a.center().y()), Theme::kBorder));  // a split handle
        // The real views are in: the arrangement's interface is there.
        QVERIFY(item(QStringLiteral("arrangement"))->metaObject()->indexOfMethod("zoomToArrangement()") >= 0);

        // The menus, open (with the shortcuts on the right): File clicked, the
        // others as the mouse moves over them.
        auto* bar = item(QStringLiteral("menuBar"));
        QVERIFY(bar);
        static const char* names[] = {"file", "edit", "create", "view", "options"};
        for (int menu = 0; menu < 5; ++menu) {
            QQuickItem* title = nullptr;
            QMetaObject::invokeMethod(bar, "itemAt", Q_RETURN_ARG(QQuickItem*, title), Q_ARG(int, menu));
            QVERIFY(title);
            const QPoint at = test::centerOf(title);
            if (menu == 0)
                test::click(window_, at);
            else
                QTest::mouseMove(window_, at);
            QTest::qWait(250);
            const QImage open = window_->grabWindow();
            QVERIFY2(near(open.pixel(int((at.x() + 20) * dpr), int((at.y() + 40) * dpr)), Theme::kPanelAlt) ||
                         near(open.pixel(int((at.x() + 20) * dpr), int((at.y() + 40) * dpr)), Theme::kAccent),
                     names[menu]);
            test::screenshot(window_, QStringLiteral("main-window-%1-menu").arg(QLatin1String(names[menu])));
        }
        key(Qt::Key_Escape);
        QTest::qWait(150);
    }

    // The title says the project's name, and * while it has unsaved changes.
    void titleFollowsTheProject() {
        QCOMPARE(window_->title(), QStringLiteral("Untitled - SUBstation"));
        trigger(QStringLiteral("insertAudioTrack"));
        QCOMPARE(window_->title(), QStringLiteral("Untitled* - SUBstation"));
        QVERIFY(session().saveProjectAs(dir_->path(QStringLiteral("song.gilproj"))));
        QCOMPARE(window_->title(), QStringLiteral("song - SUBstation"));
        trigger(QStringLiteral("insertMidiTrack"));
        QCOMPARE(window_->title(), QStringLiteral("song* - SUBstation"));
        trigger(QStringLiteral("undo"));
        QCOMPARE(window_->title(), QStringLiteral("song - SUBstation"));
        QCOMPARE(prop(QStringLiteral("windowTitle"), "text").toString(), window_->title());  // the title bar's
    }

    // The title bar: the title in the middle of the window; its empty parts, the
    // title and the status line drag the window, the menus and the buttons are
    // the window's. On Windows it is the window's only title bar: the system's
    // caption is gone (the window's top is its client area's), the window's
    // buttons are there, and maximized it starts at the screen's top.
    void titleBar() {
        auto* frame = window_->findChild<sub::ui::WindowFrame*>();
        QVERIFY(frame);
        auto* bar = item(QStringLiteral("titleBar"));
        auto* title = item(QStringLiteral("windowTitle"));
        auto* menus = item(QStringLiteral("menuBar"));
        QVERIFY(bar && title && menus);
        const QRectF t = title->mapRectToScene(title->boundingRect());
        QVERIFY(std::abs(t.center().x() - window_->width() / 2.0) <= 1);
        QVERIFY(t.left() > menus->mapRectToScene(menus->boundingRect()).right());

        using Hit = sub::ui::WindowFrame::Hit;
        Q_EMIT session().statusMessage(QStringLiteral("Hello"));
        auto* status = item(QStringLiteral("statusText"));
        const QRectF said = status->mapRectToScene(status->boundingRect());
        QCOMPARE(frame->hitTest(t.center()), Hit::Caption);
        QCOMPARE(frame->hitTest(QPointF(t.left() - 10, 16)), Hit::Caption);             // empty
        QCOMPARE(frame->hitTest(QPointF(said.right() - 5, said.center().y())), Hit::Caption);  // "Hello"
        QQuickItem* file = nullptr;
        QMetaObject::invokeMethod(menus, "itemAt", Q_RETURN_ARG(QQuickItem*, file), Q_ARG(int, 0));
        QVERIFY(file);
        QCOMPARE(frame->hitTest(file->mapToScene(QPointF(file->width() / 2, file->height() / 2))), Hit::Client);
        QCOMPARE(frame->hitTest(QPointF(t.center().x(), 50)), Hit::Client);  // the transport bar
        // While a menu is open, a press there is the window's: it closes the menu.
        QVERIFY(frame->overlay());
        test::click(window_, test::centerOf(file));
        QTRY_VERIFY(frame->overlay()->isVisible());
        QCOMPARE(frame->hitTest(t.center()), Hit::Client);
        key(Qt::Key_Escape);
        QTRY_VERIFY(!frame->overlay()->isVisible());
        QCOMPARE(frame->hitTest(t.center()), Hit::Caption);

        auto* buttons = item(QStringLiteral("captionButtons"));
        QVERIFY(buttons);
        if (!frame->active()) {  // (not Windows: the system's title bar has them)
            QVERIFY(!buttons->isVisible());
            return;
        }
        QVERIFY(buttons->isVisible());
        QCOMPARE(window_->frameGeometry().top(), window_->geometry().top());  // no caption above it
        auto* maximize = item(QStringLiteral("maximizeButton"));
        auto* close = item(QStringLiteral("closeButton"));
        QCOMPARE(frame->hitTest(maximize->mapToScene(QPointF(maximize->width() / 2, 16))), Hit::MaximizeButton);
        QCOMPARE(frame->hitTest(close->mapToScene(QPointF(close->width() / 2, 16))), Hit::Client);
        QCOMPARE(frame->hitTest(QPointF(window_->width() - 2, 16)), Hit::Client);

        // Maximized: the title bar is on the screen; then back where it was.
        const QRect normal = window_->geometry();
        frame->toggleMaximized();
        QTRY_VERIFY(window_->windowStates() & Qt::WindowMaximized);
        QTRY_COMPARE(window_->geometry().top(), window_->screen()->availableGeometry().top());
        test::screenshot(window_, QStringLiteral("main-window-title-bar-maximized"), QRect(0, 0, window_->width(), 32));
        frame->toggleMaximized();
        QTRY_VERIFY(!(window_->windowStates() & Qt::WindowMaximized));
        QTRY_COMPARE(window_->geometry(), normal);
    }

    // The tooltips are said in the info view (at the bottom left, as in
    // Ableton), at once, and don't pop up; with the info view hidden (View ›
    // Info View), they pop up again.
    void tooltipsInTheInfoView() {
        auto* frame = window_->findChild<sub::ui::WindowFrame*>();
        auto* title = item(QStringLiteral("infoTitle"));
        auto* text = item(QStringLiteral("infoText"));
        auto* metronome = item(QStringLiteral("metronome"));
        QVERIFY(frame && frame->overlay() && title && text && metronome);
        // Whether a tip pops up (a popup that takes room).
        auto popped = [&] {
            for (QQuickItem* popup : frame->overlay()->childItems())
                if (popup->isVisible() && popup->width() > 0 && popup->height() > 0) return true;
            return false;
        };
        const QPoint nowhere(window_->width() / 2, window_->height() / 3);  // (the arrangement's lanes)
        QTest::mouseMove(window_, nowhere);
        QTRY_COMPARE(title->property("text").toString(), QStringLiteral("Info"));
        QTest::mouseMove(window_, test::centerOf(metronome));
        QTRY_COMPARE(title->property("text").toString(), QStringLiteral("Metronome"));
        QCOMPARE(text->property("text").toString(), QString());
        QTest::qWait(900);  // (longer than a tip's delay)
        QVERIFY(!popped());
        test::screenshot(window_, QStringLiteral("main-window-info-view"));
        QTest::mouseMove(window_, nowhere);
        QTRY_COMPARE(title->property("text").toString(), QStringLiteral("Info"));

        trigger(QStringLiteral("infoView"));
        QVERIFY(!item(QStringLiteral("infoView"))->isVisible());
        QTest::mouseMove(window_, test::centerOf(metronome));
        QTRY_VERIFY(popped());
        QTest::mouseMove(window_, nowhere);
        QTRY_VERIFY(!popped());
        trigger(QStringLiteral("infoView"));
        QVERIFY(item(QStringLiteral("infoView"))->isVisible());
    }

    // Create's actions, Undo and Redo (their texts and enabled states).
    void createUndoAndRedo() {
        QVERIFY(!prop(QStringLiteral("undo"), "enabled").toBool());
        QCOMPARE(prop(QStringLiteral("undo"), "text").toString(), QStringLiteral("&Undo"));
        trigger(QStringLiteral("insertAudioTrack"));
        QCOMPARE(project().tracks().size(), size_t(1));
        QCOMPARE(selection().trackId(), project().tracks()[0].id);  // (the new track is selected)
        QVERIFY(prop(QStringLiteral("undo"), "enabled").toBool());
        QCOMPARE(prop(QStringLiteral("undo"), "text").toString(), QStringLiteral("&Undo ") + undo().undoText());
        trigger(QStringLiteral("insertMidiTrack"));
        QCOMPARE(project().tracks().size(), size_t(2));
        QVERIFY(project().tracks()[1].isMidi());
        trigger(QStringLiteral("insertReturnTrack"));
        QCOMPARE(project().returns().size(), size_t(1));

        trigger(QStringLiteral("undo"));
        QCOMPARE(project().returns().size(), size_t(0));
        QVERIFY(prop(QStringLiteral("redo"), "enabled").toBool());
        QCOMPARE(prop(QStringLiteral("redo"), "text").toString(), QStringLiteral("&Redo ") + undo().redoText());
        trigger(QStringLiteral("redo"));
        QCOMPARE(project().returns().size(), size_t(1));
        QVERIFY(!prop(QStringLiteral("redo"), "enabled").toBool());

        // Groups: the selected tracks, then ungrouped; Delete Selected Tracks.
        selection().selectTrack(project().tracks()[0].id, true);
        selection().selectTrack(project().tracks()[1].id, false, sub::app::Selection::Mode::Toggle);
        trigger(QStringLiteral("groupTracks"));
        QCOMPARE(project().tracks().size(), size_t(3));
        QVERIFY(project().tracks()[0].isGroup());
        trigger(QStringLiteral("ungroupTracks"));
        QCOMPARE(project().tracks().size(), size_t(2));
        selection().selectTrack(project().tracks()[1].id, true);
        trigger(QStringLiteral("deleteSelectedTracks"));
        QCOMPARE(project().tracks().size(), size_t(1));

        // Insert MIDI Clip with no MIDI track selected: the session says why.
        selection().selectTrack(project().tracks()[0].id, true);
        trigger(QStringLiteral("insertMidiClip"));
        QCOMPARE(status(), QStringLiteral("Select a MIDI track (or a time range on one) to insert a MIDI clip."));
    }

    // Edit's commands on clips (test_edit_commands): split, select all,
    // duplicate, delete; solo; freeze; automation.
    void editCommands() {
        const QString path = wav(QStringLiteral("tone.wav"), 1.0);
        const QString track = audioTrack(path, 1.0);
        QVERIFY(!track.isEmpty());
        const QString clip = project().track(track).clips[0].id;
        selection().selectClips(editor(), {sub::app::ClipRef{track, clip}}, track);
        selection().setInsert(1.0);
        trigger(QStringLiteral("split"));
        QCOMPARE(project().track(track).clips.size(), size_t(2));
        trigger(QStringLiteral("selectAll"));
        QCOMPARE(selection().clips().size(), 2);
        trigger(QStringLiteral("copy"));
        QVERIFY(session().arrangement()->hasClipboard());
        trigger(QStringLiteral("duplicate"));
        QCOMPARE(project().track(track).clips.size(), size_t(4));
        trigger(QStringLiteral("delete"));
        QCOMPARE(project().track(track).clips.size(), size_t(2));

        selection().selectTrack(track, true);
        trigger(QStringLiteral("soloSelectedTracks"));
        QVERIFY(project().track(track).solo);
        trigger(QStringLiteral("soloSelectedTracks"));
        QVERIFY(!project().track(track).solo);

        trigger(QStringLiteral("automation"));
        QVERIFY(project().automationView(track).shown);
        trigger(QStringLiteral("automation"));
        QVERIFY(!project().automationView(track).shown);

        QVERIFY(!prop(QStringLiteral("reEnableAutomation"), "enabled").toBool());  // (nothing overridden)
        QVERIFY(window_->title().startsWith(QStringLiteral("Untitled*")));
    }

    // Ctrl+Shift+F freezes the selected track in the background (its dialog
    // shows meanwhile), and unfreezes it (test_ctrl_shift_f_freezes_and_unfreezes...).
    void freezeAndUnfreeze() {
        const QString track = audioTrack(wav(QStringLiteral("freeze.wav"), 1.0), 1.0);
        editor().addDevice(track, QStringLiteral("utility"));
        selection().selectTrack(track, true);
        key(Qt::Key_F, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(shown(QStringLiteral("renderDialog")) || project().track(track).frozen.has_value());
        QTRY_VERIFY_WITH_TIMEOUT(project().track(track).frozen.has_value(), 30000);
        QTRY_VERIFY(!shown(QStringLiteral("renderDialog")));
        QVERIFY(status().startsWith(QStringLiteral("Froze ")));
        QCOMPARE(undo().undoText(), QStringLiteral("Freeze Track"));  // (one undo step)
        key(Qt::Key_F, Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY(!project().track(track).frozen.has_value());
        QVERIFY(status().startsWith(QStringLiteral("Unfroze ")));
        trigger(QStringLiteral("undo"));
        QVERIFY(project().track(track).frozen.has_value());
        trigger(QStringLiteral("flatten"));
        QVERIFY(!project().track(track).frozen.has_value());
        QVERIFY(status().startsWith(QStringLiteral("Flattened ")));
    }

    // Transport: Play / Stop, Go to Start, Record with nothing armed; their shortcuts.
    void transportActions() {
        selection().setInsert(4.0);
        trigger(QStringLiteral("playStop"));
        QVERIFY(bridge().isPlaying());
        trigger(QStringLiteral("playStop"));
        QVERIFY(!bridge().isPlaying());
        QCOMPARE(bridge().position(), 4.0);  // back where playback started
        trigger(QStringLiteral("goToStart"));
        QCOMPARE(bridge().position(), 0.0);
        QCOMPARE(selection().insertBeat(), 0.0);
        key(Qt::Key_Space);
        QVERIFY(bridge().isPlaying());
        key(Qt::Key_Space);
        QVERIFY(!bridge().isPlaying());
        key(Qt::Key_F9);  // nothing armed: the bridge says so
        QVERIFY(!bridge().isRecording());
        QVERIFY(!status().isEmpty() && status() != QStringLiteral("Ready"));
    }

    // Checkable actions follow the model, whoever changes it.
    void checkableActionsKeptInStep() {
        trigger(QStringLiteral("loop"));
        QVERIFY(project().loopEnabled());
        QVERIFY(prop(QStringLiteral("loop"), "checked").toBool());
        editor().setLoopEnabled(false);
        QVERIFY(!prop(QStringLiteral("loop"), "checked").toBool());
        key(Qt::Key_L, Qt::ControlModifier);
        QVERIFY(project().loopEnabled() && prop(QStringLiteral("loop"), "checked").toBool());
        undo().undo();
        QVERIFY(!prop(QStringLiteral("loop"), "checked").toBool());

        trigger(QStringLiteral("lockEnvelopes"));
        QVERIFY(project().automationLocked() && prop(QStringLiteral("lockEnvelopes"), "checked").toBool());
        editor().setAutomationLocked(false);
        QVERIFY(!prop(QStringLiteral("lockEnvelopes"), "checked").toBool());

        trigger(QStringLiteral("computerMidiKeyboard"));
        QVERIFY(session().computerKeyboard()->enabled());
        QVERIFY(prop(QStringLiteral("computerMidiKeyboard"), "checked").toBool());
        session().computerKeyboard()->setEnabled(false);
        QVERIFY(!prop(QStringLiteral("computerMidiKeyboard"), "checked").toBool());
        key(Qt::Key_M);
        QVERIFY(session().computerKeyboard()->enabled());
        key(Qt::Key_M);
        QVERIFY(!session().computerKeyboard()->enabled());

        // Record Quantization: one checked, the session's.
        const QVariantList choices = session().recordQuantizeChoices();
        QVERIFY(choices.size() >= 5);
        auto checkedOne = [&] {
            int checked = -1;
            for (int i = 0; i < choices.size(); ++i)
                if (prop(QStringLiteral("quantize_%1").arg(i), "checked").toBool()) {
                    if (checked >= 0) return -2;  // (two)
                    checked = i;
                }
            return checked;
        };
        QCOMPARE(checkedOne(), 0);
        trigger(QStringLiteral("quantize_4"));
        QCOMPARE(session().recordQuantize(), choices[4].toMap().value(QStringLiteral("value")).toDouble());
        QCOMPARE(checkedOne(), 4);
        session().setRecordQuantize(choices[2].toMap().value(QStringLiteral("value")).toDouble());
        QCOMPARE(checkedOne(), 2);
        trigger(QStringLiteral("quantize_0"));
        QCOMPARE(session().recordQuantize(), 0.0);
        QCOMPARE(checkedOne(), 0);
        QCOMPARE(prop(QStringLiteral("quantize_0"), "text").toString(), QStringLiteral("No Quantization"));

        // Snap to Grid: checked while the arrangement snaps (the placeholder: as at first).
        QVERIFY(prop(QStringLiteral("snapToGrid"), "checked").toBool());
    }

    // View › Browser and Device View show and hide their panes; Find in Browser shows it, searching all.
    void viewActions() {
        auto* browser = item(QStringLiteral("browser"));
        auto* area = item(QStringLiteral("devicePanel"));
        QVERIFY(browser->isVisible() && area->isVisible());
        key(Qt::Key_B, Qt::ControlModifier | Qt::AltModifier);
        QVERIFY(!browser->isVisible());
        QVERIFY(!prop(QStringLiteral("browser"), "visible").toBool() || !browser->isVisible());
        key(Qt::Key_F, Qt::ControlModifier);  // Find in Browser shows it again
        QVERIFY(browser->isVisible());
        QCOMPARE(session().browser()->scope(), QStringList{QStringLiteral("all")});
        auto* search = item(QStringLiteral("searchField"));
        QTRY_VERIFY(search->hasActiveFocus());
        // Typing there goes into the field, not to the window's one-key shortcuts.
        trigger(QStringLiteral("insertAudioTrack"));
        search->forceActiveFocus();
        QTest::keyClick(window_, Qt::Key_S);
        QCOMPARE(search->property("text").toString(), QStringLiteral("s"));
        QVERIFY(!project().tracks()[0].solo);
        QMetaObject::invokeMethod(search, "clear");
        session().browser()->setSearchText(QString());
        // Find Similar (from an audio clip's menu) shows it too.
        key(Qt::Key_B, Qt::ControlModifier | Qt::AltModifier);
        QVERIFY(!browser->isVisible());
        session().browser()->findSimilar(QStringLiteral("C:/nowhere/kick.wav"));
        QVERIFY(browser->isVisible());
        session().browser()->clearSimilar();
        session().browser()->setScope({QStringLiteral("all")});
        item(QStringLiteral("browser"))->setFocus(false);
        window_->contentItem()->forceActiveFocus();

        key(Qt::Key_L, Qt::ControlModifier | Qt::AltModifier);
        QVERIFY(!area->isVisible());
        key(Qt::Key_L, Qt::ControlModifier | Qt::AltModifier);
        QVERIFY(area->isVisible());

        // Zooming and the grid reach the arrangement through its interface: with
        // the placeholder there is none, and nothing goes wrong.
        for (const char* name : {"zoomIn", "zoomOut", "zoomToArrangement", "narrowGrid", "widenGrid", "snapToGrid", "clipView"})
            trigger(QString::fromLatin1(name));
        // Close Plug-in Editor: plug-in editors are Win32 windows.
        QCOMPARE(find(QStringLiteral("closePluginEditor"))->property("enabled").toBool(),
                 sub::ui::PluginEditorKeys::supported());
    }

    // Help › About, Options › Preferences, File › Export Audio open their dialogs.
    void dialogsOpen() {
        trigger(QStringLiteral("about"));
        QTRY_VERIFY(shown(QStringLiteral("aboutDialog")));
        QCOMPARE(prop(QStringLiteral("aboutDialog"), "title").toString(), QStringLiteral("About SUBstation"));
        QVERIFY(prop(QStringLiteral("aboutDialog"), "text").toString().contains(QStringLiteral("VST is a registered trademark")));
        test::screenshot(window_, QStringLiteral("about-dialog"));
        answer(QStringLiteral("aboutDialog"), QStringLiteral("ok"));

        key(Qt::Key_Comma, Qt::ControlModifier);
        QTRY_VERIFY(shown(QStringLiteral("preferencesDialog")));
        QMetaObject::invokeMethod(find(QStringLiteral("preferencesDialog")), "close");
        QTRY_VERIFY(!shown(QStringLiteral("preferencesDialog")));

        key(Qt::Key_R, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(shown(QStringLiteral("exportDialog")));
        QMetaObject::invokeMethod(find(QStringLiteral("exportDialog")), "close");
        QTRY_VERIFY(!shown(QStringLiteral("exportDialog")));
    }

    // The title bar's status line: the session's messages (for 8 s); a project's plug-ins loading after it.
    void statusLine() {
        Q_EMIT session().statusMessage(QStringLiteral("Hello"));
        QCOMPARE(status(), QStringLiteral("Hello"));
        QCOMPARE(prop(QStringLiteral("messageTimer"), "interval").toInt(), session().statusTimeout());
        auto* loading = item(QStringLiteral("pluginsLoading"));
        QVERIFY(!loading->isVisible());
        Q_EMIT bridge().pluginsLoading(1, 3);
        QVERIFY(loading->isVisible());
        QCOMPARE(prop(QStringLiteral("pluginsLabel"), "text").toString(), QStringLiteral("Loading plug-ins: 1 of 3"));
        QCOMPARE(prop(QStringLiteral("pluginsBar"), "value").toDouble(), 1.0);
        QCOMPARE(prop(QStringLiteral("pluginsBar"), "to").toDouble(), 3.0);
        test::screenshot(window_, QStringLiteral("main-window-status-line"), QRect(0, 0, window_->width(), 32));
        Q_EMIT bridge().pluginsLoading(0, 0);
        QVERIFY(!loading->isVisible());
    }

    // The session's warnings and informations are message boxes, one after another.
    void warningsAndInformation() {
        Q_EMIT session().warning(QStringLiteral("Something went wrong."));
        Q_EMIT session().information(QStringLiteral("For your information."));
        QTRY_VERIFY(shown(QStringLiteral("messageBox")));
        QCOMPARE(prop(QStringLiteral("messageBox"), "text").toString(), QStringLiteral("Something went wrong."));
        QCOMPARE(prop(QStringLiteral("messageBox"), "icon").toString(), QStringLiteral("warning"));
        test::screenshot(window_, QStringLiteral("message-box-warning"));
        answer(QStringLiteral("messageBox"), QStringLiteral("ok"));
        QTRY_VERIFY(shown(QStringLiteral("messageBox")));
        QCOMPARE(prop(QStringLiteral("messageBox"), "text").toString(), QStringLiteral("For your information."));
        key(Qt::Key_Return);  // (its OK)
        QTRY_VERIFY(!shown(QStringLiteral("messageBox")));
    }

    // Open Recent (test_open_recent): the projects ("&1  name"), Clear List; a
    // missing one is taken off the list with a warning.
    void openRecent() {
        session().clearRecentProjects();
        const QString first = dir_->path(QStringLiteral("first.gilproj"));
        const QString second = dir_->path(QStringLiteral("second & more.gilproj"));
        QVERIFY(session().saveProjectAs(first));
        QVERIFY(session().saveProjectAs(second));
        QVERIFY(session().saveProjectAs(first));

        auto* menu = find(QStringLiteral("recentMenu"));
        auto labels = [&] {
            call("fillRecentMenu");
            QStringList texts;
            const int count = menu->property("count").toInt();
            for (int i = 0; i < count; ++i) {
                QQuickItem* entry = nullptr;
                QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem*, entry), Q_ARG(int, i));
                texts << (entry->inherits("QQuickMenuSeparator") ? QStringLiteral("-") : entry->property("text").toString());
            }
            return texts;
        };
        QCOMPARE(labels(), (QStringList{QStringLiteral("&1  first.gilproj"), QStringLiteral("&2  second && more.gilproj"),
                                        QStringLiteral("-"), QStringLiteral("&Clear List")}));
        QQuickItem* entry = nullptr;
        QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem*, entry), Q_ARG(int, 1));
        QMetaObject::invokeMethod(entry, "triggered");
        QCOMPARE(QFileInfo(project().path()).fileName(), QStringLiteral("second & more.gilproj"));

        QVERIFY(QFile::remove(second));
        call("openRecent", second);
        QTRY_VERIFY(shown(QStringLiteral("messageBox")));  // it can't be found: taken off the list
        answer(QStringLiteral("messageBox"), QStringLiteral("ok"));
        QCOMPARE(labels().size(), 3);
        QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem*, entry), Q_ARG(int, 2));
        QMetaObject::invokeMethod(entry, "triggered");  // Clear List
        QCOMPARE(labels(), QStringList{QStringLiteral("No Recent Projects")});
    }

    // New, Open and Open Recent ask about unsaved changes: Cancel keeps them,
    // Discard drops them, Save saves first (asking where for a project never saved).
    void unsavedChanges() {
        trigger(QStringLiteral("insertAudioTrack"));
        trigger(QStringLiteral("newProject"));
        QTRY_VERIFY(shown(QStringLiteral("unsavedChangesDialog")));
        QCOMPARE(prop(QStringLiteral("unsavedChangesDialog"), "text").toString(),
                 QStringLiteral("Save changes to the current project?"));
        test::screenshot(window_, QStringLiteral("unsaved-changes"));
        answer(QStringLiteral("unsavedChangesDialog"), QStringLiteral("cancel"));
        QCOMPARE(project().tracks().size(), size_t(1));

        trigger(QStringLiteral("newProject"));
        answer(QStringLiteral("unsavedChangesDialog"), QStringLiteral("discard"));
        QCOMPARE(project().tracks().size(), size_t(0));
        QVERIFY(session().clean());

        // Never saved: Save asks where, and goes on once saved.
        trigger(QStringLiteral("insertAudioTrack"));
        trigger(QStringLiteral("newProject"));
        answer(QStringLiteral("unsavedChangesDialog"), QStringLiteral("save"));
        QTRY_VERIFY(shown(QStringLiteral("saveDialog")));
        const QString path = dir_->path(QStringLiteral("saved first.gilproj"));
        call("saveChosen", path);  // (the dialog's accepted)
        QMetaObject::invokeMethod(find(QStringLiteral("saveDialog")), "close");
        QVERIFY(QFileInfo::exists(path));
        QCOMPARE(project().tracks().size(), size_t(0));  // then the new project
        QCOMPARE(project().path(), QString());

        // Saved before: Save saves to its file, then opens the other.
        QVERIFY(session().openProject(path));
        QCOMPARE(project().tracks().size(), size_t(1));
        trigger(QStringLiteral("insertMidiTrack"));
        const QString other = dir_->path(QStringLiteral("other.gilproj"));
        session().newProject();
        QVERIFY(session().saveProjectAs(other));
        QVERIFY(session().openProject(path));
        trigger(QStringLiteral("insertMidiTrack"));
        call("openRecent", other);
        answer(QStringLiteral("unsavedChangesDialog"), QStringLiteral("save"));
        QCOMPARE(QFileInfo(project().path()).fileName(), QStringLiteral("other.gilproj"));
        QVERIFY(session().openProject(path));
        QCOMPARE(project().tracks().size(), size_t(2));  // (saved with the MIDI track)

        // Open… asks first, then which file; Ctrl+S on a project never saved asks where.
        trigger(QStringLiteral("insertAudioTrack"));
        key(Qt::Key_O, Qt::ControlModifier);
        answer(QStringLiteral("unsavedChangesDialog"), QStringLiteral("discard"));
        QTRY_VERIFY(shown(QStringLiteral("openDialog")));
        QMetaObject::invokeMethod(find(QStringLiteral("openDialog")), "close");
        call("openChosen", other);
        QCOMPARE(QFileInfo(project().path()).fileName(), QStringLiteral("other.gilproj"));
        session().newProject();
        trigger(QStringLiteral("insertAudioTrack"));
        key(Qt::Key_S, Qt::ControlModifier);
        QTRY_VERIFY(shown(QStringLiteral("saveDialog")));
        call("saveChosen", dir_->path(QStringLiteral("ctrl s.gilproj")));
        QMetaObject::invokeMethod(find(QStringLiteral("saveDialog")), "close");
        QVERIFY(session().clean());
        QCOMPARE(window_->title(), QStringLiteral("ctrl s - SUBstation"));
    }

    // Export Audio: nothing to export says so; else it asks where and renders it.
    void exportAudio() {
        call("exportChosen", QStringLiteral("arrangement"), 24);
        QTRY_VERIFY(shown(QStringLiteral("messageBox")));
        QCOMPARE(prop(QStringLiteral("messageBox"), "text").toString(), QStringLiteral("There is nothing to export yet."));
        answer(QStringLiteral("messageBox"), QStringLiteral("ok"));

        const QString path = wav(QStringLiteral("export.wav"), 0.5);
        audioTrack(path, 0.5);
        call("exportChosen", QStringLiteral("arrangement"), 16);
        QTRY_VERIFY(shown(QStringLiteral("exportFileDialog")));
        QCOMPARE(prop(QStringLiteral("exportFileDialog"), "bitDepth").toInt(), 16);
        QMetaObject::invokeMethod(find(QStringLiteral("exportFileDialog")), "close");
        const QString mix = dir_->path(QStringLiteral("mix.wav"));
        call("exportFileChosen", mix);
        QTRY_VERIFY_WITH_TIMEOUT(!session().render()->active(), 30000);
        QVERIFY(QFileInfo::exists(mix));
        QCOMPARE(status(), QStringLiteral("Exported mix.wav"));
    }

    // Closing while a render runs cancels it (the window stays); with unsaved
    // changes it asks first (Cancel: the window stays).
    void closing() {
        const QString path = wav(QStringLiteral("long.wav"), 1.0);
        audioTrack(path, 1.0, 10000.0);  // over an hour to render
        QVERIFY(session().exportAudio(dir_->path(QStringLiteral("long mix.wav")), QStringLiteral("arrangement"), 24));
        QTRY_VERIFY(shown(QStringLiteral("renderDialog")));
        window_->close();
        QVERIFY(window_->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(!session().render()->active(), 10000);
        QCOMPARE(status(), QStringLiteral("Export cancelled"));

        QVERIFY(!session().clean());
        window_->close();
        QVERIFY(window_->isVisible());
        QTRY_VERIFY(shown(QStringLiteral("unsavedChangesDialog")));
        answer(QStringLiteral("unsavedChangesDialog"), QStringLiteral("cancel"));
        QVERIFY(window_->isVisible());
        trigger(QStringLiteral("quit"));
        QTRY_VERIFY(shown(QStringLiteral("unsavedChangesDialog")));
        answer(QStringLiteral("unsavedChangesDialog"), QStringLiteral("cancel"));
        QVERIFY(window_->isVisible());
    }

    // Insert MIDI Clip opens the new clip in the clip view, which covers the
    // arrangement (the device view stays); Shift+Tab (or its Esc) goes back to
    // the arrangement.
    void clipViewCoversTheArrangement() {
        trigger(QStringLiteral("insertMidiTrack"));
        auto* clipView = item(QStringLiteral("clipView"));
        auto* lanes = item(QStringLiteral("arrangement"));
        auto* devices = item(QStringLiteral("devicePanel"));
        QVERIFY(!clipView->isVisible() && lanes->isVisible() && devices->isVisible());
        trigger(QStringLiteral("insertMidiClip"));
        QCOMPARE(project().tracks()[0].clips.size(), size_t(1));
        QTRY_VERIFY(clipView->isVisible());
        QVERIFY(!lanes->isVisible() && devices->isVisible());
        QCOMPARE(clipView->property("midi").toBool(), true);
        test::screenshot(window_, QStringLiteral("main-window-clip-view"));
        key(Qt::Key_Tab, Qt::ShiftModifier);
        QVERIFY(!clipView->isVisible() && lanes->isVisible());

        // Requested again (a double-click on the clip): it opens; Esc closes it.
        const QString track = project().tracks()[0].id;
        Q_EMIT session().arrangement()->clipViewRequested(
            QVariantList{QVariantMap{{QStringLiteral("trackId"), track},
                                     {QStringLiteral("clipId"), project().tracks()[0].clips[0].id}}},
            track, project().tracks()[0].clips[0].id);
        QTRY_VERIFY(clipView->isVisible());
        QMetaObject::invokeMethod(clipView, "closeRequested");
        QVERIFY(!clipView->isVisible() && lanes->isVisible());

        // With the device view hidden, a clip opens all the same (over the arrangement).
        key(Qt::Key_L, Qt::ControlModifier | Qt::AltModifier);
        QVERIFY(!devices->isVisible());
        Q_EMIT session().arrangement()->clipViewRequested(
            QVariantList{QVariantMap{{QStringLiteral("trackId"), track},
                                     {QStringLiteral("clipId"), project().tracks()[0].clips[0].id}}},
            track, project().tracks()[0].clips[0].id);
        QTRY_VERIFY(clipView->isVisible());
        QVERIFY(!devices->isVisible());
        // Its ruler clicked: the insert marker and the playhead go there.
        QMetaObject::invokeMethod(clipView, "locateRequested", Q_ARG(double, 3.0));
        QCOMPARE(selection().insertBeat(), 3.0);
        QCOMPARE(bridge().position(), 3.0);
        QMetaObject::invokeMethod(clipView, "closeRequested");
    }

    // What the arrangement and the piano roll scroll out of view (a selected
    // time range, the playhead, notes) never shows beside them, over the
    // browser: their items draw only inside themselves (Qt Quick doesn't clip
    // an item to its bounds).
    void scrolledOutOfViewIsntDrawnOverTheBrowser() {
        auto* browser = item(QStringLiteral("browser"));
        auto* lanes = item(QStringLiteral("lanes"));
        auto* view = item(QStringLiteral("arrangement"));
        auto* clipView = item(QStringLiteral("clipView"));
        QVERIFY(browser && lanes && view && clipView);
        auto* arrangement = view->property("arrangement").value<sub::ui::Arrangement*>();
        QVERIFY(arrangement);
        const QString track = editor().addMidiTrack(-1, QStringLiteral("Keys"));
        const auto clip = editor().addMidiClip(track, 0.0, 16.0);
        QVERIFY(clip);
        editor().setClipNotes(*clip, {{60, 0.0, 16.0, 100}, {64, 0.0, 16.0, 90}, {67, 4.0, 12.0, 80}},
                              QStringLiteral("setup"));
        selection().setTimeRange(0.0, 16.0, {track});
        bridge().play();
        arrangement->onPosition(4.0);  // (the playhead shows while playing, if the engine plays here)
        QTest::qWait(50);

        // A part of the window (scene coordinates) as a grab has it; the browser's right part beside an item.
        auto grab = [&](const QRectF& part) {
            const qreal dpr = window_->effectiveDevicePixelRatio();
            return window_->grabWindow().copy(QRectF(part.topLeft() * dpr, part.size() * dpr).toAlignedRect());
        };
        auto beside = [&](QQuickItem* next) {
            const QRectF b = browser->mapRectToScene(browser->boundingRect());
            const QRectF n = next->mapRectToScene(next->boundingRect());
            return grab(QRectF(b.right() - 60, n.top(), 56, n.height()));
        };
        auto differing = [](const QImage& a, const QImage& b) {
            int count = 0;
            for (int y = 0; y < a.height(); ++y)
                for (int x = 0; x < a.width(); ++x) count += a.pixel(x, y) != b.pixel(x, y);
            return count;
        };
        const QImage before = beside(lanes);
        arrangement->setScrollBeats(14.0);  // the range, the clip and the playhead are left of the lanes now
        QTest::qWait(50);
        test::screenshot(window_, QStringLiteral("main-window-scrolled-arrangement"));
        QCOMPARE(differing(beside(lanes), before), 0);
        bridge().stop();
        arrangement->onPosition(0.0);

        // The piano roll's notes, scrolled out to the left of the grid.
        Q_EMIT session().arrangement()->clipViewRequested(
            QVariantList{QVariantMap{{QStringLiteral("trackId"), track}, {QStringLiteral("clipId"), clip->clipId}}},
            track, clip->clipId);
        QTRY_VERIFY(clipView->isVisible());
        auto* roll = clipView->property("pianoRollView").value<QQuickItem*>()->property("roll").value<sub::ui::PianoRoll*>();
        QVERIFY(roll);
        auto* grid = item(QStringLiteral("noteGrid"));
        auto* keys = item(QStringLiteral("pianoKeys"));
        QVERIFY(grid && keys);
        QTest::qWait(50);
        const QRectF keysPart = keys->mapRectToScene(keys->boundingRect());
        const QImage browserBefore = beside(grid);
        const QImage keysBefore = grab(keysPart);
        roll->setScrollBeats(12.0);  // the notes run on left of the grid, over the keys and the browser
        QTest::qWait(50);
        test::screenshot(window_, QStringLiteral("main-window-scrolled-piano-roll"));
        QCOMPARE(differing(beside(grid), browserBefore), 0);
        QCOMPARE(differing(grab(keysPart), keysBefore), 0);  // (nor over the keys)
        QMetaObject::invokeMethod(clipView, "closeRequested");
    }

    // The actions' other keys: Ctrl+Shift+Z redoes, Backspace deletes,
    // Ctrl+Shift+M inserts a MIDI clip, Shift+Tab (as a keyboard sends it:
    // Backtab) toggles the clip view; Ctrl+T, Ctrl+Shift+T, Ctrl+Alt+T, Ctrl+G.
    // The computer MIDI keyboard's keys play while it is on, even those that are
    // the window's shortcuts (S: solo, A: automation).
    void computerKeyboardKeysBeforeShortcuts() {
        session().newProject();
        trigger(QStringLiteral("insertAudioTrack"));
        const QString track = project().tracks()[0].id;
        selection().selectTrack(track, true);
        QList<QList<int>> sent;
        session().computerKeyboard()->setSender([&](const QList<int>& message) { sent << message; });
        session().computerKeyboard()->setEnabled(true);
        key(Qt::Key_S);
        key(Qt::Key_A);
        QVERIFY(!project().track(track).solo);
        QVERIFY(!project().automationView(track).shown);
        QCOMPARE(sent.size(), 4);  // D and C, each on and off
        QCOMPARE(sent[0][0] & 0xF0, 0x90);
        QCOMPARE(sent[1][0] & 0xF0, 0x80);
        session().computerKeyboard()->setEnabled(false);
        session().computerKeyboard()->setSender(nullptr);
        key(Qt::Key_S);  // off: the shortcut
        QVERIFY(project().track(track).solo);
    }

    void shortcuts() {
        key(Qt::Key_T, Qt::ControlModifier);
        key(Qt::Key_T, Qt::ControlModifier | Qt::ShiftModifier);
        key(Qt::Key_T, Qt::ControlModifier | Qt::AltModifier);
        QCOMPARE(project().tracks().size(), size_t(2));
        QCOMPARE(project().returns().size(), size_t(1));
        key(Qt::Key_Z, Qt::ControlModifier);
        QCOMPARE(project().returns().size(), size_t(0));
        key(Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(project().returns().size(), size_t(1));
        key(Qt::Key_Z, Qt::ControlModifier);
        key(Qt::Key_Y, Qt::ControlModifier);
        QCOMPARE(project().returns().size(), size_t(1));

        // Backspace: the selected tracks go (the track header was clicked).
        selection().selectTrack(project().tracks()[0].id, true);
        key(Qt::Key_Backspace);
        QCOMPARE(project().tracks().size(), size_t(1));
        QVERIFY(project().tracks()[0].isMidi());

        selection().selectTrack(project().tracks()[0].id, true);
        key(Qt::Key_M, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(project().tracks()[0].clips.size(), size_t(1));
        auto* clipView = item(QStringLiteral("clipView"));
        QTRY_VERIFY(clipView->isVisible());
        window_->contentItem()->forceActiveFocus();
        key(Qt::Key_Backtab, Qt::ShiftModifier);
        QVERIFY(!clipView->isVisible());

        key(Qt::Key_Equal);  // (Zoom In's other key: the arrangement's, none here)
        selection().clear();  // (the new clip was selected: a time range, which it fills)
        selection().selectTrack(project().tracks()[0].id, true);
        selection().setInsert(8.0);
        key(Qt::Key_D, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(project().tracks()[0].clips.size(), size_t(2));
        QTRY_VERIFY(clipView->isVisible());
        QMetaObject::invokeMethod(clipView, "closeRequested");

        // C: the piano roll's chords and key, shown or hidden (a setting).
        session().harmony()->setShown(true);
        window_->contentItem()->forceActiveFocus();
        key(Qt::Key_C);
        QVERIFY(!session().harmony()->shown());
        QVERIFY(!prop(QStringLiteral("showHarmony"), "checked").toBool());
        key(Qt::Key_C);
        QVERIFY(session().harmony()->shown());
        QVERIFY(prop(QStringLiteral("showHarmony"), "checked").toBool());

        // 0: the selected clips deactivated; again, activated (Edit › Deactivate / Activate Clips).
        const QString track = project().tracks()[0].id;
        const QString second = project().tracks()[0].clips[1].id;
        selection().selectClips(editor(), {sub::app::ClipRef{track, second}}, track);
        window_->contentItem()->forceActiveFocus();
        key(Qt::Key_0);
        QVERIFY(project().clip(track, second).muted);
        QVERIFY(!project().tracks()[0].clips[0].muted);
        key(Qt::Key_0);
        QVERIFY(!project().clip(track, second).muted);
    }

    // Edit › Rename: the session picks what; the view renames it in place
    // (nothing to rename: the status line says so).
    void rename() {
        key(Qt::Key_R, Qt::ControlModifier);
        QCOMPARE(status(), QStringLiteral("Select a track, a rack chain or a preset to rename."));
        Q_EMIT session().statusMessage(QString());
        trigger(QStringLiteral("insertAudioTrack"));
        key(Qt::Key_R, Qt::ControlModifier);  // the new track's name, in place in its header
        QCOMPARE(status(), QString());
        auto* focused = qobject_cast<QQuickItem*>(window_->activeFocusItem());
        QVERIFY(focused && focused->inherits("QQuickTextInput"));
        key(Qt::Key_Escape);
    }

    // The keys of plug-ins' editors (test_shortcuts_from_plugin_editor): what
    // the window takes, and what stays with the plug-in.
    void pluginEditorKeys() {
        auto* keys = window_->findChild<sub::ui::PluginEditorKeys*>();
        QVERIFY(keys);
#ifdef Q_OS_WIN
        QVERIFY(keys->supported());  // (only on Windows does it watch messages)
#else
        QVERIFY(!keys->supported());
#endif
        const int ctrl = Qt::ControlModifier;
        session().browser()->setScope({QStringLiteral("samples")});
        QVERIFY(keys->keyPressed(vk('F'), ctrl));  // Ctrl+F in a plug-in's editor searches the browser
        QCOMPARE(session().browser()->scope(), QStringList{QStringLiteral("all")});
        QVERIFY(!keys->keyPressed(vk('C'), ctrl));  // the plug-in keeps its copy / paste / undo...
        QVERIFY(!keys->keyPressed(vk('Z'), ctrl));
        QVERIFY(!keys->keyPressed(vk('Z'), ctrl | Qt::ShiftModifier));
        QVERIFY(!keys->keyPressed(vk('K'), ctrl));  // ...and keys that are no shortcut
        QVERIFY(keys->keyPressed(vk('T'), ctrl));   // Insert Audio Track
        QCOMPARE(project().tracks().size(), size_t(1));
        QCOMPARE(keys->actionFor(vk('L'), ctrl), find(QStringLiteral("loop")));
        QCOMPARE(keys->actionFor(0xBB, 0), nullptr);  // "=" without Ctrl: the plug-in's

        // Space and S without Ctrl/Alt: the window's (the DAW comes first); other keys: the plug-in's.
        QVERIFY(keys->keyPressed(kVkSpace, 0));
        QVERIFY(bridge().isPlaying());
        QVERIFY(keys->keyPressed(kVkSpace, 0, false, true));  // held down: taken, but once
        QVERIFY(bridge().isPlaying());
        QVERIFY(keys->keyPressed(kVkSpace, 0));
        QVERIFY(!bridge().isPlaying());
        selection().selectTrack(project().tracks()[0].id, true);
        QVERIFY(keys->keyPressed(vk('S'), 0));
        QVERIFY(project().tracks()[0].solo);
        QVERIFY(!keys->keyPressed(vk('S'), Qt::ShiftModifier));
        QVERIFY(!keys->keyPressed(vk('D'), 0));
        QVERIFY(!keys->keyPressed(kVkDelete, 0));
        QVERIFY(!keys->keyPressed(kVkSpace, 0, true));  // typing into an Edit control
        QVERIFY(!bridge().isPlaying());
        session().computerKeyboard()->setEnabled(true);  // S plays a note, as in the window
        QVERIFY(!keys->keyPressed(vk('S'), 0));
        QVERIFY(keys->actionFor(kVkSpace, 0) != nullptr);
        session().computerKeyboard()->setEnabled(false);
        // An action's other shortcut (a Shortcut): Ctrl+Shift+Z is the plug-in's,
        // Ctrl+Shift+M inserts a MIDI clip.
        QVERIFY(keys->actionFor(vk('M'), ctrl | Qt::ShiftModifier) != nullptr);
        QVERIFY(keys->actionFor(vk('M'), ctrl | Qt::ShiftModifier)->inherits("QQuickShortcut"));
        // While a render's dialog is up the window takes no keys, from plug-ins' editors either.
        const QString path = wav(QStringLiteral("keys.wav"), 1.0);
        audioTrack(path, 1.0, 10000.0);
        QVERIFY(session().exportAudio(dir_->path(QStringLiteral("keys mix.wav")), QStringLiteral("arrangement"), 24));
        QVERIFY(session().render()->active());
        QVERIFY(!keys->keyPressed(kVkSpace, 0));
        QVERIFY(!bridge().isPlaying());
        session().render()->cancel();
        QTRY_VERIFY_WITH_TIMEOUT(!session().render()->active(), 10000);
        QCOMPARE(sub::ui::PluginEditorKeys::closeForemostEditor(), false);
    }

    // The window's place and the splitter are kept (window/geometry, window/splitter).
    void windowStateKept() {
        session().newProject();
        window_->setGeometry(QRect(150, 120, 1300, 800));
        QTest::qWait(100);
        window_->close();  // (nothing unsaved: it closes)
        QTRY_VERIFY(!window_->isVisible());
        QSettings settings;
        const QVariantMap geometry = settings.value(QStringLiteral("window/geometry")).toMap();
        QCOMPARE(geometry.value(QStringLiteral("rect")).toRect().size(), QSize(1300, 800));
        QVERIFY(settings.value(QStringLiteral("window/splitter")).isValid());
        QVERIFY(settings.value(QStringLiteral("window/device_splitter")).isValid());

        // A window made again is where this one was.
        warnings_.clear();
        window_ = ui_->show(kWindow);
        QVERIFY(window_);
        QCOMPARE(window_->size(), QSize(1300, 800));

        // What the widget version saved there (QWidget::saveGeometry, QSplitter::saveState) is ignored.
        auto* state = window_->findChild<sub::ui::WindowState*>();
        QVERIFY(state);
        settings.setValue(QStringLiteral("window/splitter"), QByteArray("\x00\x00\x00\xff\x00\x00\x00\x01", 8));
        QVERIFY(!state->splitterState().isValid());
        settings.setValue(QStringLiteral("window/geometry"), QByteArray("\x01\xd9\xd0\xcb\x00\x03", 6));
        QVERIFY(!state->restore());
        QCOMPARE(window_->size(), QSize(1300, 800));
    }
};

namespace {

const char* const kWindow = R"(
import SUBstation

Main {}
)";

}  // namespace

QTEST_MAIN(TestUiMainWindow)
#include "test_ui_mainwindow.moc"
