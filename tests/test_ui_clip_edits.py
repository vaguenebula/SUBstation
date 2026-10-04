"""Editing clips in the arrangement, through the real main window offscreen:
reversing audio clips (R), clip gain (its knob, its waveform), clips heard
where a drag takes them before it ends, time selections over groups, and the
take drawn while recording."""

from pathlib import Path

import numpy as np
import pytest
from PySide6.QtCore import QPoint, Qt
from PySide6.QtGui import QColor
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QLabel

from substation import theme
from substation.audio.engine_bridge import LiveTake
from substation.model.project import Clip
from substation.ui.arrangement.waveform_cache import quantized_gain, render_tile

from .conftest import SAMPLE_RATE
from .test_ui_smoke import drag, wait_until, write_wav

SPB = SAMPLE_RATE // 2  # samples per beat at 120 BPM


def audio_track(window, tmp_path, samples: np.ndarray, name: str = "ramp") -> tuple[str, str, str]:
    """An audio track with one clip at beat 0 playing `samples` (in both channels): (track, clip, file)."""
    path = str(write_wav(tmp_path / f"{name}.wav", np.stack([samples, samples], axis=1)))
    [(track_id, clip_id)] = window.editor.add_clips(None, 0.0, [(path, len(samples) / SAMPLE_RATE)])
    assert wait_until(lambda: window.bridge.source(path) is not None)
    QTest.qWait(10)
    return track_id, clip_id, path


def press_key(window, key, modifiers=Qt.KeyboardModifier.NoModifier) -> None:
    window.activateWindow()
    QTest.keyClick(window.arrangement.lanes, key, modifiers)
    QTest.qWait(10)


def test_r_reverses_the_selected_audio_clips_and_again_puts_them_back(window, tmp_path, monkeypatch):
    monkeypatch.setenv("SUBSTATION_RECORDINGS", str(tmp_path / "Recordings"))
    ramp = np.arange(2 * SAMPLE_RATE) / (2 * SAMPLE_RATE)  # rises from 0 to 1 over 2 s (4 beats)
    track_id, clip_id, path = audio_track(window, tmp_path, ramp)
    window.selection.select_clips(window.editor, [(track_id, clip_id)])

    press_key(window, Qt.Key.Key_R)
    clip = window.project.track(track_id).clips[0]
    assert Path(clip.path) == tmp_path / "Recordings" / "Reversed" / "ramp R.wav"  # a reversed copy, as Ableton makes
    assert (clip.id, clip.reversed_from, clip.offset_sec) == (clip_id, path, 0.0)
    assert window.undo_stack.undoText() == "Reverse Clip"
    source = window.bridge.source(path)
    backwards = window.bridge.source(clip.path)
    assert backwards is not None and backwards.frames == source.frames
    np.testing.assert_array_equal(np.array(backwards.samples(0, source.frames)),
                                  np.array(source.samples(0, source.frames))[:, ::-1])
    out = window.engine.render_offline(0.0, 4 * SPB)
    assert out[SPB, 0] == pytest.approx(0.75, abs=1e-3)  # half a second in: falling, from 1
    assert out[3 * SPB, 0] == pytest.approx(0.25, abs=1e-3)

    # Again: it plays its own file again (forwards).
    press_key(window, Qt.Key.Key_R)
    clip = window.project.track(track_id).clips[0]
    assert (clip.path, clip.reversed_from, clip.offset_sec) == (path, "", 0.0)
    assert window.engine.render_offline(0.0, 2 * SPB)[SPB, 0] == pytest.approx(0.25, abs=1e-3)
    window.undo_stack.undo()
    assert window.project.track(track_id).clips[0].reversed_from == path

    # A time range over part of it: just that part, split off and reversed (from the same copy).
    window.undo_stack.undo()
    window.selection.set_time_range(1.0, 2.0, [track_id], clips=set())
    press_key(window, Qt.Key.Key_R)
    clips = window.project.track(track_id).clips
    assert [(c.start_beat, Path(c.path).name) for c in clips] == [(0.0, "ramp.wav"), (1.0, "ramp R.wav"),
                                                                  (2.0, "ramp.wav")]
    assert len(list((tmp_path / "Recordings" / "Reversed").iterdir())) == 1
    assert clips[1].offset_sec == pytest.approx(1.0)  # it played seconds 0.5-1: 1-1.5 of the copy


