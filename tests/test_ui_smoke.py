"""Builds the real main window offscreen and drives it like a user would."""

import time
from dataclasses import replace

import numpy as np
import pytest
from PySide6.QtCore import QPoint, QPointF, Qt
from PySide6.QtGui import QMouseEvent
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication, QPushButton

from gilstudio import _engine as ge
from gilstudio.audio.engine_bridge import clip_desc
from gilstudio.ui.clip_view import WARP_MODES

from .conftest import SAMPLE_RATE, write_wav


def wait_until(predicate, timeout=5.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        QTest.qWait(10)
    return False


def tone(seconds: float, freq: float) -> np.ndarray:
    t = np.arange(int(seconds * SAMPLE_RATE)) / SAMPLE_RATE
    return np.stack([0.5 * np.sin(2 * np.pi * freq * t), 0.3 * np.sin(2 * np.pi * freq * 1.5 * t)], axis=1)


@pytest.fixture
def three_tracks(window, tmp_path):
    paths = [str(write_wav(tmp_path / f"tone{i}.wav", tone(2.0 + i, 220.0 * (i + 1)))) for i in range(3)]
    for i, path in enumerate(paths):
        window.editor.add_clips(None, i * 2.0, [(path, window.bridge.file_info(path).duration)],
                                track_index=len(window.project.tracks))
    assert wait_until(lambda: all(window.bridge.source(p) is not None for p in paths))
    QTest.qWait(20)
    return paths


def test_arrangement_renders_and_mirrors_engine(window, three_tracks):
    project = window.project
    assert [t.name for t in project.tracks] == ["tone0", "tone1", "tone2"]
    assert len(window.arrangement.headers.headers) == 3
    image = window.grab()
    assert not image.isNull() and image.width() >= 1000
    # The engine plays what the model says: clip 2 starts at beat 4 (2 s at 120 BPM).
    out = window.engine.render_offline(0.0, 3 * SAMPLE_RATE)
    assert np.abs(out[: SAMPLE_RATE // 2]).max() > 0.1  # clip 0 at beat 0
    window.editor.set_track_param(project.tracks[0].id, "mute", True)
    window.editor.set_track_param(project.tracks[1].id, "mute", True)
    quiet = window.engine.render_offline(0.0, 3 * SAMPLE_RATE)
    assert np.abs(quiet[: 2 * SAMPLE_RATE]).max() == 0.0  # clip 2 only starts at 2 s
    assert np.abs(quiet[2 * SAMPLE_RATE + 1000 :]).max() > 0.1


def test_mouse_drag_moves_clip_and_undo_restores(window, three_tracks):
    arrangement = window.arrangement
    lanes = arrangement.lanes
    view = arrangement.view
    track = window.project.tracks[0]
    clip = track.clips[0]
    row = arrangement.layout_model.rows[0]
    start = QPoint(int(view.beat_to_x(clip.start_beat) + 30), row.top - view.scroll_y + 6)  # title bar
    end = start + QPoint(int(4 * view.px_per_beat), 0)
    QTest.mousePress(lanes, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, start)
    QTest.mouseMove(lanes, start + QPoint(10, 0))
    QTest.mouseMove(lanes, end)
    QTest.mouseRelease(lanes, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, end)
    moved = window.project.track(track.id).clips[0]
    assert moved.id == clip.id
    assert moved.start_beat == pytest.approx(4.0)
    assert window.selection.clips == {(track.id, clip.id)}
    assert window.selection.insert_beat == pytest.approx(4.0)  # playback follows the moved clip
    window.undo_stack.undo()
    assert window.project.track(track.id).clips[0].start_beat == 0.0


def test_edit_commands(window, three_tracks):
    project = window.project
    track = project.tracks[0]
    window.selection.set_clips({(track.id, track.clips[0].id)}, track_id=track.id)
    window.selection.set_insert(1.0)
    window.split()
    assert [round(c.start_beat, 6) for c in project.track(track.id).clips] == [0.0, 1.0]

    window.select_all()
    assert len(window.selection.clips) == 4
    window.duplicate()
    assert sum(len(t.clips) for t in project.tracks) == 8
    window.delete_selection()
    assert sum(len(t.clips) for t in project.tracks) == 4

    window.insert_track()
    assert len(project.tracks) == 4
    window.delete_track()
    assert len(project.tracks) == 3
    assert window.windowTitle().startswith("Untitled*")


def test_zoom_scroll_and_follow(window, three_tracks):
    view = window.arrangement.view
    before = view.px_per_beat
    window.arrangement.zoom(2.0)
    assert view.px_per_beat == pytest.approx(before * 2.0)
    window.arrangement.zoom_to_arrangement()
    assert view.scroll_beats == 0.0
    view.set_scroll_beats(8.0)
    assert window.arrangement.hbar.value() == int(8.0 * view.px_per_beat)
    window.arrangement.vbar.setValue(10**6)
    assert view.scroll_y == view.max_scroll_y
    window.grab()


def test_transport_and_locate(window, three_tracks):
    window.locate(6.0)
    assert window.bridge.position == pytest.approx(6.0)
    window.toggle_play()
    assert window.engine.is_playing
    window.toggle_play()
    assert not window.engine.is_playing
    assert window.bridge.position == pytest.approx(6.0)  # returned to where playback started
    window.stop_button()
    assert window.bridge.position == 0.0


def test_devices_and_mixer_reach_engine(window, three_tracks):
    track = window.project.tracks[0]
    window.selection.select_track(track.id)
    device = window.editor.add_device(track.id, "utility")
    assert device.id in window.devices.widgets
    engine_id = window.bridge.engine_device_id(track.id, device.id)
    window.editor.set_device_param(track.id, device.id, "gain", -12.0)
    assert window.engine.processor_param(engine_id, 0) == pytest.approx(-12.0)
    window.undo_stack.undo()
    assert window.engine.processor_param(engine_id, 0) == pytest.approx(0.0)
    window.editor.remove_device(track.id, device.id)
    assert window.devices.widgets == {}


def test_builtin_devices_in_browser(window, three_tracks):
    from PySide6.QtCore import QPointF
    from PySide6.QtGui import QDropEvent

    from gilstudio.ui.browser.browser_models import device_kinds

    browser, devices = window.browser, window.devices
    tracks = window.project.tracks
    assert not hasattr(devices, "add_button")  # devices come from the browser now
    # Built-in > Audio Effects > Utility
    [builtin] = [browser.sidebar.topLevelItem(i) for i in range(browser.sidebar.topLevelItemCount())
                 if browser.sidebar.topLevelItem(i).text(0) == "Built-in"]
    categories = {builtin.child(i).text(0): builtin.child(i) for i in range(builtin.childCount())}
    assert list(categories) == ["Instruments", "Audio Effects"] and builtin.isExpanded()
    audio_effects = categories["Audio Effects"]
    browser.sidebar.setCurrentItem(audio_effects)
    assert browser.list_model.rowCount() == 1
    index = browser.list_model.index(0)
    assert browser.list_model.item(index).name == "Utility"
    mime = browser.list_model.mimeData([index])
    assert device_kinds(mime) == ["utility"]

    # Double-click adds it to the selected track...
    window.selection.select_track(tracks[0].id)
    browser._activate_list(index)
    assert [d.kind for d in tracks[0].devices] == ["utility"]
    # ...dropping on the device view adds it to the track shown there...
    actions, buttons, mods = Qt.DropAction.CopyAction, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier
    devices.dropEvent(QDropEvent(QPointF(20, 20), actions, mime, buttons, mods))
    assert len(tracks[0].devices) == 2 and len(devices.widgets) == 2
    # ...and dropping on a track in the arrangement adds it to that track.
    lanes, row = window.arrangement.lanes, window.arrangement.layout_model.rows[2]
    lanes.dropEvent(QDropEvent(QPointF(40, row.top - window.arrangement.view.scroll_y + 10), actions, mime,
                               buttons, mods))
    assert [d.kind for d in tracks[2].devices] == ["utility"]
    assert window.selection.track_id == tracks[2].id and len(devices.widgets) == 1
    assert devices.minimumHeight() == devices.maximumHeight()  # fixed height, not resizable


def test_save_open_roundtrip(window, three_tracks, tmp_path):
    path = tmp_path / "song.gilproj"
    window.editor.set_tempo(98.0)
    assert window._save_to(path)
    assert window.windowTitle().startswith("song -")
    window.new_project()
    assert window.project.tracks == []
    window.open_project(str(path))
    assert len(window.project.tracks) == 3
    assert window.project.tempo == 98.0
    assert window.engine.tempo == 98.0


def test_browser_indexes_and_searches(window, tmp_path):
    folder = tmp_path / "Drums"
    folder.mkdir()
    write_wav(folder / "Kick Deep.wav", tone(0.2, 60.0))
    write_wav(folder / "Snare Tight.wav", tone(0.2, 200.0))
    (folder / "notes.txt").write_text("not audio")
    browser = window.browser
    browser.index.rebuild([str(tmp_path)])
    assert wait_until(lambda: not browser.index.indexing and len(browser.index.audio) >= 2)
    browser.search.setText("kick")
    browser._refresh()
    assert browser.list_model.rowCount() == 1
    item = browser.list_model.item(browser.list_model.index(0))
    assert item.name == "Kick Deep.wav"
    mime = browser.list_model.mimeData([browser.list_model.index(0)])
    assert mime.urls()[0].toLocalFile().endswith("Kick Deep.wav")
    browser.file_activated.emit(item.path)
    assert window.project.tracks[-1].clips[0].name == "Kick Deep"


def drag(widget, start: QPoint, end: QPoint, modifiers=Qt.KeyboardModifier.NoModifier):
    QTest.mousePress(widget, Qt.MouseButton.LeftButton, modifiers, start)
    QTest.mouseMove(widget, start + (end - start) / 2)
    QTest.mouseMove(widget, end)
    QTest.mouseRelease(widget, Qt.MouseButton.LeftButton, modifiers, end)


def ctrl_drag(widget, start: QPoint, end: QPoint) -> None:
    """Like drag(), holding Ctrl throughout (QTest.mouseMove can't send modifiers)."""
    ctrl = Qt.KeyboardModifier.ControlModifier
    QTest.mousePress(widget, Qt.MouseButton.LeftButton, ctrl, start)
    for point in (start + (end - start) / 2, end):
        QApplication.sendEvent(widget, QMouseEvent(QMouseEvent.Type.MouseMove, QPointF(point),
                                                   QPointF(widget.mapToGlobal(point)), Qt.MouseButton.NoButton,
                                                   Qt.MouseButton.LeftButton, ctrl))
    QTest.mouseRelease(widget, Qt.MouseButton.LeftButton, ctrl, end)


def test_ruler_loop_brace_and_scrub_zoom(window):
    arrangement = window.arrangement
    ruler, view = arrangement.ruler, arrangement.view
    y = 6  # loop strip
    project = window.project
    assert (project.loop_start, project.loop_end) == (0.0, 16.0)  # the default brace
    # Dragging outside the brace draws a new loop and enables it.
    drag(ruler, QPoint(int(view.beat_to_x(20.0)), y), QPoint(int(view.beat_to_x(28.0)), y))
    assert (project.loop_enabled, project.loop_start, project.loop_end) == (True, 20.0, 28.0)
    # Dragging the brace body moves it; the whole gesture is one undo step.
    drag(ruler, QPoint(int(view.beat_to_x(24.0)), y), QPoint(int(view.beat_to_x(22.0)), y))
    assert (project.loop_start, project.loop_end) == (18.0, 26.0)
    window.undo_stack.undo()
    assert (project.loop_start, project.loop_end) == (20.0, 28.0)
    # Dragging an edge resizes it.
    drag(ruler, QPoint(int(view.beat_to_x(28.0)), y), QPoint(int(view.beat_to_x(32.0)), y))
    assert (project.loop_start, project.loop_end) == (20.0, 32.0)

    before = view.px_per_beat
    scrub_y = ruler.height() - 8
    drag(ruler, QPoint(300, scrub_y), QPoint(300, scrub_y + 60))
    assert view.px_per_beat > before
    QTest.mouseClick(ruler, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier,
                     QPoint(int(view.beat_to_x(3.0)), scrub_y))
    assert window.bridge.position == pytest.approx(view.snap_beat(3.0))


def test_mouse_trim_and_rubber_band(window, three_tracks):
    arrangement = window.arrangement
    lanes, view = arrangement.lanes, arrangement.view
    track = window.project.tracks[0]
    clip = track.clips[0]
    row = arrangement.layout_model.rows[0]
    y = row.top - view.scroll_y + 6  # trim handles are at the ends of the title bar
    body_y = row.top - view.scroll_y + row.height // 2
    right_edge = int(view.beat_to_x(clip.end_beat(window.project.tempo))) - 2
    assert lanes.hit_clip(QPointF(right_edge, body_y))[2] == "body"  # lower down: no trimming
    drag(lanes, QPoint(right_edge, y), QPoint(int(view.beat_to_x(2.0)), y))
    trimmed = window.project.track(track.id).clips[0]
    assert trimmed.duration_sec == pytest.approx(1.0)  # 2 beats at 120 BPM
    left_edge = int(view.beat_to_x(0.0)) + 2
    drag(lanes, QPoint(left_edge, y), QPoint(int(view.beat_to_x(1.0)), y))
    trimmed = window.project.track(track.id).clips[0]
    assert (trimmed.start_beat, trimmed.offset_sec) == (1.0, pytest.approx(0.5))

    # Trim handles are only just inside a clip's ends, shown with bracket cursors.
    start_x = int(view.beat_to_x(trimmed.start_beat))
    QTest.mouseMove(lanes, QPoint(start_x + 2, y))
    assert lanes.cursor().shape() == Qt.CursorShape.BitmapCursor
    assert lanes._hover_edge == (trimmed.id, "left")
    assert not lanes.grab().isNull()
    QTest.mouseMove(lanes, QPoint(start_x - 2, y))  # just outside: not a trim
    assert lanes.cursor().shape() == Qt.CursorShape.IBeamCursor and lanes._hover_edge is None
    drag(lanes, QPoint(start_x - 2, y), QPoint(int(view.beat_to_x(0.0)), y))
    assert window.project.track(track.id).clips[0].start_beat == 1.0  # a time selection, not a trim
    # Where two clips touch, each side of the boundary trims its own clip.
    window.editor.add_clips(track.id, 2.0, [(three_tracks[0], 1.0)])
    first, second = window.project.track(track.id).clips
    boundary = view.beat_to_x(2.0)
    assert lanes.hit_clip(QPointF(boundary - 2, y))[1:] == (first, "right")
    assert lanes.hit_clip(QPointF(boundary + 2, y))[1:] == (second, "left")

    empty_y = arrangement.layout_model.total_height - view.scroll_y + 40  # below the tracks
    drag(lanes, QPoint(int(view.beat_to_x(20.0)), empty_y), QPoint(int(view.beat_to_x(0.5)), 2))
    assert len(window.selection.clips) == 4  # including the clip added above


def test_clip_body_sets_insert_and_selects_time(window, three_tracks):
    arrangement = window.arrangement
    lanes, view, rows = arrangement.lanes, arrangement.view, arrangement.layout_model.rows
    tracks = window.project.tracks
    body_y = rows[0].top - view.scroll_y + rows[0].height // 2
    # A click in a clip body places the insert marker instead of selecting the clip.
    QTest.mouseClick(lanes, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier,
                     QPoint(int(view.beat_to_x(1.0)), body_y))
    assert window.selection.clips == set()
    assert window.selection.insert_beat == pytest.approx(1.0)
    assert window.selection.track_id == tracks[0].id
    # Dragging selects a time range across the tracks it covers; play starts at its start.
    end_y = rows[1].top - view.scroll_y + rows[1].height // 2
    drag(lanes, QPoint(int(view.beat_to_x(3.0)), body_y), QPoint(int(view.beat_to_x(1.0)), end_y))
    assert window.selection.time_range == (1.0, 3.0, (tracks[0].id, tracks[1].id))
    assert window.selection.insert_beat == pytest.approx(1.0)
    assert window.selection.clips == set()
    window.editor.delete_tracks([tracks[1].id])
    assert window.selection.time_range[2] == (tracks[0].id,)


def wheel(widget, pos: QPoint, notches: int, modifiers):
    from PySide6.QtCore import QPointF
    from PySide6.QtGui import QWheelEvent

    event = QWheelEvent(QPointF(pos), QPointF(widget.mapToGlobal(pos)), QPoint(), QPoint(0, 120 * notches),
                        Qt.MouseButton.NoButton, modifiers, Qt.ScrollPhase.NoScrollPhase, False)
    QApplication.sendEvent(widget, event)


def test_alt_wheel_resizes_tracks(window, three_tracks):
    arrangement = window.arrangement
    lanes, view, rows = arrangement.lanes, arrangement.view, arrangement.layout_model.rows
    track = window.project.tracks[1]
    before = track.height
    wheel(lanes, QPoint(200, rows[1].top - view.scroll_y + 10), 2, Qt.KeyboardModifier.AltModifier)
    assert window.project.track(track.id).height == before + 24
    assert arrangement.layout_model.rows[2].top == arrangement.layout_model.rows[1].top + before + 24
    header = arrangement.headers.headers[track.id]
    wheel(header.pan, QPoint(5, 5), -1, Qt.KeyboardModifier.AltModifier)  # over a control, too
    assert window.project.track(track.id).height == before + 12
    assert window.project.track(track.id).pan == 0.0
    wheel(header, QPoint(20, 5), -100, Qt.KeyboardModifier.AltModifier)
    assert window.project.track(track.id).height == 24  # MIN_TRACK_HEIGHT


def test_ctrl_alt_drag_scrolls_both_ways(window, three_tracks):
    for track in window.project.tracks:
        window.editor.set_track_height(track.id, 400)
    view = window.arrangement.view
    assert view.max_scroll_y > 100
    view.set_scroll_beats(8.0)
    mods = Qt.KeyboardModifier.ControlModifier | Qt.KeyboardModifier.AltModifier
    clips_before = [list(t.clips) for t in window.project.tracks]
    drag(window.arrangement.lanes, QPoint(400, 300), QPoint(300, 200), mods)
    assert view.scroll_y == 100
    assert view.scroll_beats == pytest.approx(8.0 + 100 / view.px_per_beat)
    assert [list(t.clips) for t in window.project.tracks] == clips_before  # nothing was copied or moved


def test_title_click_sets_playback_start(window, three_tracks):
    arrangement = window.arrangement
    lanes, view, rows = arrangement.lanes, arrangement.view, arrangement.layout_model.rows
    track = window.project.tracks[2]
    clip = track.clips[0]  # starts at 2 s = beat 4
    title = QPoint(int(view.beat_to_x(clip.start_beat) + 30), rows[2].top - view.scroll_y + 14)
    lanes.mouseMoveEvent(QMouseEvent(QMouseEvent.Type.MouseMove, QPointF(title), QPointF(lanes.mapToGlobal(title)),
                                     Qt.MouseButton.NoButton, Qt.MouseButton.NoButton,
                                     Qt.KeyboardModifier.NoModifier))
    assert lanes.cursor().shape() == Qt.CursorShape.PointingHandCursor
    QTest.mouseClick(lanes, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, title)
    assert window.selection.clips == {(track.id, clip.id)}
    assert window.selection.insert_beat == pytest.approx(4.0)
    window.toggle_play()
    assert window.bridge.position == pytest.approx(4.0, abs=0.1)
    window.toggle_play()


def test_ctrl_drag_copies_selected_clip(window, three_tracks):
    arrangement = window.arrangement
    lanes, view, row = arrangement.lanes, arrangement.view, arrangement.layout_model.rows[0]
    track = window.project.tracks[0]
    clip = track.clips[0]  # beats 0-4
    before = replace(clip)

    def title(c) -> QPoint:
        return QPoint(int(view.beat_to_x(c.start_beat) + 30), row.top - view.scroll_y + 6)

    QTest.mouseClick(lanes, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, title(clip))
    assert window.selection.clips == {(track.id, clip.id)}
    # Ctrl-dragging the selected clip copies it, leaving the original where it was.
    ctrl_drag(lanes, title(clip), title(clip) + QPoint(int(8 * view.px_per_beat), 0))
    original, copy = window.project.track(track.id).clips
    assert original == before
    assert copy.id != clip.id and copy.start_beat == pytest.approx(8.0)
    assert window.selection.clips == {(track.id, copy.id)}

    # Ctrl-clicking an unselected clip adds it; Ctrl-clicking a selected one deselects it.
    ctrl = Qt.KeyboardModifier.ControlModifier
    QTest.mouseClick(lanes, Qt.MouseButton.LeftButton, ctrl, title(original))
    assert window.selection.clips == {(track.id, original.id), (track.id, copy.id)}
    QTest.mouseClick(lanes, Qt.MouseButton.LeftButton, ctrl, title(copy))
    assert window.selection.clips == {(track.id, original.id)}
    assert len(window.project.track(track.id).clips) == 2  # clicks copy nothing


def test_double_click_clip_opens_clip_view(window, three_tracks):
    arrangement = window.arrangement
    view, row = arrangement.view, arrangement.layout_model.rows[0]
    track = window.project.tracks[0]
    clip = track.clips[0]
    clip_view = arrangement.clip_view
    assert not clip_view.isVisible()
    QTest.mouseDClick(arrangement.lanes, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier,
                      QPoint(int(view.beat_to_x(1.0)), row.top - view.scroll_y + row.height // 2))
    assert clip_view.isVisible()
    assert clip_view.geometry() == arrangement.rect()  # overlays the whole arrangement
    assert clip_view.clip_refs == [(track.id, clip.id)]
    assert clip_view.name.text() == clip.name
    assert clip_view.waveform.clips[0][0] == clip
    assert not clip_view.waveform.grab().isNull()
    assert window.selection.clips == {(track.id, clip.id)}
    QTest.keyClick(clip_view, Qt.Key.Key_Escape)
    assert not clip_view.isVisible()
    arrangement.toggle_clip_view()  # Shift+Tab reopens the selected clip
    assert clip_view.isVisible()
    window.editor.delete_clips([(track.id, clip.id)])  # deleting the clip closes the view
    assert not clip_view.isVisible()


def test_drag_ending_in_clip_band_selects_clip_range(window, three_tracks):
    arrangement = window.arrangement
    lanes, view, rows = arrangement.lanes, arrangement.view, arrangement.layout_model.rows
    selection, tracks = window.selection, window.project.tracks
    tempo = window.project.tempo
    body_y = rows[0].top - view.scroll_y + rows[0].height // 2
    lane_y = rows[1].top - view.scroll_y + rows[1].height - 8
    # Ending lower in the lane selects time (for automation later); Delete leaves clips alone.
    drag(lanes, QPoint(int(view.beat_to_x(1.0)), body_y), QPoint(int(view.beat_to_x(3.0)), lane_y))
    assert selection.time_range == (1.0, 3.0, (tracks[0].id, tracks[1].id))
    assert not selection.clip_range and selection.clips == set()
    window.delete_selection()
    assert [len(t.clips) for t in tracks] == [1, 1, 1]
    # Ending in the top (clip) band makes a clip range: it persists, and it knows
    # the clips it touches (for the clip view).
    band_y = rows[1].top - view.scroll_y + 5
    drag(lanes, QPoint(int(view.beat_to_x(1.0)), body_y), QPoint(int(view.beat_to_x(3.0)), band_y))
    assert selection.time_range == (1.0, 3.0, (tracks[0].id, tracks[1].id))
    assert selection.clip_range
    assert selection.clips == {(tracks[0].id, tracks[0].clips[0].id), (tracks[1].id, tracks[1].clips[0].id)}
    assert not lanes.grab().isNull()
    # Delete cuts out only the range: clip 0 (beats 0-4) keeps its start and end,
    # clip 1 (beats 2-8) loses its first beat, track 3 is outside the range.
    window.delete_selection()
    assert [(c.start_beat, c.end_beat(tempo)) for c in tracks[0].clips] == [(0.0, 1.0), (3.0, 4.0)]
    assert [(c.start_beat, c.end_beat(tempo)) for c in tracks[1].clips] == [(3.0, 8.0)]
    assert len(tracks[2].clips) == 1
    assert selection.time_range == (1.0, 3.0, (tracks[0].id, tracks[1].id))  # still selected
    assert selection.clips == set()
    window.undo_stack.undo()
    assert [len(t.clips) for t in tracks] == [1, 1, 1]
    # Ctrl+D copies just the selected area to right after it, then selects the copy.
    selection.set_time_range(1.0, 3.0, (tracks[0].id, tracks[1].id), clips=set())
    window.duplicate()

    def spans(track):
        return [(round(c.start_beat, 6), round(c.end_beat(tempo), 6)) for c in track.clips]

    assert spans(tracks[0]) == [(0.0, 3.0), (3.0, 5.0)]
    assert spans(tracks[1]) == [(2.0, 4.0), (4.0, 5.0), (5.0, 8.0)]
    assert tracks[1].clips[1].offset_sec == pytest.approx(0.0)  # the copy plays clip 1's first beat
    assert selection.time_range == (3.0, 5.0, (tracks[0].id, tracks[1].id)) and selection.clip_range
    assert selection.insert_beat == 3.0
    window.undo_stack.undo()
    assert [len(t.clips) for t in tracks] == [1, 1, 1]
    # Clicking a clip's title inside the range selects just that clip.
    QTest.mouseClick(lanes, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier,
                     QPoint(int(view.beat_to_x(5.0)), rows[1].top - view.scroll_y + 6))
    assert selection.time_range is None and not selection.clip_range
    assert selection.clips == {(tracks[1].id, tracks[1].clips[0].id)}
    # ...and draws it highlighted (this used to be hidden by a stale clip-range flag).
    body = QPoint(int(view.beat_to_x(5.0)), rows[1].top - view.scroll_y + rows[1].height // 2)
    highlighted = lanes.grab().toImage().pixelColor(body)
    selection.set_clips(set())
    assert lanes.grab().toImage().pixelColor(body) != highlighted


def test_clip_view_edits_several_clips_in_unison(window, three_tracks):
    arrangement = window.arrangement
    editor, tracks = window.editor, window.project.tracks
    refs = [(t.id, t.clips[0].id) for t in tracks[:2]]
    editor.update_clips(refs[1:], lambda c: replace(c, transpose=5, warp_mode="Smooth"), "setup")
    window.selection.set_clips(refs)
    arrangement.toggle_clip_view()
    clip_view = arrangement.clip_view
    assert clip_view.clip_refs == refs
    assert clip_view.name.text() == "2 Clips"
    assert len(clip_view.waveform.clips) == 2
    assert not clip_view.waveform.grab().isNull()
    assert clip_view.mode.currentIndex() == -1  # modes differ
    assert "…" in clip_view.transpose.readout.text()

    # A knob drag moves every clip by the same amount, as one undo step.
    depth = window.undo_stack.count()
    key = object()
    for value in (1.0, 2.2):
        clip_view.transpose.knob.valueChanged.emit(value, key)
    assert [t.clips[0].transpose for t in tracks[:2]] == [2, 7]
    assert window.undo_stack.count() == depth + 1
    # Choosing a warp mode sets it on all of them.
    clip_view.mode.activated.emit(WARP_MODES.index("Transients"))
    assert {t.clips[0].warp_mode for t in tracks[:2]} == {"Transients"}
    assert clip_view.mode.currentText() == "Transients"
    clip_view.warp.click()
    assert all(t.clips[0].warp for t in tracks[:2])
    window.undo_stack.undo()
    window.undo_stack.undo()
    window.undo_stack.undo()
    assert [t.clips[0].transpose for t in tracks[:2]] == [0, 5]
    assert tracks[2].clips[0].transpose == 0  # not selected, untouched
    # Deleting one clip keeps the view open on the other.
    editor.delete_clips(refs[:1])
    assert clip_view.clip_refs == refs[1:]
    assert clip_view.name.text() == tracks[1].clips[0].name


def test_clip_view_warping_reaches_the_audio(window, three_tracks):
    arrangement, project, editor = window.arrangement, window.project, window.editor
    track = project.tracks[0]  # tone0: 2 s at 220 Hz, beats 0..4
    for other in project.tracks[1:]:
        editor.set_track_param(other.id, "mute", True)
    window.selection.set_clips({(track.id, track.clips[0].id)})
    arrangement.toggle_clip_view()
    clip_view = arrangement.clip_view

    # Warp on: the segment BPM takes the project tempo, so nothing moves yet.
    clip_view.warp.click()
    clip = track.clips[0]
    assert (clip.warp, clip.segment_bpm) == (True, 120.0)
    assert clip.end_beat(project.tempo) == pytest.approx(4.0)
    # Doubling the tempo halves its duration and keeps it on the beat grid.
    editor.set_tempo(240.0)
    clip = track.clips[0]
    assert clip.end_beat(project.tempo) == pytest.approx(4.0)
    assert "1.00 s" in clip_view.info.text()
    out = window.engine.render_offline(0.0, 2 * SAMPLE_RATE)
    assert np.abs(out[SAMPLE_RATE // 2 : SAMPLE_RATE - 2000]).max() > 0.1
    assert np.abs(out[SAMPLE_RATE + 10 :]).max() == 0.0

    # Transpose is heard (an octave up) and the mode list offers every mode.
    clip_view.transpose.knob.valueChanged.emit(12.0, object())
    assert track.clips[0].transpose == 12
    out = window.engine.render_offline(0.0, SAMPLE_RATE)
    spectrum = np.abs(np.fft.rfft(out[SAMPLE_RATE // 4 : SAMPLE_RATE // 4 + 16384, 0] * np.hanning(16384)))
    assert np.argmax(spectrum) * SAMPLE_RATE / 16384 == pytest.approx(440.0, rel=0.01)

    # Re-Pitch ignores transposition, so its knobs are disabled.
    clip_view.mode.activated.emit(WARP_MODES.index("Re-Pitch"))
    assert not clip_view.transpose.isEnabled() and not clip_view.detune.isEnabled()
    desc = clip_desc(track.clips[0])
    assert (desc.warp, desc.segment_bpm, desc.warp_mode, desc.transpose) == (True, 120.0, ge.WarpMode.RE_PITCH, 12.0)
    clip_view.mode.activated.emit(WARP_MODES.index("Formants"))
    assert clip_view.transpose.isEnabled()

    # :2 halves the segment BPM: the clip plays twice as fast, half as long.
    before = track.clips[0].end_beat(project.tempo)
    buttons = [b for b in clip_view.findChildren(QPushButton) if b.text() == ":2"]
    buttons[0].click()
    assert track.clips[0].segment_bpm == 60.0
    assert track.clips[0].end_beat(project.tempo) == pytest.approx(before / 2)
    assert not arrangement.grab().isNull()  # warped waveforms draw at their own scale


def test_drop_files_from_browser(window, tmp_path):
    from PySide6.QtCore import QMimeData, QPointF, QUrl
    from PySide6.QtGui import QDragMoveEvent, QDropEvent

    path = write_wav(tmp_path / "dropped.wav", tone(1.0, 330.0))
    mime = QMimeData()
    mime.setUrls([QUrl.fromLocalFile(str(path))])
    lanes = window.arrangement.lanes
    pos = QPointF(window.arrangement.view.beat_to_x(4.0) + 1, 20)
    actions = Qt.DropAction.CopyAction
    buttons, mods = Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier
    lanes.dragMoveEvent(QDragMoveEvent(pos.toPoint(), actions, mime, buttons, mods))
    lanes.dropEvent(QDropEvent(pos, actions, mime, buttons, mods))
    [track] = window.project.tracks
    assert track.name == "dropped"
    assert track.clips[0].start_beat == pytest.approx(4.0)
    assert track.clips[0].duration_sec == pytest.approx(1.0)
    assert wait_until(lambda: window.bridge.source(str(path)) is not None)


def test_header_controls_and_dialogs(window, three_tracks):
    track = window.project.tracks[0]
    header = window.arrangement.headers.headers[track.id]
    drag(header.volume, QPoint(30, 10), QPoint(30, -30))  # drag up 40 px
    assert window.project.track(track.id).volume_db == pytest.approx(6.0)  # clamped at +6 dB
    window.undo_stack.undo()
    assert window.project.track(track.id).volume_db == 0.0
    QTest.mouseClick(header.solo, Qt.MouseButton.LeftButton)
    assert window.project.track(track.id).solo
    QTest.mouseClick(header.activator, Qt.MouseButton.LeftButton)
    assert window.project.track(track.id).mute
    header.start_rename()
    header._rename.setText("Drums")
    header._rename.editingFinished.emit()
    assert window.project.track(track.id).name == "Drums"

    from gilstudio.ui.dialogs import ExportDialog, PreferencesDialog

    prefs = PreferencesDialog(window.bridge, window)
    assert prefs.device.count() >= 1
    prefs.reject()
    export = ExportDialog(True, window)
    assert export.bit_depth.currentData() == 24
    export.reject()
