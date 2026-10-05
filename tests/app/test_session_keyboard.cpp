// The computer MIDI keyboard (the session's ComputerKeyboard): turned on, the
// letter keys play notes (A is C3, W C#3, ...), Z and X change the octave, and
// while it is on those keys don't reach the window's shortcuts (S, A, Z) nor
// text inputs (an object answering Qt::ImEnabled), and keys with modifiers are
// left alone. Notes held are released when it is turned off, and when the
// application loses the keyboard.
// From tests/test_computer_keyboard.py (a QWindow and a QShortcut stand for the
// window and its actions).

#include "SessionFixture.h"
#include "TestSupport.h"

#include "audio/EngineBridge.h"
#include "session/ComputerKeyboard.h"

#include <QGuiApplication>
#include <QInputMethodQueryEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QSettings>
#include <QShortcut>
#include <QSignalSpy>
#include <QTest>
#include <QWindow>

#include <algorithm>

using namespace sub::app;
using sub::app::test::SessionFixture;

namespace {

constexpr int kOn = 0x90;
constexpr int kOff = 0x80;

using Message = QList<int>;

// What a text field answers: it takes text.
class TextInput : public QObject {
public:
    bool event(QEvent* event) override {
        if (event->type() == QEvent::InputMethodQuery) {
            auto* query = static_cast<QInputMethodQueryEvent*>(event);
            if (query->queries() & Qt::ImEnabled) query->setValue(Qt::ImEnabled, true);
            query->accept();
            return true;
        }
        return QObject::event(event);
    }
};

// The window, with whatever has its keyboard focus.
class Window : public QWindow {
public:
    QObject* focused = nullptr;
    QObject* focusObject() const override { return focused != nullptr ? focused : QWindow::focusObject(); }
};

}  // namespace

class TestSessionKeyboard : public QObject {
    Q_OBJECT

private:
    void press(Window& window, Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QTest::keyPress(&window, key, modifiers);
    }
    void release(Window& window, Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QTest::keyRelease(&window, key, modifiers);
    }

private Q_SLOTS:
    void initTestCase() { test::prepareApplication(); }
    void init() { QSettings().clear(); }

    void keysPlayNotesWhileItIsOn() {
        SessionFixture f;
        ComputerKeyboard& keyboard = *f.s().computerKeyboard();
        QList<Message> played;
        keyboard.setSender([&](const QList<int>& message) { played.append(message); });
        Window window;
        window.show();
        window.requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        QShortcut solo(QKeySequence(Qt::Key_S), &window);  // S: Solo Selected Tracks, while it is off
        QSignalSpy soloed(&solo, &QShortcut::activated);

        press(window, Qt::Key_A);
        release(window, Qt::Key_A);
        QVERIFY(played.isEmpty());  // off: A is the automation shortcut
        QSignalSpy changed(&keyboard, &ComputerKeyboard::changed);
        keyboard.toggle();  // M
        QVERIFY(keyboard.enabled() && changed.count() == 1);
        QCOMPARE(f.lastMessage(), QStringLiteral("Computer MIDI keyboard on: A plays C3, Z and X change the octave."));
        QCOMPARE(keyboard.toolTip(),
                 QStringLiteral("Computer MIDI Keyboard (M): A S D F... play the white keys from C3, W E T Y U... the "
                                "black keys; Z and X change the octave"));

        for (Qt::Key key : {Qt::Key_A, Qt::Key_W, Qt::Key_S, Qt::Key_J, Qt::Key_K, Qt::Key_Apostrophe}) press(window, key);
        QCOMPARE(played, (QList<Message>{{kOn, 60, 100}, {kOn, 61, 100}, {kOn, 62, 100}, {kOn, 71, 100}, {kOn, 72, 100},
                                         {kOn, 77, 100}}));
        QCOMPARE(soloed.count(), 0);  // S played a note instead of soloing
        played.clear();
        QKeyEvent repeat(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("a"), true);
        QCoreApplication::sendEvent(&window, &repeat);  // held down: no new note
        release(window, Qt::Key_A);
        QCOMPARE(played, (QList<Message>{{kOff, 60, 0}}));

        played.clear();
        press(window, Qt::Key_Z);  // an octave down, while W is still held
        press(window, Qt::Key_A);
        release(window, Qt::Key_W);  // still releases the note it started
        QCOMPARE(played, (QList<Message>{{kOn, 48, 100}, {kOff, 61, 0}}));
        QVERIFY(f.lastMessage().contains(QStringLiteral("C2")));
        for (int i = 0; i < 12; ++i) press(window, Qt::Key_X);
        QCOMPARE(keyboard.octaveLabel(), QStringLiteral("C7"));  // as high as it goes
        for (int i = 0; i < 12; ++i) press(window, Qt::Key_Z);
        QCOMPARE(keyboard.octaveLabel(), QStringLiteral("C-2"));

        // Turning it off releases what is held; then the keys are shortcuts again.
        played.clear();
        keyboard.toggle();
        QVERIFY(!keyboard.enabled());
        std::sort(played.begin(), played.end());
        QCOMPARE(played, (QList<Message>{{kOff, 48, 0}, {kOff, 62, 0}, {kOff, 71, 0}, {kOff, 72, 0}, {kOff, 77, 0}}));
        QCOMPARE(f.lastMessage(), QStringLiteral("Computer MIDI keyboard off."));
        played.clear();
        press(window, Qt::Key_S);
        release(window, Qt::Key_S);
        QVERIFY(played.isEmpty());
        QCOMPARE(soloed.count(), 1);
    }

