"""Renders in the window: exporting and freezing in the background, their
progress in a dialog with Cancel (the window going on meanwhile), and a
project's plug-ins loading after it opens (the selected track's first;
renders wait for them)."""

import json
from pathlib import Path

import numpy as np
import pytest
from PySide6.QtCore import Qt, QTimer
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication, QDialog, QFileDialog

from substation.model.project import PLUGIN_KIND, Clip, MidiClip, Note
from substation.ui import dialogs as ui_dialogs
from substation.ui import main_window as ui_main_window
from substation.ui import rendering

from .conftest import SAMPLE_RATE, TEST_PLUGINS
from .test_ui_plugins import engine_id, installed

CTRL_SHIFT = Qt.KeyboardModifier.ControlModifier | Qt.KeyboardModifier.ShiftModifier

needs_plugins = pytest.mark.skipif(not TEST_PLUGINS.exists(), reason="test plug-ins not built")


@pytest.fixture(autouse=True)
def recordings(tmp_path, monkeypatch):
    monkeypatch.setenv("SUBSTATION_RECORDINGS", str(tmp_path / "Recordings"))
    return tmp_path / "Recordings"


@pytest.fixture
def export_to(monkeypatch, tmp_path):
    """Export Audio… answered: the whole arrangement at 24 bits, to this file."""
    target = tmp_path / "mix.wav"

    class Accept(ui_dialogs.ExportDialog):
        def exec(self):
            return QDialog.DialogCode.Accepted

    monkeypatch.setattr(ui_main_window, "ExportDialog", Accept)
    monkeypatch.setattr(QFileDialog, "getSaveFileName", lambda *_a, **_k: (str(target), ""))
    return target


def audio_track(window, make_wav, name="A", start_beat=0.0) -> str:
    wav = make_wav(np.full((SAMPLE_RATE, 2), 0.5))
    window.engine.load_source(wav)
    track = window.editor.add_audio_track(name=name)
    window.editor._commit("Add", {track.id: [Clip(id=name + "c", path=wav, name=name, start_beat=start_beat,
                                                  duration_sec=1.0, source_duration_sec=1.0)]})
    return track.id


def during_render(action) -> list:
    """Runs action(dialog) once a render's dialog shows (in its event loop); its results."""
    results = []

    def poll():
        dialog = rendering.active()
        if dialog is None:
            QTimer.singleShot(1, poll)
        else:
            results.append(action(dialog))

    QTimer.singleShot(0, poll)
    return results


def following(monkeypatch, before=None) -> list:
    """What each render's dialog follows: (its title, label, part); before(dialog, part) runs first."""
    seen = []
    follow = rendering.RenderProgress.follow

    def recording(dialog, job, label, part=(0, 1)):
        seen.append((dialog.windowTitle(), label, part))
        if before is not None:
            before(dialog, part)
        follow(dialog, job, label, part)

    monkeypatch.setattr(rendering.RenderProgress, "follow", recording)
    return seen


def test_exporting_shows_its_progress_and_writes_the_file(window, make_wav, export_to, monkeypatch):
    audio_track(window, make_wav)
    seen = following(monkeypatch)
    window.export_audio()
    assert seen == [("Export Audio", "Exporting mix.wav…", (0, 1))]
    assert export_to.exists() and window.statusBar().currentMessage() == "Exported mix.wav"
    assert rendering.active() is None and not window.engine.is_rendering
    source = window.engine.load_source(str(export_to))
    assert source.frames == SAMPLE_RATE and np.array(source.samples(0, 2000))[0, 1000] == pytest.approx(0.5, abs=1e-4)


def test_the_window_goes_on_while_it_renders_and_cancel_stops_it(window, make_wav, export_to):
    audio_track(window, make_wav, start_beat=10_000.0)  # over an hour to render

    def look(dialog):
        rendering_now = window.engine.is_rendering
        modal = QApplication.activeModalWidget() is dialog  # (the window takes no edits meanwhile)
        window.bridge._poll_meters()  # what the window polls doesn't wait for the render
        window.repaint()
        dialog.reject()  # Cancel
        return rendering_now, modal

    seen = during_render(look)
    window.export_audio()
    assert seen == [(True, True)]
    assert not export_to.exists()  # (what was written of it is gone)
    assert window.statusBar().currentMessage() == "Export cancelled"
    assert not window.engine.is_rendering and window.engine.render_offline(0.0, 100).shape == (100, 2)


def test_closing_the_window_while_it_renders_cancels_the_render(window, make_wav, export_to):
    audio_track(window, make_wav, start_beat=10_000.0)
    closed = during_render(lambda _dialog: window.close())
    window.export_audio()
    assert closed == [False] and window.isVisible()  # (the window stays: the render was cancelled)
    assert window.statusBar().currentMessage() == "Export cancelled" and not export_to.exists()


