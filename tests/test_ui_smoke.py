"""Builds the real main window offscreen and drives it like a user would."""

import sys
import time
from dataclasses import replace

import numpy as np
import pytest
from PySide6.QtCore import QPoint, QPointF, QSettings, Qt
from PySide6.QtGui import QMouseEvent
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication, QPushButton

from substation import _engine as ge
from substation.audio.engine_bridge import clip_desc
from substation.ui.clip_view import WARP_MODES

from .conftest import SAMPLE_RATE, write_wav


def wait_until(predicate, timeout=5.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        QTest.qWait(10)
    return False


def settle(browser) -> None:
    """Until the browser shows the results of its latest search (they come from another thread)."""
    assert wait_until(lambda: not browser.searching)


def indexed(browser, count: int) -> None:
    """Until the browser's index is done and has `count` files at least."""
    assert wait_until(lambda: not browser.index.indexing and browser.index.file_count >= count)


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
    window.selection.select_clips(window.editor, {(track.id, track.clips[0].id)}, track_id=track.id)
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


def test_cut_copy_paste_clips(window, three_tracks):
    project, selection, lanes = window.project, window.selection, window.arrangement.lanes
    first, second, third = project.tracks  # clips at beats 0-4, 2-8 and 4-12
    window.activateWindow()

    def press(key) -> None:
        QTest.keyClick(lanes, key, Qt.KeyboardModifier.ControlModifier)
        QTest.qWait(10)

    # Nothing copied yet: pasting says so and changes nothing.
    press(Qt.Key.Key_V)
    assert window.undo_stack.count() == 3

    selection.select_clips(window.editor, [(first.id, first.clips[0].id)])
    press(Qt.Key.Key_C)
    # Pasted at the insert marker on the selected track, and selected.
    selection.select_track(third.id)
    selection.set_insert(20.0)
    press(Qt.Key.Key_V)
    assert [(c.start_beat, c.end_beat(project.tempo)) for c in project.track(third.id).clips] == [(4, 12), (20, 24)]
    pasted = project.track(third.id).clips[1]
    assert pasted.path == first.clips[0].path and pasted.id != first.clips[0].id
    assert selection.time_range == (20.0, 24.0, (third.id,)) and selection.clips == {(third.id, pasted.id)}
    # The insert marker moves to its end, so pasting again appends.
    assert selection.insert_beat == 24.0
    press(Qt.Key.Key_V)
    assert [c.start_beat for c in project.track(third.id).clips] == [4, 20, 24]

    # Cut takes just the selected stretch out; the empty area stays selected.
    selection.set_time_range(5.0, 7.0, [second.id], clips=window.editor.clips_in_range(5.0, 7.0, [second.id]))
    press(Qt.Key.Key_X)
    assert [(c.start_beat, c.end_beat(project.tempo)) for c in project.track(second.id).clips] == [(2, 5), (7, 8)]
    assert selection.time_range == (5.0, 7.0, (second.id,)) and not selection.clips
    assert window.undo_stack.undoText() == "Cut"
    selection.set_insert(12.0)
    press(Qt.Key.Key_V)
    assert [c.start_beat for c in project.track(second.id).clips] == [2, 7, 12]
    window.undo_stack.undo()
    window.undo_stack.undo()
    assert [(c.start_beat, c.end_beat(project.tempo)) for c in project.track(second.id).clips] == [(2, 8)]


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


def test_switching_a_device_keeps_the_chain(window, three_tracks):
    """Switching a device on or off updates it in place: rebuilding the chain made it flash."""
    track = window.project.tracks[0]
    window.selection.select_track(track.id)
    a, b = window.editor.add_device(track.id, "utility"), window.editor.add_device(track.id, "ott")
    before = dict(window.devices.widgets)
    before[b.id].enabled.click()
    assert window.devices.widgets == before and not window.project.device(track.id, b.id).enabled
    window.undo_stack.undo()
    assert window.devices.widgets == before and before[b.id].enabled.isChecked()
    window.editor.move_device(track.id, b.id, 0)  # a real change still rebuilds it
    assert list(window.devices.widgets) == [b.id, a.id] and window.devices.widgets[a.id] is not before[a.id]


def test_builtin_devices_in_browser(window, three_tracks):
    from PySide6.QtCore import QPointF
    from PySide6.QtGui import QDropEvent

    from substation.ui.browser.browser_models import device_kinds

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
    settle(browser)
    rows = [browser.list_model.index(i) for i in range(browser.list_model.rowCount())]
    assert sorted(browser.list_model.item(i).name for i in rows) == ["Compressor", "Delay", "EQ", "Over The Top", "Sidechain",
                                                                                 "Utility"]
    [index] = [i for i in rows if browser.list_model.item(i).name == "Utility"]
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
    indexed(browser, 2)
    browser.search.setText("kick")
    browser._refresh()
    settle(browser)
    assert browser.list_model.rowCount() == 1
    item = browser.list_model.item(browser.list_model.index(0))
    assert item.name == "Kick Deep.wav"
    mime = browser.list_model.mimeData([browser.list_model.index(0)])
    assert mime.urls()[0].toLocalFile().endswith("Kick Deep.wav")
    browser.file_activated.emit(item.path)
    assert window.project.tracks[-1].clips[0].name == "Kick Deep"


def test_down_from_search_previews_first_result_until_clicking_elsewhere(window, tmp_path, monkeypatch):
    write_wav(tmp_path / "Kick Long.wav", tone(0.5, 60.0))
    browser = window.browser
    browser.index.rebuild([str(tmp_path)])
    indexed(browser, 1)
    previewed, stopped = [], []
    monkeypatch.setattr(window.bridge, "preview_file", previewed.append)
    monkeypatch.setattr(window.bridge, "stop_preview", lambda: stopped.append(True))
    browser.search.setText("kick long")
    QTest.keyClick(browser.search, Qt.Key.Key_Down)
    settle(browser)
    assert wait_until(lambda: browser.list_view.currentIndex().row() == 0)
    assert previewed and previewed[-1].endswith("Kick Long.wav")
    QTest.mouseClick(browser.list_view.viewport(), Qt.MouseButton.LeftButton, pos=QPoint(200, 200))
    assert not stopped  # clicks in the browser keep it playing
    QTest.mouseClick(window.arrangement.lanes, Qt.MouseButton.LeftButton, pos=QPoint(50, 10))
    assert stopped == [True]
    QTest.mouseClick(window.arrangement.lanes, Qt.MouseButton.LeftButton, pos=QPoint(60, 10))
    assert stopped == [True]  # nothing playing any more


def test_find_searches_all(window, tmp_path):
    write_wav(tmp_path / "Utility Hit.wav", tone(0.2, 60.0))
    browser = window.browser
    browser.index.rebuild([str(tmp_path)])
    indexed(browser, 1)
    [samples] = [browser.sidebar.topLevelItem(i) for i in range(browser.sidebar.topLevelItemCount())
                 if browser.sidebar.topLevelItem(i).text(0) == "Samples"]
    browser.sidebar.setCurrentItem(samples)
    window._focus_search()  # Ctrl+F
    assert browser._scope() == ("all",)
    browser.search.setText("utility")
    browser._refresh()
    settle(browser)
    kinds = {browser.list_model.item(browser.list_model.index(r)).kind
             for r in range(browser.list_model.rowCount())}
    assert kinds == {"device", "audio"}  # the built-in Utility and the sample


def select_scope(browser, scope: tuple) -> None:
    from substation.ui.browser.browser_panel import ROLE_SCOPE

    sidebar = browser.sidebar
    items = [sidebar.topLevelItem(i) for i in range(sidebar.topLevelItemCount())]
    items += [item.child(j) for item in list(items) for j in range(item.childCount())]
    [item] = [i for i in items if tuple(i.data(0, ROLE_SCOPE) or ()) == scope]
    sidebar.setCurrentItem(item)


def test_browser_keeps_its_place_when_files_change(window, tmp_path):
    folder = tmp_path / "Drums"
    folder.mkdir()
    for name in ("Kick 1.wav", "Kick 2.wav", "Snare.wav"):
        write_wav(folder / name, tone(0.1, 60.0))
    browser = window.browser
    browser.preview.setChecked(False)
    indexed(browser, 3)
    select_scope(browser, ("samples",))
    assert wait_until(lambda: browser.list_model.total == 3 and not browser.searching)  # the index's news came
    names = [browser.list_model.item(browser.list_model.index(r)).name for r in range(browser.list_model.rowCount())]
    assert names == ["Kick 1.wav", "Kick 2.wav", "Snare.wav"]
    browser.list_view.setCurrentIndex(browser.list_model.index(2))
    # A file appears above it (found by watching the place): the list shows it, and stays on Snare.
    write_wav(folder / "Big Kick.wav", tone(0.1, 60.0))
    assert wait_until(lambda: browser.list_model.total == 4 and not browser.searching)
    assert browser.list_model.item(browser.list_model.index(0)).name == "Big Kick.wav"
    assert browser.list_model.item(browser.list_view.currentIndex()).name == "Snare.wav"


def test_results_after_the_tree_was_shown_are_dropped(window, tmp_path):
    write_wav(tmp_path / "Kick.wav", tone(0.1, 60.0))
    browser = window.browser
    indexed(browser, 1)
    select_scope(browser, ("place", str(tmp_path)))
    settle(browser)
    assert browser.content.currentWidget() is browser.tree_view
    browser.search.setText("kick")
    browser._refresh()  # a search on its way...
    browser.search.setText("")
    browser._refresh()  # ...and the folder shown again before it came
    QTest.qWait(100)
    assert browser.content.currentWidget() is browser.tree_view and not browser.searching


def test_enter_in_search_selects_then_adds(window, three_tracks):
    browser, track = window.browser, window.project.tracks[0]
    window.selection.select_track(track.id)
    window._focus_search()
    browser.search.setText("utility")  # Enter before the results came: the first is selected when they do
    QTest.keyClick(browser.search, Qt.Key.Key_Return)
    assert wait_until(lambda: browser.list_model.item(browser.list_view.currentIndex()) is not None)
    current = browser.list_model.item(browser.list_view.currentIndex())
    assert current.kind == "device" and current.name == "Utility"
    assert not track.devices
    QTest.keyClick(browser.list_view, Qt.Key.Key_Return)
    assert [d.kind for d in track.devices] == ["utility"]


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


def test_mouse_trim_and_selecting_below_the_tracks(window, three_tracks):
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
    assert window.selection.insert_beat == 1.0  # playback starts from the trimmed clip
    window.selection.set_insert(6.0)
    QTest.mouseClick(lanes, Qt.MouseButton.LeftButton, pos=QPoint(int(view.beat_to_x(trimmed.end_beat(window.project.tempo))) - 2, y))
    assert window.selection.insert_beat == 1.0  # a click on its edge selects it, as on its body

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

    # Below the tracks is grid too: a drag from there selects time on it, from the last track up,
    # snapped like anywhere else (here ending in the first track's title band: the clips it touches).
    tracks = window.project.tracks
    empty_y = arrangement.layout_model.total_height - view.scroll_y + 40
    assert lanes.row_index_at(empty_y) is None
    QTest.mouseMove(lanes, QPoint(int(view.beat_to_x(3.0)), empty_y))
    assert lanes.cursor().shape() == Qt.CursorShape.IBeamCursor
    drag(lanes, QPoint(int(view.beat_to_x(20.3)), empty_y), QPoint(int(view.beat_to_x(0.6)), 2))
    snapped = (view.snap_beat(0.6), view.snap_beat(20.3))
    assert snapped == (1.0, 20.0)  # (a one-beat grid at this zoom)
    assert window.selection.time_range == (*snapped, tuple(t.id for t in tracks))
    assert window.selection.clip_range and len(window.selection.clips) == 4  # including the clip added above
    # A click there places the insert marker on the grid and deselects.
    QTest.mouseClick(lanes, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier,
                     QPoint(int(view.beat_to_x(6.1)), empty_y))
    assert window.selection.time_range is None and not window.selection.clips
    assert window.selection.insert_beat == 6.0


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
    alt = Qt.KeyboardModifier.AltModifier
    wheel(lanes, QPoint(200, rows[1].top - view.scroll_y + 10), 2, alt)
    assert window.project.track(track.id).height == before + 24
    assert arrangement.layout_model.rows[2].top == arrangement.layout_model.rows[1].top + before + 24
    header = arrangement.headers.headers[track.id]
    wheel(header.pan, QPoint(5, 5), -1, alt)  # over a control, too
    assert window.project.track(track.id).height == before + 12
    assert window.project.track(track.id).pan == 0.0
    wheel(header, QPoint(20, 5), -100, alt)
    assert window.project.track(track.id).height == 24  # MIN_TRACK_HEIGHT
    assert not window.project.track(track.id).folded  # (it got there in this turn)


def test_alt_wheel_folds_a_track_at_its_smallest_and_unfolds_it(window, three_tracks, monkeypatch):
    from substation.model.project import MIN_TRACK_HEIGHT
    from substation.ui.arrangement import lanes_canvas

    clock = [100.0]
    monkeypatch.setattr(lanes_canvas.time, "monotonic", lambda: clock[0])
    arrangement, project = window.arrangement, window.project
    lanes, view = arrangement.lanes, arrangement.view
    second, third = (t.id for t in project.tracks[1:])
    alt = Qt.KeyboardModifier.AltModifier

    def over_lane(track_id: str) -> QPoint:
        row = next(r for r in arrangement.layout_model.rows if r.track_id == track_id)
        return QPoint(200, row.top - view.scroll_y + 5)

    def turn(widget, pos, notches):
        clock[0] += 0.1  # one turn of the wheel, a notch at a time
        wheel(widget, pos, notches, alt)

    height = project.track(second).height
    for _ in range(-(-(height - MIN_TRACK_HEIGHT) // lanes_canvas.HEIGHT_STEP)):  # down: it shrinks...
        assert not project.track(second).folded
        turn(lanes, over_lane(second), -1)
    assert project.track(second).height == MIN_TRACK_HEIGHT and not project.track(second).folded
    turn(lanes, over_lane(second), -1)  # ...and at its smallest, folds
    assert project.track(second).folded
    # The same turn goes on with that track, though the next is under the mouse now.
    turn(lanes, over_lane(third), -1)
    assert project.track(second).folded and not project.track(third).folded
    assert project.track(third).height == height
    clock[0] += 1.0  # a turn of its own, over a control of its header
    turn(arrangement.headers.headers[second].pan, QPoint(5, 5), 1)  # up: unfolds it...
    assert not project.track(second).folded and project.track(second).height == MIN_TRACK_HEIGHT
    turn(arrangement.headers.headers[second], QPoint(20, 5), 1)  # ...then makes it taller
    assert project.track(second).height == MIN_TRACK_HEIGHT + lanes_canvas.HEIGHT_STEP


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
    # Selecting a clip selects the area it covers on the grid.
    assert window.selection.time_range == (0.0, 4.0, (track.id,)) and window.selection.clip_range
    assert window.selection.clips == {(track.id, clip.id)}
    # Ctrl-dragging the selected clip copies it, leaving the original where it was.
    ctrl_drag(lanes, title(clip), title(clip) + QPoint(int(8 * view.px_per_beat), 0))
    original, copy = window.project.track(track.id).clips
    assert original == before
    assert copy.id != clip.id and copy.start_beat == pytest.approx(8.0)
    assert window.selection.clips == {(track.id, copy.id)}
    assert window.selection.time_range == (8.0, 12.0, (track.id,))

    # A Ctrl-click (no drag) just selects the clip's area.
    QTest.mouseClick(lanes, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.ControlModifier, title(original))
    assert window.selection.time_range == (0.0, 4.0, (track.id,))
    assert len(window.project.track(track.id).clips) == 2  # clicks copy nothing


def test_shift_click_selects_the_clips_in_between(window, three_tracks):
    arrangement = window.arrangement
    lanes, view, rows = arrangement.lanes, arrangement.view, arrangement.layout_model.rows
    tracks = window.project.tracks  # clips at beats 0-4, 2-8 and 4-12
    window.editor.add_clips(tracks[1].id, 20.0, [(three_tracks[0], 2.0)])  # outside the span
    far = max(tracks[1].clips, key=lambda c: c.start_beat)

    def title(index, c) -> QPoint:
        return QPoint(int(view.beat_to_x(c.start_beat) + 10), rows[index].top - view.scroll_y + 6)

    shift = Qt.KeyboardModifier.ShiftModifier
    QTest.mouseClick(lanes, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, title(0, tracks[0].clips[0]))
    QTest.mouseClick(lanes, Qt.MouseButton.LeftButton, shift, title(2, tracks[2].clips[0]))
    # The area fully containing both clips, on the tracks from one to the other.
    assert window.selection.time_range == (0.0, 12.0, tuple(t.id for t in tracks))
    assert window.selection.clips == {(t.id, t.clips[0].id) for t in tracks if t.clips[0] is not far}
    assert (tracks[1].id, far.id) not in window.selection.clips
    # A plain click on one of the selected clips selects just it (a drag would move them all).
    QTest.mouseClick(lanes, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, title(1, tracks[1].clips[0]))
    assert window.selection.clips == {(tracks[1].id, tracks[1].clips[0].id)}
    QTest.mouseClick(lanes, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, title(0, tracks[0].clips[0]))
    QTest.mouseClick(lanes, Qt.MouseButton.LeftButton, shift, title(2, tracks[2].clips[0]))
    # Shift-clicking again extends from the same anchor rather than adding to the selection.
    QTest.mouseClick(lanes, Qt.MouseButton.LeftButton, shift, title(0, tracks[0].clips[0]))
    assert window.selection.clips == {(tracks[0].id, tracks[0].clips[0].id)}


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
    # Going up into the lanes above selects the clips, wherever in the lane it ends.
    drag(lanes, QPoint(int(view.beat_to_x(1.0)), lane_y), QPoint(int(view.beat_to_x(3.0)), body_y))
    assert selection.time_range == (1.0, 3.0, (tracks[0].id, tracks[1].id)) and selection.clip_range
    selection.clear()
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
    # Clicking a clip's title inside the range selects just that clip's area.
    QTest.mouseClick(lanes, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier,
                     QPoint(int(view.beat_to_x(5.0)), rows[1].top - view.scroll_y + 6))
    assert selection.time_range == (2.0, 8.0, (tracks[1].id,)) and selection.clip_range
    assert selection.clips == {(tracks[1].id, tracks[1].clips[0].id)}
    # ...and draws it highlighted.
    body = QPoint(int(view.beat_to_x(5.0)), rows[1].top - view.scroll_y + rows[1].height // 2)
    highlighted = lanes.grab().toImage().pixelColor(body)
    selection.clear()
    assert lanes.grab().toImage().pixelColor(body) != highlighted


def test_dragging_a_clip_range_moves_it(window, three_tracks):
    arrangement = window.arrangement
    lanes, view, rows = arrangement.lanes, arrangement.view, arrangement.layout_model.rows
    selection, tracks, tempo = window.selection, window.project.tracks, window.project.tempo

    def band(row, beat):
        return QPoint(int(view.beat_to_x(beat)), rows[row].top - view.scroll_y + 5)

    def spans(track):
        return [(round(c.start_beat, 6), round(c.end_beat(tempo), 6)) for c in track.clips]

    def select_range():
        body = QPoint(int(view.beat_to_x(1.0)), rows[0].top - view.scroll_y + rows[0].height // 2)
        drag(lanes, body, band(1, 3.0))
        assert selection.time_range == (1.0, 3.0, (tracks[0].id, tracks[1].id)) and selection.clip_range

    # Clips: track 0 beats 0-4, track 1 beats 2-8, track 2 beats 4-12.
    select_range()
    depth = window.undo_stack.count()
    drag(lanes, band(0, 2.0), band(0, 6.0))  # grabbing any selected clip moves the whole range
    assert spans(tracks[0]) == [(0.0, 1.0), (3.0, 4.0), (5.0, 7.0)]
    assert spans(tracks[1]) == [(3.0, 6.0), (6.0, 7.0), (7.0, 8.0)]  # the moved beat replaces what it lands on
    assert selection.time_range == (5.0, 7.0, (tracks[0].id, tracks[1].id)) and selection.clip_range
    assert window.undo_stack.count() == depth + 1
    window.undo_stack.undo()
    assert [len(t.clips) for t in tracks] == [1, 1, 1]

    # Down a track, and Ctrl copies instead.
    select_range()
    ctrl_drag(lanes, band(0, 2.0), band(1, 2.0))
    assert spans(tracks[0]) == [(0.0, 4.0)]
    assert spans(tracks[1]) == [(1.0, 3.0), (3.0, 8.0)]
    assert spans(tracks[2]) == [(2.0, 3.0), (4.0, 12.0)]
    assert selection.time_range == (1.0, 3.0, (tracks[1].id, tracks[2].id))
    window.undo_stack.undo()

    # A click inside the range, without dragging, still selects just that clip's area.
    select_range()
    QTest.mouseClick(lanes, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, band(1, 2.5))
    assert selection.time_range == (2.0, 8.0, (tracks[1].id,))
    assert selection.clips == {(tracks[1].id, tracks[1].clips[0].id)}
    assert lanes._gesture is None

    # Dragging a selected clip moves the very clip (same id), unchanged.
    clip = tracks[1].clips[0]
    drag(lanes, band(1, 2.5), band(1, 4.5))
    [moved] = tracks[1].clips
    assert moved.id == clip.id and moved.start_beat == 4.0 and moved.duration_sec == clip.duration_sec
    assert selection.time_range == (4.0, 10.0, (tracks[1].id,))


def test_clip_view_edits_several_clips_in_unison(window, three_tracks):
    arrangement = window.arrangement
    editor, tracks = window.editor, window.project.tracks
    refs = [(t.id, t.clips[0].id) for t in tracks[:2]]
    editor.update_clips(refs[1:], lambda c: replace(c, transpose=5, warp_mode="Smooth"), "setup")
    window.selection.select_clips(window.editor, refs)
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
    window.selection.select_clips(window.editor, {(track.id, track.clips[0].id)})
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



def test_dropped_loop_is_set_up_in_the_same_undo_step(window, tmp_path):
    """Warping and transposing a dropped loop isn't an edit of its own: one undo removes the sample."""
    from PySide6.QtCore import QMimeData, QPointF, QUrl
    from PySide6.QtGui import QDragMoveEvent, QDropEvent

    from substation.model.keys import Key

    window.editor.set_key(Key(0))  # C major
    path = write_wav(tmp_path / "Bass_Loop_100_D.wav", tone(1.0, 330.0))
    mime = QMimeData()
    mime.setUrls([QUrl.fromLocalFile(str(path))])
    lanes = window.arrangement.lanes
    pos = QPointF(window.arrangement.view.beat_to_x(0.0) + 1, 20)
    actions = Qt.DropAction.CopyAction
    buttons, mods = Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier
    steps = window.undo_stack.index()
    lanes.dragMoveEvent(QDragMoveEvent(pos.toPoint(), actions, mime, buttons, mods))
    lanes.dropEvent(QDropEvent(pos, actions, mime, buttons, mods))
    window.arrangement.toggle_clip_view()  # showing it changes nothing
    QApplication.processEvents()
    [clip] = window.project.tracks[0].clips
    assert clip.is_warped and clip.segment_bpm == 100.0 and clip.transpose == -2
    assert window.undo_stack.index() == steps + 1
    window.undo_stack.undo()
    assert window.project.tracks == [] and window.project.key == Key(0)

def test_header_controls_and_dialogs(window, three_tracks):
    track = window.project.tracks[0]
    header = window.arrangement.headers.headers[track.id]
    drag(header.volume, QPoint(30, 10), QPoint(30, -110))  # drag up 120 px
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

    from substation.ui.dialogs import ExportDialog, PreferencesDialog

    prefs = PreferencesDialog(window.bridge, window)
    assert prefs.device.count() >= 1
    prefs.reject()
    export = ExportDialog(True, window)
    assert export.bit_depth.currentData() == 24
    export.reject()


def test_audio_threads_preference(window):
    """Preferences > Audio Threads applies at once and is remembered (the default unless chosen)."""
    from substation import _engine as ge
    from substation.audio.settings import audio_threads
    from substation.ui.dialogs import PreferencesDialog

    engine, default = window.engine, ge.Engine.default_audio_threads()
    assert audio_threads() == 0 and engine.audio_threads == default
    prefs = PreferencesDialog(window.bridge, window)
    assert prefs.threads.currentData() == default
    assert prefs.threads.itemText(0) == "1 (off)" and "(default)" in prefs.threads.currentText()
    chosen = 1 if default != 1 else 2
    prefs.threads.setCurrentIndex(prefs.threads.findData(chosen))
    assert engine.audio_threads == chosen and audio_threads() == chosen
    prefs.reject()

    engine.audio_threads = 3
    window.bridge.apply_audio_threads()  # (on start-up)
    assert engine.audio_threads == chosen
    prefs = PreferencesDialog(window.bridge, window)
    assert prefs.threads.currentData() == chosen
    prefs.threads.setCurrentIndex(prefs.threads.findData(default))
    assert engine.audio_threads == default and audio_threads() == 0  # back to the default: not pinned
    prefs.reject()


@pytest.mark.skipif(sys.platform != "win32", reason="plug-in editors are Win32 windows")
def test_shortcuts_from_plugin_editor(window, monkeypatch):
    import ctypes
    from ctypes import wintypes

    from substation.ui import plugin_keys

    shortcuts = window._plugin_shortcuts
    ctrl = Qt.KeyboardModifier.ControlModifier
    monkeypatch.setattr(plugin_keys, "is_plugin_editor", lambda hwnd: hwnd == 1234)
    monkeypatch.setattr(plugin_keys, "pressed_modifiers", lambda: ctrl)

    def key_down(hwnd: int, vk: int) -> bool:
        msg = wintypes.MSG(hWnd=hwnd, message=plugin_keys.WM_KEYDOWN, wParam=vk)
        return shortcuts.nativeEventFilter(b"windows_generic_MSG", ctypes.addressof(msg))[0]

    window.browser.sidebar.setCurrentItem(window.browser.sidebar.topLevelItem(2))  # Samples
    assert key_down(1234, ord("F"))  # Ctrl+F in a plug-in's editor searches the browser
    assert window.browser._scope() == ("all",)
    assert not key_down(1234, ord("C"))  # the plug-in keeps its copy / paste / undo...
    assert not key_down(1234, ord("K"))  # ...and keys that are no shortcut
    assert not key_down(999, ord("F"))  # other windows' keys are Qt's
    # Space and S without Ctrl/Alt: the window's (the DAW comes first); other keys: the plug-in's.
    monkeypatch.setattr(plugin_keys, "pressed_modifiers", lambda: Qt.KeyboardModifier.NoModifier)
    played = []
    monkeypatch.setattr(window.bridge, "play", lambda: played.append("play"))
    assert key_down(1234, 0x20) and played == ["play"]
    msg = wintypes.MSG(hWnd=1234, message=plugin_keys.WM_KEYDOWN, wParam=0x20, lParam=plugin_keys.REPEAT_BIT)
    assert shortcuts.nativeEventFilter(b"windows_generic_MSG", ctypes.addressof(msg))[0]
    assert played == ["play"]  # held down: once
    window.insert_track()
    track = window.project.tracks[-1]
    window.selection.select_track(track.id)
    assert key_down(1234, ord("S")) and window.project.track(track.id).solo
    assert not key_down(1234, ord("D")) and not key_down(1234, 0x2E)  # Delete
    monkeypatch.setattr(plugin_keys, "is_text_field", lambda hwnd: True)  # typing into an Edit control
    assert not key_down(1234, 0x20) and played == ["play"]
    monkeypatch.setattr(plugin_keys, "is_text_field", lambda hwnd: False)
    window.computer_keyboard.set_enabled(True)  # S plays a note, as in the window
    assert not key_down(1234, ord("S")) and key_down(1234, 0x20)
    window.computer_keyboard.set_enabled(False)


def test_used_items_rank_first(window, three_tracks):
    browser, track = window.browser, window.project.tracks[0]
    window.selection.select_track(track.id)
    window._focus_search()

    def names() -> list[str]:
        browser.search.setText("e")
        browser._refresh()
        settle(browser)
        return [browser.list_model.item(browser.list_model.index(r)).name
                for r in range(browser.list_model.rowCount())]

    before = names()
    last = before[-1]
    browser.list_model.ensure_rows(len(before))
    browser._activate_list(browser.list_model.index(len(before) - 1))  # a double-click
    assert browser.library.uses(browser.list_model.item(browser.list_model.index(len(before) - 1)).key) == 1
    assert names()[0] == last  # used, so first
    browser.sort.setCurrentIndex(browser.sort.findData("name"))
    assert names() == sorted(before, key=str.casefold)
    assert QSettings().value("browser/sort") == "name"


def test_open_recent(window, tmp_path, monkeypatch):
    from PySide6.QtWidgets import QMessageBox

    first, second = tmp_path / "first.gilproj", tmp_path / "second & more.gilproj"
    assert window._save_to(first)
    assert window._save_to(second)
    window._save_to(first)  # back to the top, not listed twice
    assert window.recent_projects() == [str(first.resolve()), str(second.resolve())]

    window._fill_recent_menu()
    labels = [a.text() for a in window.recent_menu.actions() if not a.isSeparator()]
    assert labels == ["&1  first.gilproj", "&2  second && more.gilproj", "&Clear List"]

    window.recent_menu.actions()[1].trigger()
    assert window.project.path.resolve() == second.resolve()
    assert window.recent_projects()[0] == str(second.resolve())

    second.unlink()  # a missing file is dropped from the list
    monkeypatch.setattr(QMessageBox, "warning", lambda *args: None)
    window._open_recent(str(second.resolve()))
    assert window.recent_projects() == [str(first.resolve())]

    window.recent_menu.actions()[-1].trigger()  # Clear List
    window._fill_recent_menu()
    assert [a.isEnabled() for a in window.recent_menu.actions()] == [False]
