"""The computer MIDI keyboard: M turns it on, the letter keys play notes (A is
C3, W C#3, ...), Z and X change the octave, and while it is on those keys don't
reach the window's shortcuts (S solo, A automation, Z zoom) or text fields."""

import pytest
from PySide6.QtCore import QEvent, Qt
from PySide6.QtGui import QKeyEvent
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication, QLineEdit

from substation.audio.engine_bridge import COMPUTER_KEYBOARD
from substation.model.project import MidiInput

ON, OFF = 0x90, 0x80


@pytest.fixture
def played(window, monkeypatch):
    """The MIDI messages the keyboard sends: (device, status, key, velocity)."""
    messages = []
    monkeypatch.setattr(window.bridge, "send_midi", lambda message, device=COMPUTER_KEYBOARD:
                        messages.append((device, *message)))
    window.setFocus()
    return messages


def key(window, k, press=True):
    (QTest.keyPress if press else QTest.keyRelease)(window, k)


def test_m_turns_it_on_and_keys_play_notes(window, app, played):
    track = window.editor.add_midi_track()
    window.selection.select_track(track.id)
    key(window, Qt.Key.Key_A)
    key(window, Qt.Key.Key_A, press=False)
    assert played == []  # off: A is the automation shortcut
    QTest.keyClick(window, Qt.Key.Key_M)
    assert window.computer_keyboard.enabled and window.transport.computer_keys.isChecked()
    assert window.computer_keyboard_action.isChecked()

    for k in (Qt.Key.Key_A, Qt.Key.Key_W, Qt.Key.Key_S, Qt.Key.Key_J, Qt.Key.Key_K, Qt.Key.Key_Apostrophe):
        key(window, k)
    assert played == [(COMPUTER_KEYBOARD, ON, note, 100) for note in (60, 61, 62, 71, 72, 77)]
    assert not track.solo  # S played a note instead of soloing
    played.clear()
    repeat = QKeyEvent(QEvent.Type.KeyPress, Qt.Key.Key_A, Qt.KeyboardModifier.NoModifier, "a", True)
    QApplication.sendEvent(window, repeat)  # held down: no new note
    key(window, Qt.Key.Key_A, press=False)
    assert played == [(COMPUTER_KEYBOARD, OFF, 60, 0)]

    played.clear()
    key(window, Qt.Key.Key_Z)  # an octave down, while W is still held
    key(window, Qt.Key.Key_A)
    key(window, Qt.Key.Key_W, press=False)  # still releases the note it started
    assert played == [(COMPUTER_KEYBOARD, ON, 48, 100), (COMPUTER_KEYBOARD, OFF, 61, 0)]
    assert "C2" in window.statusBar().currentMessage()
    for _ in range(12):
        key(window, Qt.Key.Key_X)
    assert window.computer_keyboard.octave_label() == "C7"  # as high as it goes
    for _ in range(12):
        key(window, Qt.Key.Key_Z)
    assert window.computer_keyboard.octave_label() == "C-2"

    # Turning it off releases what is held; then the keys are shortcuts again.
    played.clear()
    QTest.keyClick(window, Qt.Key.Key_M)
    assert not window.computer_keyboard.enabled and not window.transport.computer_keys.isChecked()
    assert sorted(played) == [(COMPUTER_KEYBOARD, OFF, n, 0) for n in (48, 62, 71, 72, 77)]
    played.clear()
    QTest.keyClick(window, Qt.Key.Key_S)
    assert played == [] and track.solo


def test_text_fields_and_modifiers_keep_their_keys(window, app, played):
    window.transport.computer_keys.click()
    assert window.computer_keyboard.enabled
    field = QLineEdit(window)
    field.show()
    field.setFocus()
    app.processEvents()
    QTest.keyClicks(field, "asd")
    assert field.text() == "asd" and played == []
    window.setFocus()
    QTest.keyClick(window, Qt.Key.Key_A, Qt.KeyboardModifier.ControlModifier)
    assert played == []


def test_the_track_menu_offers_it(window, app):
    track = window.editor.add_midi_track()
    app.processEvents()
    header = window.arrangement.headers.headers[track.id]
    action = next(a for a in header.input_menu().actions() if a.text() == COMPUTER_KEYBOARD)
    action.trigger()
    assert track.midi_input == MidiInput(COMPUTER_KEYBOARD)


def test_record_from_the_computer_keyboard(window, app, tmp_path, monkeypatch):
    """Through the fake ASIO driver: the keys' notes become a MIDI clip."""
    from .conftest import TEST_ASIO_NAME
    from .test_asio import pytestmark  # noqa: F401 - skipped without ASIO
    from .test_ui_recording import BUFFER, FLOAT32_LSB, RATE, AudioSettings, Driver

    driver = Driver()
    driver.SetManual(1)
    driver.SetSampleType(FLOAT32_LSB)
    assert window.bridge.open_device(AudioSettings("ASIO", TEST_ASIO_NAME, RATE, BUFFER)) is None
    track = window.editor.add_midi_track()
    window.editor.arm_tracks([track.id], True)
    window.toggle_record()
    driver.process(4)
    window.setFocus()
    QTest.keyClick(window, Qt.Key.Key_M)
    key(window, Qt.Key.Key_D)  # E3
    driver.process(10)
    key(window, Qt.Key.Key_D, press=False)
    driver.process(4)
    window.toggle_record()
    [clip] = track.clips
    assert [(n.pitch, n.velocity) for n in clip.notes] == [(64, 100)]
    assert clip.notes[0].length == pytest.approx(10 * BUFFER / (RATE / 2), abs=BUFFER / (RATE / 2))
    window.engine.close_device()