def test_freezing_shows_its_progress_and_cancel_freezes_nothing(window, make_wav, recordings, monkeypatch):
    a = audio_track(window, make_wav, "A")
    b = audio_track(window, make_wav, "B")
    window.selection.select_track(a, focus_track=True)
    window.selection.select_track(b, mode="toggle")
    cancelling = [True]

    def cancel(dialog, part):
        if part[0] == 1 and cancelling[0]:  # B's: cancelled (A's is done)
            dialog.reject()

    seen = following(monkeypatch, cancel)
    undo_text = window.undo_stack.undoText()
    window.activateWindow()
    QTest.keyClick(window.arrangement.lanes, Qt.Key.Key_F, CTRL_SHIFT)
    assert seen == [("Freeze Tracks", "Freezing A (1 of 2)…", (0, 2)),
                    ("Freeze Tracks", "Freezing B (2 of 2)…", (1, 2))]
    assert window.project.track(a).frozen is None and window.project.track(b).frozen is None
    assert window.undo_stack.undoText() == undo_text
    assert not list((recordings / "Freeze").glob("*.wav"))  # A's render too: no track plays it

    cancelling[0] = False
    window.activateWindow()
    QTest.qWait(10)
    QTest.keyClick(window.arrangement.lanes, Qt.Key.Key_F, CTRL_SHIFT)
    assert window.project.track(a).frozen is not None and window.project.track(b).frozen is not None
    assert window.undo_stack.undoText() == "Freeze Tracks"


def save_with_plugins(window, tmp_path, names, effects=False) -> Path:
    """A project with a track per name, each with SUB Test Synth playing a note
    (or an audio track with SUB Test Effect), saved; then a new project."""
    plugins = installed(window)
    for name in names:
        if effects:
            track = window.editor.add_audio_track(name=name)
            window.editor.add_device(track.id, PLUGIN_KIND, plugin=plugins["SUB Test Effect"])
            continue
        track = window.editor.add_midi_track(name=name, instrument=None, plugin=plugins["SUB Test Synth"])
        window.editor._commit("Add", {track.id: [MidiClip(id=name + "m", name=name, start_beat=0.0,
                                                          duration_beats=2.0, notes=(Note(60, 0.0, 1.0),))]})
    path = tmp_path / "plugins.gilproj"
    assert window._save_to(path)
    window.new_project()
    return path


@needs_plugins
def test_a_projects_plugins_load_after_it_opens_the_selected_tracks_first(window, tmp_path):
    path = save_with_plugins(window, tmp_path, ["A", "B", "C"])
    window.open_project(str(path))
    window.bridge._plugin_timer.stop()  # (one at a time, by hand here)
    a, b, c = window.project.tracks
    assert window.bridge.plugins_pending == 3 and window.plugins_label.text() == "Loading plug-ins: 0 of 3"
    window.selection.select_track(c.id)
    window.bridge._load_next_plugin()
    assert [engine_id(window, t, t.devices[0]) is not None for t in (a, b, c)] == [False, False, True]
    assert window.plugins_label.text() == "Loading plug-ins: 1 of 3"
    # Its editor asked for: that one now.
    window.bridge.request_plugin_editor(b.id, b.devices[0].id)
    assert engine_id(window, b, b.devices[0]) is not None and window.bridge.plugins_pending == 1
    window.bridge._load_next_plugin()
    assert engine_id(window, a, a.devices[0]) is not None and not window.bridge.plugins_pending
    assert window.plugins_label.isHidden()


@needs_plugins
def test_a_plugin_waiting_to_load_may_move_or_go(window, tmp_path):
    path = save_with_plugins(window, tmp_path, ["A", "B"], effects=True)
    window.open_project(str(path))
    window.bridge._plugin_timer.stop()
    a, b = (t.id for t in window.project.tracks)
    fx_a, fx_b = (window.project.track(t).devices[0].id for t in (a, b))
    window.editor.remove_device(b, fx_b)
    assert window.editor.move_devices_to_track(a, [fx_a], b)
    window.bridge.load_pending_plugins()  # A's loads where it is now; B's went
    assert window.bridge.engine_device_id(b, fx_a) is not None and not window.bridge.plugins_pending
    assert window.bridge.engine_device_id(b, fx_b) is None
    window.undo_stack.undo()  # (A's goes back, its processor along)
    assert window.bridge.engine_device_id(a, fx_a) is not None
    window.undo_stack.undo()  # B's is back: it loads now, as any device added
    assert window.bridge.engine_device_id(b, fx_b) is not None


@needs_plugins
def test_renders_wait_for_the_plugins_still_loading(window, tmp_path, export_to):
    path = save_with_plugins(window, tmp_path, ["A"])
    window.open_project(str(path))
    assert window.bridge.plugins_pending == 1
    labels = during_render(lambda dialog: dialog.label.text())
    window.export_audio()
    assert labels == ["Loading plug-ins (1 to go)…"]
    assert not window.bridge.plugins_pending and window.statusBar().currentMessage() == "Exported mix.wav"
    exported = np.array(window.engine.load_source(str(export_to)).samples(0, SAMPLE_RATE // 2))
    assert np.abs(exported).max() > 0.01  # the synth played


@needs_plugins
def test_saved_before_its_plugins_load_a_project_keeps_their_state(window, tmp_path):
    path = save_with_plugins(window, tmp_path, ["A"])
    saved = json.loads(path.read_text(encoding="utf-8"))["tracks"][0]["devices"][0]["state"]
    window.open_project(str(path))
    window.bridge._plugin_timer.stop()
    again = tmp_path / "again.gilproj"
    assert window._save_to(again)
    assert json.loads(again.read_text(encoding="utf-8"))["tracks"][0]["devices"][0]["state"] == saved
