"""Freezing in the window: Ctrl+Shift+F freezes the selected tracks and
unfreezes them, the device view says why it shows no devices, the track menu's
freeze actions, and flattening."""

import numpy as np
import pytest
from PySide6.QtCore import Qt
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QMenu

from substation.model.project import Clip, MidiClip, Note
from substation.ui.arrangement.track_headers import add_freeze_actions

from .conftest import SAMPLE_RATE

CTRL_SHIFT = Qt.KeyboardModifier.ControlModifier | Qt.KeyboardModifier.ShiftModifier


@pytest.fixture(autouse=True)
def freeze_folder(tmp_path, monkeypatch):
    monkeypatch.setenv("SUBSTATION_RECORDINGS", str(tmp_path / "Recordings"))
    return tmp_path / "Recordings" / "Freeze"


def press_freeze(window) -> None:
    window.activateWindow()
    QTest.keyClick(window.arrangement.lanes, Qt.Key.Key_F, CTRL_SHIFT)
    QTest.qWait(10)


def audio_track(window, make_wav, name="A"):
    wav = make_wav(np.full((SAMPLE_RATE, 2), 0.5))
    window.engine.load_source(wav)
    track = window.editor.add_audio_track(name=name)
    window.editor._commit("Add", {track.id: [Clip(id=name + "c", path=wav, name=name, start_beat=0.0,
                                                  duration_sec=1.0, source_duration_sec=1.0)]})
    window.editor.add_device(track.id, "utility")
    return track.id


def test_ctrl_shift_f_freezes_and_unfreezes_the_selected_track(window, make_wav, freeze_folder):
    project = window.project
    track = audio_track(window, make_wav)
    window.selection.select_track(track, focus_track=True)
    assert window.devices.widgets  # its utility
    press_freeze(window)
    assert project.track(track).frozen is not None
    assert list(freeze_folder.glob("A Freeze *.wav"))
    assert not window.devices.widgets and "frozen" in window.devices.hint.text()
    assert window.statusBar().currentMessage().startswith("Froze A")
    window.grab()  # the header's snowflake, the lane's tint
    press_freeze(window)
    assert project.track(track).frozen is None and window.devices.widgets
    window.undo_stack.undo()
    assert project.track(track).frozen is not None


def test_a_frozen_track_says_why_an_edit_isnt_made(window, make_wav):
    track = audio_track(window, make_wav)
    window.selection.select_track(track, focus_track=True)
    press_freeze(window)
    window.editor.delete_clips([(track, "Ac")])
    assert "unfreeze" in window.statusBar().currentMessage()
    assert len(window.project.track(track).clips) == 1


def test_what_is_in_a_frozen_group_shows_no_devices(window, make_wav):
    track = audio_track(window, make_wav)
    group = window.editor.group_tracks([track])
    window.selection.select_track(group.id, focus_track=True)
    press_freeze(window)
    assert window.project.track(group.id).frozen is not None
    window.selection.select_track(track, focus_track=True)
    assert not window.devices.widgets and group.name in window.devices.hint.text()
    window.grab()


def test_the_track_menu_freezes_and_flattens(window, make_wav):
    project = window.project
    track = audio_track(window, make_wav)
    menu = QMenu()
    add_freeze_actions(menu, window.editor, window.bridge, window.selection, [track])
    freeze, flatten = menu.actions()
    assert freeze.text() == "Freeze Track" and freeze.isEnabled() and not flatten.isEnabled()
    freeze.trigger()
    assert project.track(track).frozen is not None
    menu = QMenu()
    add_freeze_actions(menu, window.editor, window.bridge, window.selection, [track])
    unfreeze, flatten = menu.actions()
    assert unfreeze.text() == "Unfreeze Track" and flatten.isEnabled()


def test_flattening_a_midi_track(window):
    project = window.project
    track = window.editor.add_midi_track(name="Keys")
    window.editor._commit("Add", {track.id: [MidiClip(id="m", name="m", start_beat=0.0, duration_beats=2.0,
                                                      notes=(Note(60, 0.0, 1.0),))]})
    window.selection.select_track(track.id, focus_track=True)
    press_freeze(window)
    window.flatten_tracks()
    flat = project.track(track.id)
    assert flat.is_audio and not flat.devices and len(flat.clips) == 1
    assert window.selection.track_id == track.id  # selected again
    header = window.arrangement.headers.headers[track.id]
    assert header.track.is_audio
    window.grab()
    window.undo_stack.undo()
    assert project.track(track.id).is_midi
