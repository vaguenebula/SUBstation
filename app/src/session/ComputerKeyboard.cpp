#include "session/ComputerKeyboard.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QInputMethodQueryEvent>
#include <QKeyEvent>

#include <algorithm>
#include <utility>

#include "audio/EngineBridge.h"
#include "model/Notes.h"

namespace sub::app {

namespace {

constexpr int kNoteOn = 0x90;
constexpr int kNoteOff = 0x80;

}  // namespace

ComputerKeyboard::ComputerKeyboard(EngineBridge* bridge, QObject* parent) : QObject(parent), bridge_(bridge) {
    if (auto* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        app->installEventFilter(this);
        connect(app, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
            if (state != Qt::ApplicationActive) releaseAll();  // its key-ups would go elsewhere
        });
    }
}

ComputerKeyboard::~ComputerKeyboard() {
    if (QCoreApplication* app = QCoreApplication::instance()) app->removeEventFilter(this);
}

int ComputerKeyboard::noteOffset(int key) {
    switch (key) {
        case Qt::Key_A: return 0;
        case Qt::Key_W: return 1;
        case Qt::Key_S: return 2;
        case Qt::Key_E: return 3;
        case Qt::Key_D: return 4;
        case Qt::Key_F: return 5;
        case Qt::Key_T: return 6;
        case Qt::Key_G: return 7;
        case Qt::Key_Y: return 8;
        case Qt::Key_H: return 9;
        case Qt::Key_U: return 10;
        case Qt::Key_J: return 11;
        case Qt::Key_K: return 12;
        case Qt::Key_O: return 13;
        case Qt::Key_L: return 14;
        case Qt::Key_P: return 15;
        case Qt::Key_Semicolon: return 16;
        case Qt::Key_Apostrophe: return 17;
        default: return -1;
    }
}

int ComputerKeyboard::octaveStep(int key) {
    if (key == Qt::Key_Z) return -1;
    if (key == Qt::Key_X) return 1;
    return 0;
}

QString ComputerKeyboard::octaveLabel() const { return notes::noteName(lowestNote()); }

QString ComputerKeyboard::toolTip() const {
    return QStringLiteral("Computer MIDI Keyboard (M): A S D F... play the white keys from %1, W E T Y U... the black "
                          "keys; Z and X change the octave")
        .arg(octaveLabel());
}

void ComputerKeyboard::setEnabled(bool enabled) {
    if (enabled == enabled_) return;
    enabled_ = enabled;
    if (!enabled) releaseAll();
    Q_EMIT changed();
    const QString state = enabled ? QStringLiteral("on: A plays %1, Z and X change the octave").arg(octaveLabel())
                                  : QStringLiteral("off");
    Q_EMIT statusMessage(QStringLiteral("Computer MIDI keyboard %1.").arg(state));
}

void ComputerKeyboard::shiftOctave(int delta) {
    const int octave = std::clamp(octave_ + delta, 0, kMaxOctave);
    if (octave != octave_) {
        octave_ = octave;
        Q_EMIT changed();
    }
    Q_EMIT statusMessage(QStringLiteral("Computer MIDI keyboard: A plays %1.").arg(octaveLabel()));
}

void ComputerKeyboard::press(int key) {
    if (const int step = octaveStep(key)) {
        shiftOctave(step);
        return;
    }
    const int offset = noteOffset(key);
    if (held_.contains(key) || offset < 0) return;
    const int note = lowestNote() + offset;
    if (note > 127) return;
    held_.insert(key, note);
    send({kNoteOn, note, kVelocity});
}

void ComputerKeyboard::release(int key) {
    const auto it = held_.find(key);
    if (it == held_.end()) return;
    const int note = it.value();
    held_.erase(it);
    send({kNoteOff, note, 0});
}

void ComputerKeyboard::setSender(Sender sender) { sender_ = std::move(sender); }

void ComputerKeyboard::send(const QList<int>& message) {
    if (sender_) {
        sender_(message);
    } else {
        bridge_->sendMidi(message, kComputerKeyboard);
    }
}

void ComputerKeyboard::releaseAll() {
    const QList<int> keys = held_.keys();
    for (int key : keys) release(key);
}

bool ComputerKeyboard::takesKey(int key) const {
    return enabled_ && (noteOffset(key) >= 0 || octaveStep(key) != 0);
}

bool ComputerKeyboard::focusTakesText() {
    QObject* focus = QGuiApplication::focusObject();
    if (focus == nullptr) return false;
    QInputMethodQueryEvent query(Qt::ImEnabled);
    QCoreApplication::sendEvent(focus, &query);
    return query.value(Qt::ImEnabled).toBool();
}

bool ComputerKeyboard::handles(const QKeyEvent* event) const {
    if (!enabled_ || (event->modifiers() & ~Qt::KeyboardModifiers(Qt::KeypadModifier))) return false;
    const int key = event->key();
    if (noteOffset(key) < 0 && octaveStep(key) == 0) return false;
    return !focusTakesText();
}

bool ComputerKeyboard::eventFilter(QObject* watched, QEvent* event) {
    const QEvent::Type type = event->type();
    if (type == QEvent::ShortcutOverride) {
        // The key comes as a key press, not as the window's shortcut (A, S...). Taken
        // here: a Qt Quick item given the event would set it back to ignored.
        if (!handles(static_cast<QKeyEvent*>(event))) return false;
        event->accept();
        return true;
    }
    if (type == QEvent::KeyPress || type == QEvent::KeyRelease) {
        const auto* key = static_cast<QKeyEvent*>(event);
        // Key-ups go through even when they no longer would be ours (a modifier pressed meanwhile).
        if (type == QEvent::KeyRelease && !key->isAutoRepeat() && held_.contains(key->key())) {
            release(key->key());
            return true;
        }
        if (!handles(key)) return false;
        // A key event reaches the filter once for each object it is delivered to: taken the first time.
        if (type == QEvent::KeyPress && !key->isAutoRepeat()) press(key->key());
        return true;
    }
    return QObject::eventFilter(watched, event);
}

}  // namespace sub::app
