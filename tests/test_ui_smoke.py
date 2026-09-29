"""Builds the real main window offscreen and drives it like a user would."""

import os
import sys
import time
import traceback

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import numpy as np
import pytest
from PySide6.QtCore import QCoreApplication, QPoint, QSettings, Qt
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication

from gilstudio import _engine as ge
from gilstudio import theme

from .conftest import SAMPLE_RATE, write_wav


@pytest.fixture(scope="module")
def app():
    QCoreApplication.setOrganizationName("GIL Studio Tests")  # keep tests out of the user's settings
    QCoreApplication.setApplicationName("GIL Studio Tests")
    application = QApplication.instance() or QApplication([])
    theme.apply(application)
    yield application


@pytest.fixture
def window(app, tmp_path):
    settings = QSettings()
    settings.clear()
    settings.setValue("browser/places", [str(tmp_path)])
    from gilstudio.ui.main_window import MainWindow

    # Exceptions raised inside Qt slots are only printed; collect them instead.
    errors = []
    previous_hook = sys.excepthook
    sys.excepthook = lambda *exc_info: errors.append(exc_info)
    engine = ge.Engine()
    w = MainWindow(engine)
    w.resize(1400, 820)
    w.show()
    app.processEvents()
    yield w
    w.undo_stack.setClean()
    w.close()
    engine.close_device()
    w.deleteLater()
    app.processEvents()
    sys.excepthook = previous_hook
    assert not errors, "".join("".join(traceback.format_exception(*e)) for e in errors)


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
    start = QPoint(int(view.beat_to_x(clip.start_beat) + 30), row.top - view.scroll_y + row.height // 2)
    end = start + QPoint(int(4 * view.px_per_beat), 0)
    QTest.mousePress(lanes, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, start)
    QTest.mouseMove(lanes, start + QPoint(10, 0))
    QTest.mouseMove(lanes, end)
    QTest.mouseRelease(lanes, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, end)
    moved = window.project.track(track.id).clips[0]
    assert moved.id == clip.id
    assert moved.start_beat == pytest.approx(4.0)
    assert window.selection.clips == {(track.id, clip.id)}
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
    y = row.top - view.scroll_y + row.height // 2
    right_edge = int(view.beat_to_x(clip.end_beat(window.project.tempo))) - 2
    drag(lanes, QPoint(right_edge, y), QPoint(int(view.beat_to_x(2.0)), y))
    trimmed = window.project.track(track.id).clips[0]
    assert trimmed.duration_sec == pytest.approx(1.0)  # 2 beats at 120 BPM
    left_edge = int(view.beat_to_x(0.0)) + 2
    drag(lanes, QPoint(left_edge, y), QPoint(int(view.beat_to_x(1.0)), y))
    trimmed = window.project.track(track.id).clips[0]
    assert (trimmed.start_beat, trimmed.offset_sec) == (1.0, pytest.approx(0.5))

    empty_y = arrangement.layout_model.total_height - view.scroll_y + 40  # below the tracks
    drag(lanes, QPoint(int(view.beat_to_x(0.5)), 2), QPoint(int(view.beat_to_x(20.0)), empty_y))
    assert len(window.selection.clips) == 3


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