def test_a_reversed_copy_saved_with_the_project_is_used_again(window, tmp_path, monkeypatch):
    monkeypatch.setenv("SUBSTATION_RECORDINGS", str(tmp_path / "Recordings"))
    ramp = np.arange(SAMPLE_RATE) / SAMPLE_RATE
    track_id, clip_id, path = audio_track(window, tmp_path, ramp)
    window.selection.select_clips(window.editor, [(track_id, clip_id)])
    press_key(window, Qt.Key.Key_R)
    copy = window.project.track(track_id).clips[0].path
    saved = tmp_path / "song.gilproj"
    assert window._save_to(saved)
    window.open_project(str(saved))  # (what this session wrote is forgotten)
    assert wait_until(lambda: window.bridge.source(path) is not None or window.bridge.request_source(path))
    # Another clip of the same file, reversed: the copy the project has is used again.
    [(other, other_clip)] = window.editor.add_clips(None, 8.0, [(path, 1.0)])
    window.selection.select_clips(window.editor, [(other, other_clip)])
    press_key(window, Qt.Key.Key_R)
    assert window.project.track(other).clips[0].path == copy
    assert [p.name for p in (Path(copy).parent).iterdir()] == ["ramp R.wav"]


def test_long_clips_reverse_in_the_background(window, tmp_path, monkeypatch):
    from substation.audio.engine_bridge import reversing
    from substation.ui import rendering
    from substation.ui.arrangement import lanes_canvas

    monkeypatch.setenv("SUBSTATION_RECORDINGS", str(tmp_path / "Recordings"))
    monkeypatch.setattr(lanes_canvas, "REVERSE_IN_PLACE_SECONDS", 0.0)  # (any clip is "long" here)
    monkeypatch.setattr(reversing, "REVERSE_CHUNK", 1000)  # (written in many pieces)
    ramp = np.arange(2 * SAMPLE_RATE) / (2 * SAMPLE_RATE)
    track_id, clip_id, path = audio_track(window, tmp_path, ramp)
    window.selection.select_clips(window.editor, [(track_id, clip_id)])
    seen, cancel = [], [True]
    follow = rendering.RenderProgress.follow

    def recording(dialog, job, label, part=(0, 1)):
        seen.append((dialog.windowTitle(), label))
        if cancel[0]:
            dialog.reject()
        follow(dialog, job, label, part)

    monkeypatch.setattr(rendering.RenderProgress, "follow", recording)
    undo_text = window.undo_stack.undoText()
    press_key(window, Qt.Key.Key_R)  # cancelled: nothing changes, no copy is left
    assert seen == [("Reverse Clips", "Reversing ramp.wav…")]
    assert window.undo_stack.undoText() == undo_text and window.project.track(track_id).clips[0].path == path
    assert not list((tmp_path / "Recordings" / "Reversed").iterdir())

    cancel[0] = False
    window.activateWindow()
    QTest.qWait(10)
    press_key(window, Qt.Key.Key_R)
    clip = window.project.track(track_id).clips[0]
    assert Path(clip.path).name == "ramp R.wav" and window.undo_stack.undoText() == "Reverse Clip"
    source, backwards = window.bridge.source(path), window.bridge.source(clip.path)
    np.testing.assert_array_equal(np.array(backwards.samples(0, source.frames)),
                                  np.array(source.samples(0, source.frames))[:, ::-1])


def test_reverse_in_the_menu_follows_the_selected_area(window, tmp_path, monkeypatch):
    from PySide6.QtGui import QContextMenuEvent
    from PySide6.QtWidgets import QMenu

    from substation.ui.arrangement import lanes_canvas

    track_id, clip_id, _path = audio_track(window, tmp_path, np.full(2 * SAMPLE_RATE, 0.5), name="dc")
    menus = []

    class Menu(QMenu):
        def exec(self, *_args):  # (not shown)
            menus.append({a.text(): a.isEnabled() for a in self.actions() if a.text()})

    monkeypatch.setattr(lanes_canvas, "QMenu", Menu)
    lanes, view = window.arrangement.lanes, window.arrangement.view
    row = window.arrangement.layout_model.row_for(track_id)
    window.selection.set_time_range(1.0, 2.0, [track_id], clips={(track_id, clip_id)})
    window.delete_selection()
    window.undo_stack.undo()  # the clip is back in the (still selected) area
    assert window.selection.clip_range and not window.selection.clips
    pos = QPoint(int(view.beat_to_x(1.5)), row.top - view.scroll_y + row.main_height - 4)
    lanes.contextMenuEvent(QContextMenuEvent(QContextMenuEvent.Reason.Mouse, pos, lanes.mapToGlobal(pos)))
    assert menus and menus[-1]["Reverse"]


