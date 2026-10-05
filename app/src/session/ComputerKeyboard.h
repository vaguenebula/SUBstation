#pragma once
// The computer MIDI keyboard (as in Ableton): while it is on (M), the letter
// keys play notes into the MIDI tracks that hear it, as a MIDI input called
// "Computer Keyboard" (kComputerKeyboard; every track on All Ins hears it too).
//
// The middle row is the white keys from C (A, S, D, F, G, H, J, K, L, ;, '), the
// row above it the black keys between them (W, E, T, Y, U, O, P). Z and X move
// the octave down and up.
//
// The keys reach it before the window's shortcuts (S, A and Z do other things
// while it is off: it accepts their ShortcutOverride, so they come as key
// presses), but not while typing in a text input (the focus object answers
// Qt::ImEnabled to an input method query, as Qt Quick's text fields do), nor
// with modifiers held. Notes still held when it is turned off, or when the
// application loses the keyboard, are released. It is an application event
// filter (QtGui key events), so it sees the keys wherever they go.

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>

class QKeyEvent;

namespace sub::app {

class EngineBridge;

class ComputerKeyboard : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY changed)
    Q_PROPERTY(int octave READ octave NOTIFY changed)
    // The note A plays: "C3".
    Q_PROPERTY(QString octaveLabel READ octaveLabel NOTIFY changed)
    // The transport's button's tooltip.
    Q_PROPERTY(QString toolTip READ toolTip NOTIFY changed)

public:
    static constexpr int kDefaultOctave = 5;  // C3 (60) at A
    static constexpr int kMaxOctave = 9;      // its C is 108; the highest key is F of the next octave (125)
    static constexpr int kVelocity = 100;

    // Watches the application's keys (a QGuiApplication's; none otherwise).
    explicit ComputerKeyboard(EngineBridge* bridge, QObject* parent = nullptr);
    ~ComputerKeyboard() override;

    bool enabled() const { return enabled_; }
    void setEnabled(bool enabled);
    int octave() const { return octave_; }
    // The note A plays.
    int lowestNote() const { return octave_ * 12; }
    QString octaveLabel() const;
    QString toolTip() const;

    Q_INVOKABLE void toggle() { setEnabled(!enabled_); }
    Q_INVOKABLE void shiftOctave(int delta);
    // A key (Qt::Key) pressed or released while it is on: a note on or off (or the octave).
    void press(int key);
    void release(int key);
    void releaseAll();
    // Whether this key (without modifiers) plays (or changes the octave) while it is on.
    Q_INVOKABLE bool takesKey(int key) const;

    // Semitones above the octave's C a key plays (-1: none).
    static int noteOffset(int key);
    // -1 or 1 for the octave keys (0: not one).
    static int octaveStep(int key);
    // Whether the object with the keyboard focus takes text (it answers Qt::ImEnabled).
    static bool focusTakesText();

    // Where its MIDI messages go (default: the bridge's sendMidi, as the computer
    // keyboard; the tests listen here).
    using Sender = std::function<void(const QList<int>& message)>;
    void setSender(Sender sender);

Q_SIGNALS:
    void changed();  // turned on or off, or another octave
    void statusMessage(const QString& message);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    bool handles(const QKeyEvent* event) const;

    void send(const QList<int>& message);

    EngineBridge* bridge_;
    Sender sender_;
    bool enabled_ = false;
    int octave_ = kDefaultOctave;
    QHash<int, int> held_;  // key -> the note it plays (kept when the octave changes)
};

}  // namespace sub::app
