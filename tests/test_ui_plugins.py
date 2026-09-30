"""VST3 plug-ins in the application: the browser, the device view, editors,
undo, and projects. Drives the real main window offscreen with the test
plug-ins (see tests/vst3_plugins); plug-in editors are real Win32 windows."""

import base64
import ctypes
import json
from ctypes import wintypes

import pytest
from PySide6.QtCore import QEvent, QObject, QPoint, QPointF, Qt
from PySide6.QtGui import QDropEvent
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication, QFileDialog, QWidget

from gilstudio.model.project import PLUGIN_KIND, PluginRef
from gilstudio.ui.browser.browser_models import plugin_refs
from gilstudio.ui.browser.file_index import plugin_ref
from gilstudio.ui.device_panel import PluginDeviceWidget

from .conftest import TEST_PLUGINS
from .test_ui_smoke import drag, wait_until
from .test_vst3_engine import EDIT_GAIN, FX_GAIN, GAIN, WAVE, editor_window, user32

pytestmark = pytest.mark.skipif(not TEST_PLUGINS.exists(), reason="test plug-ins not built")



def drop(pos: QPointF, mime) -> QDropEvent:
    return QDropEvent(pos, Qt.DropAction.CopyAction, mime, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier)


def installed(window) -> dict[str, PluginRef]:
    index = window.browser.plugin_index
    assert wait_until(lambda: not index.scanning and index.plugins)
    return {p.name: plugin_ref(p) for p in index.plugins}


def synth_track(window):
    track = window.editor.add_midi_track(instrument=None, plugin=installed(window)["GIL Test Synth"])
    window.selection.select_track(track.id)
    [device] = track.devices
    return track, device


def engine_id(window, track, device):
    return window.bridge.engine_device_id(track.id, device.id)


def poll(window):
    """What the UI's timer does: the engine's housekeeping and plug-in reports."""
    window.bridge.poll_plugins()


def test_plugins_in_the_browser(window):
    refs = installed(window)
    browser = window.browser
    plugins = browser._plugins_entry
    assert [plugins.child(i).text(0) for i in range(plugins.childCount())] == ["Instruments", "Audio Effects"]
    browser.sidebar.setCurrentItem(plugins.child(1))
    names = [browser.list_model.item(browser.list_model.index(i)).name for i in range(browser.list_model.rowCount())]
    assert names == ["GIL Test Effect", "GIL Test Mono"]
    assert browser.status.text() == "2 plug-ins"
    browser.sidebar.setCurrentItem(plugins.child(0))
    index = browser.list_model.index(0)
    item = browser.list_model.item(index)
    assert item.name == "GIL Test Synth" and item.detail == "GIL Studio" and "Instrument" in item.tooltip
    assert plugin_refs(browser.list_model.mimeData([index])) == [refs["GIL Test Synth"]]

    # Double-clicking an instrument with no MIDI track selected makes one for it.
    browser._activate_list(index)
    [track] = window.project.tracks
    [device] = track.devices
    assert track.is_midi and device.kind == PLUGIN_KIND and device.plugin == refs["GIL Test Synth"]
    widget = window.devices.widgets[device.id]
    assert isinstance(widget, PluginDeviceWidget)
    # The generic editor offers what the user can change (not the read-only
    # parameters), a page at a time.
    assert list(widget.knobs) == [GAIN, 6, 7, 8, 9, 10, 11] and list(widget.choices) == [WAVE]
    assert widget.choices[WAVE].currentText() == "Sine" and widget.pages == 2
    assert widget.page_label.text() == "1/2" and not widget.previous.isEnabled()
    widget.next.click()
    assert list(widget.knobs) == [12, 13, 14, 15] and widget.page_label.text() == "2/2"
    window.editor.add_device(track.id, "utility")  # the device view is rebuilt...
    assert list(window.devices.widgets[device.id].knobs) == [12, 13, 14, 15]  # ...on the same page


class _WindowsShown(QObject):
    """Every widget shown as a window of its own, however briefly."""

    def __init__(self):
        super().__init__()
        self.shown = []

    def eventFilter(self, watched, event):
        if event.type() == QEvent.Type.Show and isinstance(watched, QWidget) and watched.isWindow():
            self.shown.append(type(watched).__name__)
        return False


def test_showing_a_plugin_opens_no_stray_windows(window):
    # The synth has two pages of parameters, so its device shows page buttons.
    refs = installed(window)
    spy = _WindowsShown()
    QApplication.instance().installEventFilter(spy)
    try:
        track = window.editor.add_midi_track(instrument=None, plugin=refs["GIL Test Synth"])
        window.selection.select_track(track.id)
    finally:
        QApplication.instance().removeEventFilter(spy)
    assert window.devices.widgets[track.devices[0].id].pages == 2
    assert spy.shown == []
    QTest.qWait(1)  # it has no editor to show, and that's no news to report
    assert "no editor" not in window.statusBar().currentMessage()