    void textInputsAndModifiersKeepTheirKeys() {
        SessionFixture f;
        ComputerKeyboard& keyboard = *f.s().computerKeyboard();
        QList<Message> played;
        keyboard.setSender([&](const QList<int>& message) { played.append(message); });
        Window window;
        window.show();
        window.requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(&window));
        keyboard.setEnabled(true);
        TextInput field;
        window.focused = &field;
        QVERIFY(ComputerKeyboard::focusTakesText());
        for (Qt::Key key : {Qt::Key_A, Qt::Key_S, Qt::Key_D}) {
            press(window, key);
            release(window, key);
        }
        QVERIFY(played.isEmpty());
        window.focused = nullptr;
        QVERIFY(!ComputerKeyboard::focusTakesText());
        press(window, Qt::Key_A, Qt::ControlModifier);
        release(window, Qt::Key_A, Qt::ControlModifier);
        QVERIFY(played.isEmpty());
        // A key-up goes through even when a modifier was pressed meanwhile.
        press(window, Qt::Key_D);
        release(window, Qt::Key_D, Qt::ShiftModifier);
        QCOMPARE(played, (QList<Message>{{kOn, 64, 100}, {kOff, 64, 0}}));
    }

    void notesAreReleasedWhenTheApplicationLosesTheKeyboard() {
        SessionFixture f;
        ComputerKeyboard& keyboard = *f.s().computerKeyboard();
        QList<Message> played;
        keyboard.setSender([&](const QList<int>& message) { played.append(message); });
        keyboard.setEnabled(true);
        keyboard.press(Qt::Key_K);
        QCOMPARE(played, (QList<Message>{{kOn, 72, 100}}));
        Q_EMIT qGuiApp->applicationStateChanged(Qt::ApplicationInactive);  // its key-ups would go elsewhere
        QCOMPARE(played.back(), (Message{kOff, 72, 0}));
        QVERIFY(keyboard.takesKey(Qt::Key_Z) && keyboard.takesKey(Qt::Key_Semicolon) && !keyboard.takesKey(Qt::Key_M));
        keyboard.setEnabled(false);
        QVERIFY(!keyboard.takesKey(Qt::Key_A));
        // Without a sender of its own it plays into the bridge's computer keyboard input.
        QCOMPARE(f.bridge().midiInputChoices().last(), kComputerKeyboard);
    }

    void theHighestOctaveReachesF125() {
        SessionFixture f;
        ComputerKeyboard& keyboard = *f.s().computerKeyboard();
        QList<Message> played;
        keyboard.setSender([&](const QList<int>& message) { played.append(message); });
        keyboard.setEnabled(true);
        for (int i = 0; i < 9; ++i) keyboard.shiftOctave(1);
        QCOMPARE(keyboard.octave(), ComputerKeyboard::kMaxOctave);
        keyboard.press(Qt::Key_Apostrophe);  // F of the next octave: 125
        keyboard.press(Qt::Key_K);  // 120
        QCOMPARE(played, (QList<Message>{{kOn, 125, 100}, {kOn, 120, 100}}));
    }
};

QTEST_MAIN(TestSessionKeyboard)
#include "test_session_keyboard.moc"