def test_reversing_needs_audio_clips(window):
    track = window.editor.add_midi_track()
    ref = window.editor.add_midi_clip(track.id, 0.0, 4.0)
    window.selection.select_clips(window.editor, [ref])
    messages = []
    window.arrangement.lanes.status_message.connect(messages.append)
    press_key(window, Qt.Key.Key_R)
    assert messages == ["There are no audio clips in the selection to reverse."]
    assert window.undo_stack.undoText() == "Insert MIDI Clip"


def test_clip_gain_is_called_gain_and_makes_the_waveform_taller(window, tmp_path):
    square = np.where(np.arange(SAMPLE_RATE) % 2, 0.25, -0.25)  # its waveform: a band from -0.25 to 0.25
    track_id, clip_id, path = audio_track(window, tmp_path, square, name="quiet")
    clip_view = window.arrangement.clip_view
    window.arrangement.open_clips([(track_id, clip_id)])
    captions = [label.text() for label in clip_view.gain.findChildren(QLabel)]
    assert "Gain" in captions and "Volume" not in captions
    clip_view.gain.knob.valueChanged.emit(6.0, object())
    assert window.project.track(track_id).clips[0].gain_db == 6.0
    assert window.undo_stack.undoText() == "Change Clip Gain"
    clip_view.close_view()

    source = window.bridge.source(path)
    argb = QColor(theme.WAVEFORM).rgba()

    def drawn_rows(gain: float) -> int:
        image = render_tile(source, 100.0, 0, 64, False, argb, gain)
        return sum(any(image.pixelColor(x, y).alpha() for x in range(0, 256, 16)) for y in range(64))

    assert drawn_rows(1.0) == pytest.approx(16, abs=2)  # a quarter of the lane's height each way
    assert drawn_rows(2.0) == pytest.approx(32, abs=2)  # twice as loud: twice as tall
    assert drawn_rows(8.0) >= 62  # too loud for the lane: cut off at its edges
    # Drawn at 0.1 dB steps: the same tiles for gains that look the same, a quiet one not flat.
    assert quantized_gain(1.004) == quantized_gain(1.0) == pytest.approx(1.0)
    assert quantized_gain(10 ** (-60 / 20)) == pytest.approx(0.001) and quantized_gain(0.0) == 0.0
    window.grab()  # (the arrangement draws it with its gain)


def test_a_dragged_clip_is_heard_where_it_goes_before_it_is_dropped(window, tmp_path):
    track_id, _clip_id, _path = audio_track(window, tmp_path, np.full(2 * SAMPLE_RATE, 0.5), name="dc")  # beats 0-4
    lanes, view = window.arrangement.lanes, window.arrangement.view
    row = window.arrangement.layout_model.row_for(track_id)
    title_y = row.top - view.scroll_y + 6

    def heard_at(beat: float) -> float:
        return float(window.engine.render_offline(0.0, 10 * SPB)[int(beat * SPB), 0])

    start = QPoint(int(view.beat_to_x(1.0)), title_y)
    QTest.mousePress(lanes, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, start)
    QTest.mouseMove(lanes, start + QPoint(10, 0))
    QTest.mouseMove(lanes, QPoint(int(view.beat_to_x(5.0)), title_y))  # four beats on
    assert window.project.track(track_id).clips[0].start_beat == 0.0  # the model waits for the drop...
    assert heard_at(1.0) == 0.0 and heard_at(6.0) == pytest.approx(0.5)  # ...the engine plays it there now
    # Back where it was, and dropped: nothing changed, and it plays there again.
    QTest.mouseMove(lanes, start)
    QTest.mouseRelease(lanes, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, start)
    assert window.undo_stack.undoText() == "Add Clip"
    assert heard_at(1.0) == pytest.approx(0.5) and heard_at(6.0) == 0.0

    drag(lanes, start, QPoint(int(view.beat_to_x(5.0)), title_y))
    assert window.project.track(track_id).clips[0].start_beat == 4.0
    assert heard_at(1.0) == 0.0 and heard_at(6.0) == pytest.approx(0.5)
    window.undo_stack.undo()
    assert heard_at(1.0) == pytest.approx(0.5)

    # Trimming too: the clip plays trimmed while its edge is dragged.
    edge = QPoint(int(view.beat_to_x(4.0)) - 2, title_y)
    QTest.mousePress(lanes, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, edge)
    QTest.mouseMove(lanes, QPoint(int(view.beat_to_x(2.0)), title_y))
    assert heard_at(1.0) == pytest.approx(0.5) and heard_at(3.0) == 0.0
    QTest.mouseRelease(lanes, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier,
                       QPoint(int(view.beat_to_x(2.0)), title_y))
    assert window.project.track(track_id).clips[0].end_beat(window.project.tempo) == pytest.approx(2.0)


