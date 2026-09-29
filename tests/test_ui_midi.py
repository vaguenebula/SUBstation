"""MIDI tracks, clips and the piano roll, driven through the real main window offscreen."""

import numpy as np
import pytest
from PySide6.QtCore import QPoint, QPointF, Qt
from PySide6.QtGui import QDropEvent, QMouseEvent, QWheelEvent
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication

from gilstudio.model.project import MidiClip, Note
from gilstudio.ui.browser.browser_models import device_kinds

from .conftest import SAMPLE_RATE, write_wav
from .test_ui_smoke import drag, tone

SPB = SAMPLE_RATE // 2  # samples per beat at 120 BPM


def lane_point(window, track_index: int, beat: float, y: str = "body") -> QPoint:
    arrangement = window.arrangement
    row = arrangement.layout_model.rows[track_index]
    top = row.top - arrangement.view.scroll_y
    return QPoint(int(arrangement.view.beat_to_x(beat)), top + (6 if y == "title" else row.height // 2))


def ctrl_drag(widget, start: QPoint, end: QPoint, ctrl=Qt.KeyboardModifier.ControlModifier) -> None:
    """Like drag(), holding Ctrl (or `ctrl`) throughout (QTest.mouseMove can't send modifiers)."""
    QTest.mousePress(widget, Qt.MouseButton.LeftButton, ctrl, start)
    for point in (start + (end - start) / 2, end):
        QApplication.sendEvent(widget, QMouseEvent(QMouseEvent.Type.MouseMove, QPointF(point),
                                                   QPointF(widget.mapToGlobal(point)), Qt.MouseButton.NoButton,
                                                   Qt.MouseButton.LeftButton, ctrl))
    QTest.mouseRelease(widget, Qt.MouseButton.LeftButton, ctrl, end)


def cell(roll, beat: float, pitch: int) -> QPoint:
    """Just inside the grid cell of a beat and key in the piano roll."""
    return QPoint(int(roll.view.beat_to_x(beat) + 3), int(roll.pitch_top(pitch) + roll.row_height / 2))


@pytest.fixture
def midi_clip(window):
    """A MIDI track with a one-bar clip at beat 4, open in the piano roll (as by double-clicking the lane)."""
    window.insert_midi_track()
    QTest.mouseDClick(window.arrangement.lanes, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier,
                      lane_point(window, 0, 4.5))
    track = window.project.tracks[0]
    [clip] = track.clips
    return track, clip, window.arrangement.clip_view.piano_roll


def test_midi_track_with_the_synth_and_a_clip_in_the_piano_roll(window, midi_clip):
    track, clip, roll = midi_clip
    clip_view = window.arrangement.clip_view
    assert track.is_midi and [d.kind for d in track.devices] == ["synth"]
    assert window.devices.widgets  # the Synth shows in the device view...
    [synth] = window.devices.widgets.values()
    assert synth.choices["wave"].currentText() == "Saw" and "cutoff" in synth.knobs  # ...with a list and knobs
    # Double-clicking empty space on a MIDI track made a one-bar clip at the grid line and opened it.
    assert isinstance(clip, MidiClip) and (clip.start_beat, clip.duration_beats) == (4.0, 4.0)
    assert clip_view.isVisible() and clip_view.midi
    assert clip_view.body.currentWidget() is roll  # just the piano roll: no audio controls
    assert not clip_view.audio_page.isVisible()
    assert roll.grid.hasFocus()
    assert clip_view.name.text() == track.name and "0 notes" in clip_view.info.text()
    # Fitted to the clip: its four beats fill the width.
    assert roll.view.beat_to_x(4.0) == pytest.approx(roll.grid.width() * 0.96, abs=1)
    assert not clip_view.grab().isNull()


def test_notes_drawn_in_the_piano_roll_are_heard(window, midi_clip):
    track, clip, roll = midi_clip
    for beat, pitch in ((0.0, 69), (2.0, 76)):  # A3 at the clip start, E4 two beats in
        QTest.mouseDClick(roll.grid, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, cell(roll, beat, pitch))
    notes = window.project.clip(track.id, clip.id).notes
    step = roll.view.grid_step()
    assert notes == (Note(69, 0.0, step), Note(76, 2.0, step))
    assert roll.selected == {notes[1]}  # the new note is selected
    assert "2 notes" in window.arrangement.clip_view.info.text()

    out = window.engine.render_offline(0.0, 8 * SPB)[:, 0]
    first = int(np.nonzero(np.abs(out) > 1e-6)[0][0])
    assert 4 * SPB <= first < 4 * SPB + 10  # clip at beat 4, note at its start
    assert np.abs(out[6 * SPB : 6 * SPB + 2000]).max() > 0.02  # the second note, two beats later
    # Clicking the ruler plays from there: two beats into the clip is beat 6 of the song.
    QTest.mouseClick(roll.ruler, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier,
                     QPoint(int(roll.view.beat_to_x(2.0)), 10))
    assert window.bridge.position == pytest.approx(6.0)
    assert roll.playhead == pytest.approx(2.0)
    assert window.undo_stack.undoText() == "Add Note"
    window.undo_stack.undo()
    assert len(window.project.clip(track.id, clip.id).notes) == 1
    assert roll.selected == set()  # the selected note is gone


def test_dragging_moves_resizes_and_copies_notes(window, midi_clip):
    track, clip, roll = midi_clip
    grid = roll.grid
    QTest.mouseDClick(grid, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, cell(roll, 1.0, 60))
    [note] = window.project.clip(track.id, clip.id).notes
    depth = window.undo_stack.count()
    # Move: one beat later and a fifth up, as one undo step.
    start = cell(roll, 1.0, 60) + QPoint(8, 0)
    drag(grid, start, start + QPoint(int(roll.view.px_per_beat), -7 * roll.row_height))
    [moved] = window.project.clip(track.id, clip.id).notes
    assert (moved.start, moved.pitch, moved.length) == (2.0, 67, note.length)
    assert window.undo_stack.count() == depth + 1
    # Resize the end by one beat.
    rect = grid.note_rect(moved)
    edge = QPoint(int(rect.right()) - 2, int(rect.center().y()))
    assert grid.note_at(QPointF(edge))[1] == "end"
    drag(grid, edge, edge + QPoint(int(roll.view.px_per_beat), 0))
    [resized] = window.project.clip(track.id, clip.id).notes
    assert resized.length == pytest.approx(note.length + 1.0)
    # Ctrl-drag copies (the selected note: Ctrl-clicking it would deselect it).
    body = QPoint(int(rect.center().x()), int(rect.center().y()))
    ctrl_drag(grid, body, body + QPoint(0, 2 * roll.row_height))
    assert [n.pitch for n in window.project.clip(track.id, clip.id).notes] == [65, 67]
    assert {n.pitch for n in roll.selected} == {65}  # the copy
    window.undo_stack.undo()
    window.undo_stack.undo()
    window.undo_stack.undo()
    assert window.project.clip(track.id, clip.id).notes == (note,)


def test_piano_roll_keys_take_precedence_over_arrangement_shortcuts(window, midi_clip):
    track, clip, roll = midi_clip
    grid = roll.grid
    for beat in (0.0, 1.0):
        QTest.mouseDClick(grid, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, cell(roll, beat, 60))
    window.selection.set_clips({(track.id, clip.id)})  # the arrangement clip is selected too
    grid.setFocus()
    QTest.keyClick(grid, Qt.Key.Key_A, Qt.KeyboardModifier.ControlModifier)
    assert len(roll.selected) == 2
    QTest.keyClick(grid, Qt.Key.Key_Up, Qt.KeyboardModifier.ShiftModifier)  # an octave up
    assert {n.pitch for n in window.project.clip(track.id, clip.id).notes} == {72}
    QTest.keyClick(grid, Qt.Key.Key_D, Qt.KeyboardModifier.ControlModifier)  # duplicate after them
    notes = window.project.clip(track.id, clip.id).notes
    assert [n.start for n in notes] == [0.0, 1.0, 1.25, 2.25]
    QTest.keyClick(grid, Qt.Key.Key_Delete)  # deletes the selected notes, not the clip
    assert len(window.project.clip(track.id, clip.id).notes) == 2
    assert len(track.clips) == 1
    QTest.keyClick(grid, Qt.Key.Key_Escape)
    assert not window.arrangement.clip_view.isVisible()


def test_rubber_band_keys_and_velocity_lane(window, midi_clip):
    track, clip, roll = midi_clip
    grid = roll.grid
    for beat, pitch in ((0.0, 60), (1.0, 62), (2.0, 64)):
        QTest.mouseDClick(grid, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, cell(roll, beat, pitch))
    notes = window.project.clip(track.id, clip.id).notes
    # Rubber band around the first two.
    drag(grid, cell(roll, 0.0, 63) - QPoint(2, 0), cell(roll, 1.5, 59))
    assert roll.selected == set(notes[:2])
    # Ctrl-clicking a selected note deselects it.
    QTest.mouseClick(grid, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.ControlModifier, cell(roll, 0.0, 60))
    assert roll.selected == {notes[1]}
    # Clicking a key selects the notes on it.
    QTest.mouseClick(roll.keys, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier,
                     QPoint(10, int(roll.pitch_top(64) + 4)))
    assert roll.selected == {notes[2]}
    # Dragging a stem down changes the velocity (of the selected notes, when it is one of theirs).
    lane = roll.velocity
    x = int(roll.view.beat_to_x(2.0))
    top = QPoint(x, int(lane.velocity_y(100)))
    drag(lane, top, top + QPoint(0, int(lane._span() / 2)))
    velocities = [n.velocity for n in window.project.clip(track.id, clip.id).notes]
    assert velocities[:2] == [100, 100] and velocities[2] == pytest.approx(37, abs=2)
    assert window.undo_stack.undoText() == "Change Velocity"


def wheel(widget, pos: QPoint, notches: float, mods) -> None:
    QApplication.sendEvent(widget, QWheelEvent(QPointF(pos), QPointF(widget.mapToGlobal(pos)), QPoint(),
                                               QPoint(0, int(notches * 120)), Qt.MouseButton.NoButton, mods,
                                               Qt.ScrollPhase.NoScrollPhase, False))


def test_alt_wheel_resizes_the_keys_and_ctrl_alt_drag_scrolls(window, midi_clip):
    track, clip, roll = midi_clip
    grid, alt = roll.grid, Qt.KeyboardModifier.AltModifier
    at = cell(roll, 1.0, 64)
    scroll_beats, height = roll.view.scroll_beats, roll.row_height
    # Alt+wheel over the grid (or the keys) makes the rows taller, keeping the key under the mouse there.
    wheel(grid, at, 4, alt)
    assert roll.row_height > height and roll.view.scroll_beats == scroll_beats
    assert roll.pitch_at(at.y()) == 64
    taller = roll.row_height
    wheel(roll.keys, QPoint(10, at.y()), -8, alt)
    assert roll.row_height < taller and roll.pitch_at(at.y()) == 64
    for _ in range(50):
        wheel(grid, at, -1, alt)
    assert roll.row_height == 5  # no shorter than this
    # Ctrl+Alt drag scrolls both ways, and draws no note or rubber band.
    ctrl_alt = Qt.KeyboardModifier.ControlModifier | alt
    start = cell(roll, 2.0, roll.pitch_at(grid.height() / 2))
    beat, y = roll.view.scroll_beats, roll.view.scroll_y
    ctrl_drag(grid, start, start + QPoint(-40, 30), ctrl_alt)
    assert roll.view.scroll_beats == pytest.approx(beat + 40 / roll.view.px_per_beat)
    assert 0 < roll.view.scroll_y == y - 30
    assert not window.project.clip(track.id, clip.id).notes and not roll.selected


def test_legato_quantize_and_humanize_buttons(window, midi_clip):
    track, clip, roll = midi_clip
    tools = roll.tools

    def clip_notes():
        return window.project.clip(track.id, clip.id).notes

    assert not tools.legato.isEnabled() and not tools.quantize.isEnabled()  # nothing to act on yet
    played = [Note(60, 0.1, 0.25), Note(64, 1.05, 0.25), Note(67, 1.9, 0.25), Note(72, 3.2, 0.25)]
    window.editor.set_clip_notes((track.id, clip.id), played, "setup")
    assert tools.legato.isEnabled() and tools.humanize.isEnabled()

    # With nothing selected, Quantize (to 1/16 by default) moves every note.
    depth = window.undo_stack.count()
    tools.quantize.click()
    assert [n.start for n in clip_notes()] == [0.0, 1.0, 2.0, 3.25]
    assert window.undo_stack.count() == depth + 1 and window.undo_stack.undoText() == "Quantize"
    window.undo_stack.undo()
    # Ctrl+U does the same, here to 1/4 at half strength.
    tools.grid.setCurrentText("1/4")
    tools.amount.setValue(50.0)
    QTest.keyClick(roll.grid, Qt.Key.Key_U, Qt.KeyboardModifier.ControlModifier)
    assert [n.start for n in clip_notes()] == pytest.approx([0.05, 1.025, 1.95, 3.1])
    window.undo_stack.undo()

    # Legato acts on the selected notes: each reaches the next, the last the clip's end.
    roll.set_selection(clip_notes()[:3])
    tools.legato.click()
    assert [(n.start, n.length) for n in clip_notes()] == [
        (0.1, pytest.approx(0.95)), (1.05, pytest.approx(0.85)), (1.9, pytest.approx(2.1)), (3.2, 0.25)]
    assert roll.selected == set(clip_notes()[:3])  # still selected
    window.undo_stack.undo()

    # Humanize moves starts and velocities a little, lengths stay.
    roll.set_selection(set())
    roll._rng.seed(3)
    tools.humanize_amount.setValue(100.0)
    before = clip_notes()
    tools.humanize.click()
    after = sorted(clip_notes(), key=lambda n: n.pitch)
    assert after != sorted(before, key=lambda n: n.pitch)
    for old, new in zip(before, after, strict=True):
        assert abs(new.start - old.start) <= 0.125 and abs(new.velocity - old.velocity) <= 24
        assert new.length == old.length
    assert window.undo_stack.undoText() == "Humanize"
    assert not window.arrangement.clip_view.grab().isNull()


def test_midi_clips_in_the_arrangement(window, midi_clip, tmp_path):
    track, clip, _ = midi_clip
    project, editor = window.project, window.editor
    editor.set_clip_notes((track.id, clip.id), [Note(60, 0.0, 1.0), Note(64, 2.0, 1.0)], "setup")
    window.arrangement.clip_view.close_view()
    assert not window.arrangement.lanes.grab().isNull()  # draws the notes inside the clip
    # Split at beat 6: each piece plays its own note; the notes stay in both.
    window.selection.set_clips({(track.id, clip.id)})
    window.selection.set_insert(6.0)
    window.split()
    left, right = project.track(track.id).clips
    assert [len(c.played_notes()) for c in (left, right)] == [1, 1]
    # Ctrl+Shift+M on a time range makes a clip there.
    window.selection.set_time_range(12.0, 14.0, (track.id,), clips=set())
    window.insert_midi_clip()
    assert [(c.start_beat, c.end_beat()) for c in project.track(track.id).clips][-1] == (12.0, 14.0)
    # An audio clip can't be dragged onto the MIDI track.
    path = str(write_wav(tmp_path / "a.wav", tone(1.0, 220.0)))
    audio_ref = editor.add_clips(None, 0.0, [(path, 1.0)], track_index=1)[0]
    lanes = window.arrangement.lanes
    start = lane_point(window, 1, 0.5, "title")
    drag(lanes, start, start - QPoint(0, window.arrangement.layout_model.rows[0].height))
    assert audio_ref in {(t.id, c.id) for t in project.tracks for c in t.clips}  # still on its audio track
    # Saving and opening keeps it all.
    target = tmp_path / "midi.gilproj"
    assert window._save_to(target)
    window.new_project()
    window.open_project(str(target))
    reopened = window.project.tracks[0]
    assert reopened.is_midi and len(reopened.clips) == 3 and [d.kind for d in reopened.devices] == ["synth"]
    out = window.engine.render_offline(0.0, 8 * SPB)
    assert np.abs(out[4 * SPB : 5 * SPB]).max() > 0.02


def test_instruments_from_the_browser(window):
    browser, project = window.browser, window.project
    [builtin] = [browser.sidebar.topLevelItem(i) for i in range(browser.sidebar.topLevelItemCount())
                 if browser.sidebar.topLevelItem(i).text(0) == "Built-in"]
    browser.sidebar.setCurrentItem(builtin.child(0))
    assert builtin.child(0).text(0) == "Instruments"
    index = browser.list_model.index(0)
    assert browser.list_model.item(index).name == "Synth"
    # With an audio track selected, double-clicking an instrument makes a MIDI track for it.
    window.insert_track()
    browser._activate_list(index)
    assert [t.kind for t in project.tracks] == ["audio", "midi"]
    assert window.selection.track_id == project.tracks[1].id
    # Dropped on an audio track it is refused; below the tracks it makes a MIDI track.
    mime = browser.list_model.mimeData([index])
    assert device_kinds(mime) == ["synth"]
    lanes = window.arrangement.lanes
    actions, buttons, mods = Qt.DropAction.CopyAction, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier
    lanes.dropEvent(QDropEvent(QPointF(lane_point(window, 0, 1.0)), actions, mime, buttons, mods))
    assert project.tracks[0].devices == []
    assert "MIDI" in window.statusBar().currentMessage()
    below = QPointF(40, window.arrangement.layout_model.total_height + 20)
    lanes.dropEvent(QDropEvent(below, actions, mime, buttons, mods))
    assert [t.kind for t in project.tracks] == ["audio", "midi", "midi"]
    assert [d.kind for d in project.tracks[2].devices] == ["synth"]
    # A MIDI track without an instrument asks for one.
    window.editor.remove_device(project.tracks[2].id, project.tracks[2].devices[0].id)
    assert "instrument" in window.devices.hint.text() and not window.devices.hint.isHidden()