def test_knobs_edit_plugins_undoably(window):
    track, device = synth_track(window)
    pid = engine_id(window, track, device)
    widget = window.devices.widgets[device.id]
    knob, readout = widget.knobs[GAIN]
    centre = QPoint(knob.width() // 2, knob.height() // 2)
    drag(knob, centre, centre + QPoint(0, 75))  # half the range down
    assert window.engine.processor_param(pid, GAIN) == pytest.approx(0.5, abs=0.02)
    assert readout.text().startswith("0.5") and window.undo_stack.count() == 2  # the track, one knob drag
    widget.choices[WAVE].activated.emit(0)
    assert window.engine.processor_param(pid, WAVE) == 0.0
    window.undo_stack.undo()
    window.undo_stack.undo()
    assert window.engine.processor_param(pid, GAIN) == 1.0 and window.engine.processor_param(pid, WAVE) == 1.0
    assert widget.choices[WAVE].currentText() == "Sine" and readout.text().startswith("1.0")
    window.undo_stack.redo()
    assert window.engine.processor_param(pid, GAIN) == pytest.approx(0.5, abs=0.02)


def test_edits_in_the_plugin_editor_are_undoable(window):
    track = window.editor.add_audio_track(name="Drums")
    window.selection.select_track(track.id)
    device = window.editor.add_device(track.id, PLUGIN_KIND, plugin=installed(window)["GIL Test Effect"])
    pid = engine_id(window, track, device)
    widget = window.devices.widgets[device.id]
    widget.edit.click()  # shows the editor
    assert widget.edit.isChecked() and window.engine.is_editor_open(pid)
    frame = editor_window("GIL Test Effect - Drums")
    view = user32.GetWindow(frame, 5)
    commands = window.undo_stack.count()

    user32.SendMessageW(view, EDIT_GAIN, 0, 0)  # a knob drag in the plug-in's editor
    poll(window)
    assert window.undo_stack.count() == commands + 1  # one gesture, one undo step
    assert window.project.device(track.id, device.id).params["0"] == pytest.approx(0.3)
    assert widget.knobs[FX_GAIN][0].value() == pytest.approx(0.3)
    window.undo_stack.undo()
    assert window.engine.processor_param(pid, FX_GAIN) == 0.5  # the plug-in shows the old value again
    window.undo_stack.redo()
    assert window.engine.processor_param(pid, FX_GAIN) == pytest.approx(0.3)

    # Renaming the track renames the editor window.
    window.editor.rename_track(track.id, "Loops")
    assert editor_window("GIL Test Effect - Loops") == frame
    # Closing the window unchecks the button.
    user32.SendMessageW(frame, 0x0010, 0, 0)  # WM_CLOSE
    poll(window)
    assert not widget.edit.isChecked() and not window.engine.is_editor_open(pid)


def test_plugin_changes_mark_the_project_changed(window, tmp_path):
    track = window.editor.add_audio_track()
    device = window.editor.add_device(track.id, PLUGIN_KIND, plugin=installed(window)["GIL Test Effect"])
    assert window._save_to(tmp_path / "song.gilproj") and window.undo_stack.isClean()
    window.bridge.open_plugin_editor(track.id, device.id)
    view = user32.GetWindow(editor_window(f"GIL Test Effect - {track.name}"), 5)
    user32.SendMessageW(view, 0x403, 0, 0)  # the plug-in says its state changed
    poll(window)
    assert not window.undo_stack.isClean() and window.windowTitle().startswith("song*")


def test_devices_keep_their_plugin_state(window):
    track, synth = synth_track(window)
    pid = engine_id(window, track, synth)
    window.engine.set_processor_param(pid, GAIN, 0.3)  # as if changed in its editor, without an undo step
    utility = window.editor.add_device(track.id, "utility")
    window.editor.move_device(track.id, utility.id, 0)  # an instrument stays first...
    assert [d.id for d in track.devices] == [synth.id, utility.id]
    assert engine_id(window, track, synth) == pid  # ...and nothing is reloaded

    window.editor.remove_device(track.id, synth.id)
    assert engine_id(window, track, utility) is not None
    window.undo_stack.undo()  # the plug-in comes back as it was
    restored = engine_id(window, track, window.project.track(track.id).devices[0])
    assert restored != pid and window.engine.processor_param(restored, GAIN) == pytest.approx(0.3)

    window.editor.delete_tracks([track.id])
    window.undo_stack.undo()
    track = window.project.tracks[0]
    assert window.engine.processor_param(engine_id(window, track, track.devices[0]), GAIN) == pytest.approx(0.3)


def test_projects_keep_plugins_and_their_state(window, tmp_path):
    track, device = synth_track(window)
    window.engine.set_processor_param(engine_id(window, track, device), GAIN, 0.3)
    path = tmp_path / "song.gilproj"
    assert window._save_to(path)
    [saved] = json.loads(path.read_text(encoding="utf-8"))["tracks"][0]["devices"]
    assert saved["kind"] == "plugin" and saved["plugin"]["name"] == "GIL Test Synth"
    assert saved["plugin"]["path"] == str(TEST_PLUGINS) and saved["plugin"]["instrument"] is True
    assert base64.b64decode(saved["state"])[:4] == b"VST3"

    window.new_project()
    window.open_project(str(path))
    track = window.project.tracks[0]
    assert window.engine.processor_param(engine_id(window, track, track.devices[0]), GAIN) == pytest.approx(0.3)


def test_missing_and_moved_plugins(window, tmp_path):
    refs = installed(window)
    synth_track(window)
    path = tmp_path / "song.gilproj"
    window._save_to(path)
    data = json.loads(path.read_text(encoding="utf-8"))
    devices = data["tracks"][0]["devices"]
    devices[0]["plugin"]["path"] = str(tmp_path / "moved away" / "GILTestPlugins.vst3")  # found by its id
    devices.append(json.loads(json.dumps(devices[0])) | {"id": "gone"})
    devices[1]["plugin"] |= {"uid": "0" * 32, "name": "Gone Synth", "instrument": False}
    path.write_text(json.dumps(data), encoding="utf-8")

    window.open_project(str(path))
    track = window.project.tracks[0]
    moved, gone = track.devices
    assert engine_id(window, track, moved) is not None and engine_id(window, track, gone) is None
    assert window.bridge.plugin_errors["gone"] == "Gone Synth is not installed."
    window.selection.select_track(track.id)
    widget = window.devices.widgets["gone"]
    assert not widget.edit.isEnabled()
    # Saving keeps the missing device (and its settings) for when it's back.
    window._save_to(path)
    kept = json.loads(path.read_text(encoding="utf-8"))["tracks"][0]["devices"]
    assert kept[1]["plugin"]["name"] == "Gone Synth" and kept[1]["state"] == devices[1]["state"]
    assert kept[0]["plugin"]["path"] == refs["GIL Test Synth"].path  # remembers where it was found


def test_dropping_plugins(window):
    refs = installed(window)
    window.insert_track()
    audio = window.project.tracks[0]
    lanes, arrangement = window.arrangement.lanes, window.arrangement
    browser = window.browser
    browser.sidebar.setCurrentItem(browser._plugins_entry)  # all plug-ins, by name
    model = browser.list_model
    mime = {model.item(model.index(i)).name: model.mimeData([model.index(i)]) for i in range(model.rowCount())}
    row = arrangement.layout_model.rows[0]
    on_track = QPointF(40, row.top - arrangement.view.scroll_y + 10)

    lanes.dropEvent(drop(on_track, mime["GIL Test Effect"]))
    assert [d.plugin for d in audio.devices] == [refs["GIL Test Effect"]]
    lanes.dropEvent(drop(on_track, mime["GIL Test Synth"]))  # not on an audio track
    assert len(audio.devices) == 1 and "MIDI" in window.statusBar().currentMessage()
    below = QPointF(40, arrangement.layout_model.total_height + 20)
    lanes.dropEvent(drop(below, mime["GIL Test Synth"]))  # below the tracks: a new MIDI track
    midi = window.project.tracks[1]
    assert midi.is_midi and [d.plugin.name for d in midi.devices] == ["GIL Test Synth"]
    window.devices.dropEvent(drop(QPointF(20, 20), mime["GIL Test Mono"]))  # the track shown
    assert [d.plugin.name for d in midi.devices] == ["GIL Test Synth", "GIL Test Mono"]


def test_presets(window, tmp_path, monkeypatch):
    track, device = synth_track(window)
    pid = engine_id(window, track, device)
    widget = window.devices.widgets[device.id]
    window.engine.set_processor_param(pid, GAIN, 0.25)
    preset = tmp_path / "Quiet.vstpreset"
    monkeypatch.setattr(QFileDialog, "getSaveFileName", lambda *args: (str(preset), ""))
    widget.save_preset()
    assert preset.read_bytes()[:4] == b"VST3"

    window.engine.set_processor_param(pid, GAIN, 0.8)
    monkeypatch.setattr(QFileDialog, "getOpenFileName", lambda *args: (str(preset), ""))
    widget.load_preset()
    assert window.engine.processor_param(pid, GAIN) == pytest.approx(0.25)
    assert window.undo_stack.undoText() == "Load Preset Quiet"
    assert window.devices.widgets[device.id].knobs[GAIN][0].value() == pytest.approx(0.25)
    window.undo_stack.undo()
    assert window.engine.processor_param(pid, GAIN) == pytest.approx(0.8)

    # Another plug-in's preset is refused, and changes nothing.
    other = window.editor.add_device(track.id, PLUGIN_KIND, plugin=installed(window)["GIL Test Effect"])
    window.devices.widgets[other.id].save_preset()
    commands = window.undo_stack.count()
    widget = window.devices.widgets[device.id]
    widget.load_preset()
    assert window.undo_stack.count() == commands and "not for GIL Test Synth" in window.statusBar().currentMessage()


def test_editor_follows_its_device(window):
    track = window.editor.add_audio_track(name="Bass")
    device = window.editor.add_device(track.id, PLUGIN_KIND, plugin=installed(window)["GIL Test Effect"])
    assert window.bridge.open_plugin_editor(track.id, device.id)
    assert editor_window("GIL Test Effect - Bass")
    window.editor.remove_device(track.id, device.id)  # its editor closes with it
    assert not editor_window("GIL Test Effect - Bass")
    window.undo_stack.undo()
    assert window.bridge.open_plugin_editor(track.id, device.id)
    window.undo_stack.setClean()  # no "save changes?" question
    window.close()  # and they all close with the main window
    assert not editor_window("GIL Test Effect - Bass")
    QTest.qWait(1)


def window_rect(hwnd) -> tuple[int, int, int, int]:
    rect = wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rect))
    return rect.left, rect.top, rect.right, rect.bottom