def test_a_time_selection_over_a_group_takes_in_its_tracks(window):
    editor, project = window.editor, window.project
    a, b, c = (editor.add_audio_track(name=n).id for n in "ABC")
    for i, track_id in enumerate((a, b, c)):
        editor._commit("Add", {track_id: [Clip(id=f"c{i}", path="missing.wav", name="x", start_beat=0.0,
                                               duration_sec=2.0, source_duration_sec=2.0)]})
    group = editor.group_tracks([a, b]).id
    editor.set_folded(group, True)  # (its tracks hidden: still in it)
    QTest.qWait(10)
    lanes, view = window.arrangement.lanes, window.arrangement.view
    row = window.arrangement.layout_model.row_for(group)
    y = row.top - view.scroll_y + row.main_height // 2

    # Dragged along the group's lane, a selection takes in everything in the group.
    drag(lanes, QPoint(int(view.beat_to_x(1.0)), y), QPoint(int(view.beat_to_x(3.0)), y))
    assert window.selection.time_range == (1.0, 3.0, (group, a, b))
    assert window.selection.clips == {(a, "c0"), (b, "c1")}
    window.grab()
    window.delete_selection()
    tempo = project.tempo
    for track_id in (a, b):
        assert [(cl.start_beat, cl.end_beat(tempo)) for cl in project.track(track_id).clips] == [(0.0, 1.0),
                                                                                                (3.0, 4.0)]
    assert len(project.track(c).clips[0:]) == 1 and project.track(c).clips[0].end_beat(tempo) == 4.0
    window.undo_stack.undo()

    # Copy and paste it further on: onto the group's tracks again.
    window.copy()
    window.selection.set_insert(8.0)
    window.paste()
    assert window.selection.time_range == (8.0, 10.0, (a, b))
    for track_id in (a, b):
        assert [cl.start_beat for cl in project.track(track_id).clips] == [0.0, 8.0]
    window.undo_stack.undo()

    # Ctrl+A: from the first clip to the last, on every track.
    press_key(window, Qt.Key.Key_A, Qt.KeyboardModifier.ControlModifier)
    assert window.selection.time_range == (0.0, 4.0, (group, a, b, c))


def test_a_take_is_drawn_up_to_the_playhead(window):
    track_id = window.editor.add_audio_track().id
    lanes, view = window.arrangement.lanes, window.arrangement.view
    row = window.arrangement.layout_model.row_for(track_id)
    y = row.top - view.scroll_y + 6  # its red title bar
    # Half a beat has come in (the input lags): the take still reaches the playhead, at beat 2.
    window.bridge.live_takes = {track_id: LiveTake(track_id, start_sample=0, started=True, frames=SPB // 2)}
    lanes.set_playhead(2.0)
    image = lanes.grab().toImage()
    red = QColor(theme.RECORD_ON)
    assert image.pixelColor(int(view.beat_to_x(1.5)), y) == red
    assert image.pixelColor(int(view.beat_to_x(2.5)), y) != red
    window.bridge.live_takes = {}
    lanes.set_playhead(None)