def test_plugin_editors_follow_the_selected_track(window):
    effect = installed(window)["GIL Test Effect"]
    drums = window.editor.add_audio_track(name="Drums")
    bass = window.editor.add_audio_track(name="Bass")
    window.selection.select_track(drums.id)
    # Adding a plug-in shows its editor.
    on_drums = window.editor.add_device(drums.id, PLUGIN_KIND, plugin=effect)
    QTest.qWait(1)
    assert window.bridge.is_plugin_editor_open(drums.id, on_drums.id)
    assert window.devices.widgets[on_drums.id].edit.isChecked()
    drums_frame = editor_window("GIL Test Effect - Drums")
    user32.SetWindowPos(drums_frame, None, 40, 50, 0, 0, 0x0001 | 0x0004 | 0x0010)  # moved by the user
    place = window_rect(drums_frame)

    # Selecting another track hides it; a plug-in added there shows its own.
    window.selection.select_track(bass.id)
    assert not window.bridge.is_plugin_editor_open(drums.id, on_drums.id)
    assert not user32.IsWindowVisible(drums_frame)
    on_bass = window.editor.add_device(bass.id, PLUGIN_KIND, plugin=effect)
    QTest.qWait(1)
    assert window.bridge.is_plugin_editor_open(bass.id, on_bass.id)
    window.devices.widgets[on_bass.id].edit.click()  # the user closes it
    assert not editor_window("GIL Test Effect - Bass")

    # Coming back shows the editor again, where it was; the one closed stays closed.
    window.selection.select_track(drums.id)
    assert window.bridge.is_plugin_editor_open(drums.id, on_drums.id) and user32.IsWindowVisible(drums_frame)
    assert window_rect(drums_frame) == place and window.devices.widgets[on_drums.id].edit.isChecked()
    window.selection.select_track(bass.id)
    assert not window.bridge.is_plugin_editor_open(bass.id, on_bass.id)

    # Closed with its own close button, it stays closed too.
    window.selection.select_track(drums.id)
    user32.SendMessageW(drums_frame, 0x0010, 0, 0)  # WM_CLOSE
    poll(window)
    window.selection.select_track(None)
    window.selection.select_track(drums.id)
    assert not window.bridge.is_plugin_editor_open(drums.id, on_drums.id)

    # Undo and redo don't open editors (the last step added the effect on Bass).
    window.selection.select_track(bass.id)
    window.undo_stack.undo()
    window.undo_stack.redo()
    QTest.qWait(1)
    assert not window.bridge.is_plugin_editor_open(bass.id, on_bass.id)
    window.undo_stack.setClean()
