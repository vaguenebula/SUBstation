"""VST3 plug-ins in the application: the browser, the device view, editors,
undo, and projects. Drives the real main window offscreen with the test
plug-ins (see tests/vst3_plugins); plug-in editors are real Win32 windows."""

import base64
import ctypes
import json
from ctypes import wintypes
from dataclasses import asdict
from types import SimpleNamespace

import pytest
from PySide6.QtCore import QEvent, QMimeData, QObject, QPoint, QPointF, Qt
from PySide6.QtGui import QDragLeaveEvent, QDragMoveEvent, QDropEvent, QMouseEvent
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication, QFileDialog, QWidget

from gilstudio import _engine as ge
from gilstudio.audio import engine_bridge
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


def test_selecting_deleting_and_reordering_devices(window):
    refs = installed(window)
    track, synth = synth_track(window)
    effects = [window.editor.add_device(track.id, PLUGIN_KIND, plugin=refs[name])
               for name in ("GIL Test Effect", "GIL Test Mono", "GIL Test Effect")]
    window.editor.add_device(track.id, "utility")
    panel = window.devices
    chain = lambda: [d.id for d in window.project.track(track.id).devices]
    first, second, third, utility = chain()[1:]
    assert [first, second, third] == [e.id for e in effects]

    def click(device_id, modifiers=Qt.KeyboardModifier.NoModifier):
        QTest.mouseClick(panel.widgets[device_id].title, Qt.MouseButton.LeftButton, modifiers)

    # Clicks on a device's title select it; Shift selects a range, Ctrl one more or less.
    click(first)
    assert panel.selected == [first] and panel.widgets[first].selected and window.selection.focus == "devices"
    click(utility, Qt.KeyboardModifier.ShiftModifier)
    assert panel.selected == [first, second, third, utility]
    click(second, Qt.KeyboardModifier.ControlModifier)
    assert panel.selected == [first, third, utility] and not panel.widgets[second].selected
    click(synth.id)
    click(second, Qt.KeyboardModifier.ShiftModifier)  # the instrument can be selected too
    assert panel.selected == [synth.id, first, second]

    # Delete deletes them all, in one undo step.
    steps = window.undo_stack.count()
    window.delete_selection()
    assert chain() == [third, utility] and window.undo_stack.count() == steps + 1
    window.undo_stack.undo()
    assert chain() == [synth.id, first, second, third, utility]
    window.selection.set_clips(set())  # selecting something else deselects the devices
    assert panel.selected == [] and not any(w.selected for w in panel.widgets.values())
    window.delete_selection()
    assert len(chain()) == 5

    # Dragging effects (selected together) reorders them; the instrument stays first.
    def drop_before(device_id, moving):
        mime = QMimeData()
        mime.setData("application/x-gilstudio-device-move", "\n".join([track.id, *moving]).encode())
        widget = panel.widgets[device_id]
        pos = widget.mapTo(panel, QPoint(2, widget.height() // 2))
        panel.dropEvent(drop(QPointF(pos), mime))

    drop_before(first, [third, utility])
    assert chain() == [synth.id, third, utility, first, second]
    assert window.undo_stack.undoText() == "Move Devices"
    drop_before(synth.id, [second, synth.id])  # nothing goes before the instrument, which doesn't move
    assert chain() == [synth.id, second, third, utility, first]
    window.undo_stack.undo()
    window.undo_stack.undo()
    assert chain() == [synth.id, first, second, third, utility]
    # A device's menu offers Delete for all the selected devices.
    click(first)
    click(second, Qt.KeyboardModifier.ShiftModifier)
    panel.widgets[first].remove_selected()
    assert chain() == [synth.id, third, utility]
    window.undo_stack.setClean()


def test_device_view_review_fixes(window, monkeypatch):
    refs = installed(window)
    panel = window.devices

    # Dropped from the browser with an instrument, effects land where they were dropped.
    track = window.editor.add_midi_track(instrument=None)
    window.selection.select_track(track.id)
    fx1 = window.editor.add_device(track.id, "utility").id
    fx2 = window.editor.add_device(track.id, "utility").id
    chain = lambda: [d.id for d in window.project.track(track.id).devices]

    def drop_at(device_id, *plugins, kinds=()):
        mime = QMimeData()
        if plugins:
            mime.setData("application/x-gilstudio-plugin", json.dumps([asdict(refs[n]) for n in plugins]).encode())
        if kinds:
            mime.setData("application/x-gilstudio-device", json.dumps(list(kinds)).encode())
        QTest.qWait(1)  # the rebuilt device view laid out
        widget = panel.widgets[device_id]
        panel.dropEvent(drop(QPointF(widget.mapTo(panel, QPoint(2, widget.height() // 2))), mime))

    drop_at(fx2, "GIL Test Synth", "GIL Test Effect")  # between the two utilities
    synth, _, effect, _ = chain()
    assert chain() == [synth, fx1, effect, fx2]
    drop_at(synth, "GIL Test Mono", "GIL Test Effect")  # before the instrument: right after it, in order
    names = [d.plugin.name if d.plugin else d.kind for d in window.project.track(track.id).devices]
    assert names[1:3] == ["GIL Test Mono", "GIL Test Effect"] and chain()[0] == synth and chain()[3:] == [fx1, effect, fx2]

    # With the devices in focus, a second Delete doesn't delete clips selected before.
    clip = window.editor.add_midi_clip(track.id, 0.0, 4.0)
    window.selection.set_clips({clip})
    QTest.mouseClick(panel.widgets[fx1].title, Qt.MouseButton.LeftButton)
    window.delete_selection()
    window.delete_selection()
    assert fx1 not in chain() and window.project.track(track.id).clips

    # A hidden editor takes its track's new name.
    window.selection.select_track(None)
    assert not window.bridge.is_plugin_editor_open(track.id, effect)
    window.editor.rename_track(track.id, "Sub")
    window.selection.select_track(track.id)
    assert window.bridge.is_plugin_editor_open(track.id, effect) and editor_window("GIL Test Effect - Sub")

    # Reopening an editor may change the chains (a plug-in running a message loop).
    window.selection.select_track(None)
    window.engine.close_editor(engine_id(window, track, window.project.device(track.id, effect)))
    reopen = window.bridge.open_plugin_editor

    def open_and_add_a_track(*args, **kwargs):
        window.editor.add_midi_track()  # a track with a device: a new chain
        return reopen(*args, **kwargs)

    monkeypatch.setattr(window.bridge, "open_plugin_editor", open_and_add_a_track)
    window.selection.select_track(track.id)
    assert reopen(track.id, effect)
    window.undo_stack.setClean()


class _EngineWithEvents:
    """The engine, reporting some processor events of our own."""

    def __init__(self, engine, events):
        self._engine, self._events = engine, events

    def take_processor_events(self):
        events, self._events = self._events, []
        return events

    def __getattr__(self, name):
        return getattr(self._engine, name)


def test_plugin_editor_review_fixes(window, monkeypatch):
    effect = installed(window)["GIL Test Effect"]
    bridge = window.bridge

    def track_with_editor(name):
        track = window.editor.add_audio_track(name=name)
        window.selection.select_track(track.id)
        device = window.editor.add_device(track.id, PLUGIN_KIND, plugin=effect)
        QTest.qWait(1)
        assert bridge.is_plugin_editor_open(track.id, device.id)
        return track, device

    # A device deleted with its editor open and restored by undo doesn't bring
    # its editor back later, when its track is selected again.
    one, on_one = track_with_editor("One")
    window.editor.remove_device(one.id, on_one.id)
    window.undo_stack.undo()
    window.selection.select_track(None)
    window.selection.select_track(one.id)
    assert not bridge.is_plugin_editor_open(one.id, on_one.id)

    # A plug-in asking for its editor gets it when its track is shown, not before.
    two = window.editor.add_audio_track(name="Two")
    window.selection.select_track(two.id)
    asks = SimpleNamespace(type=ge.ProcessorEventType.EDITOR_REQUESTED, processor_id=engine_id(window, one, on_one))
    monkeypatch.setattr(bridge, "engine", _EngineWithEvents(window.engine, [asks]))
    poll(window)
    assert not editor_window("GIL Test Effect - One")
    monkeypatch.setattr(bridge, "engine", window.engine)
    window.selection.select_track(one.id)
    assert bridge.is_plugin_editor_open(one.id, on_one.id)
    window.devices.widgets[on_one.id].edit.click()

    # Only so many hidden editors keep running: the one hidden longest closes, and
    # opens again where it was when its track is shown.
    monkeypatch.setattr(engine_bridge, "MAX_HIDDEN_EDITORS", 2)
    tracks = [track_with_editor("Hidden 0")]
    user32.SetWindowPos(editor_window("GIL Test Effect - Hidden 0"), None, 40, 50, 0, 0,
                        0x0001 | 0x0004 | 0x0010)  # moved by the user
    tracks += [track_with_editor(f"Hidden {i}") for i in range(1, 4)]
    window.selection.select_track(two.id)  # hides the last one too: 4 hidden
    assert not editor_window("GIL Test Effect - Hidden 0") and not editor_window("GIL Test Effect - Hidden 1")
    assert editor_window("GIL Test Effect - Hidden 2") and editor_window("GIL Test Effect - Hidden 3")
    window.selection.select_track(tracks[0][0].id)
    assert bridge.is_plugin_editor_open(tracks[0][0].id, tracks[0][1].id)
    assert window_rect(editor_window("GIL Test Effect - Hidden 0"))[:2] == (40, 50)
    window.undo_stack.setClean()


def test_dragging_devices_scrolls_the_chain(window):
    track = window.editor.add_audio_track()
    window.selection.select_track(track.id)
    for _ in range(12):
        window.editor.add_device(track.id, "utility")
    panel = window.devices
    bar = panel.scroll.horizontalScrollBar()
    assert wait_until(lambda: bar.maximum() > 0), (panel.scroll.viewport().width(), panel.chain.width())
    QTest.qWait(1)  # scrolled to the last device added
    bar.setValue(0)
    mime = QMimeData()
    mime.setData("application/x-gilstudio-device-move", f"{track.id}\n{track.devices[0].id}".encode())
    viewport = panel.scroll.viewport()

    def drag_to(x):
        pos = viewport.mapTo(panel, QPoint(x, viewport.height() // 2))
        panel.dragMoveEvent(QDragMoveEvent(pos, Qt.DropAction.MoveAction, mime, Qt.MouseButton.LeftButton,
                                           Qt.KeyboardModifier.NoModifier))

    drag_to(viewport.width() - 5)  # held at the right edge, the chain scrolls on
    assert wait_until(lambda: bar.value() == bar.maximum())
    drag_to(viewport.width() // 2)  # away from the edges, it stops
    stopped = bar.value()
    drag_to(5)
    assert wait_until(lambda: bar.value() < stopped)
    panel.dragLeaveEvent(QDragLeaveEvent())
    left = bar.value()
    QTest.qWait(50)
    assert bar.value() == left and not panel.drop_marker.isVisible()


def test_ctrl_alt_drag_scrolls_the_chain(window):
    track = window.editor.add_audio_track()
    window.selection.select_track(track.id)
    for _ in range(12):
        window.editor.add_device(track.id, "utility")
    panel = window.devices
    bar = panel.scroll.horizontalScrollBar()
    assert wait_until(lambda: bar.maximum() > 0)
    QTest.qWait(1)  # scrolled to the last device added
    bar.setValue(0)
    device = track.devices[0].id
    knob = panel.widgets[device].findChildren(QWidget)[-1]  # the press can land on a knob
    pan = Qt.KeyboardModifier.ControlModifier | Qt.KeyboardModifier.AltModifier
    left = Qt.MouseButton.LeftButton
    local = QPointF(knob.width() / 2, knob.height() / 2)
    start = QPointF(knob.mapToGlobal(local.toPoint()))  # the mouse's screen position; the knob moves

    def send(kind, x, buttons):
        screen = start + QPointF(x, 0)
        QApplication.sendEvent(knob, QMouseEvent(kind, local, screen, left, buttons, pan))

    send(QEvent.Type.MouseButtonPress, 0, left)
    send(QEvent.Type.MouseMove, -150, left)  # drag left: the chain scrolls right
    assert bar.value() == min(150, bar.maximum())
    send(QEvent.Type.MouseMove, -50, left)
    assert bar.value() == 50
    send(QEvent.Type.MouseButtonRelease, -50, Qt.MouseButton.NoButton)
    assert panel.selected == [] and QApplication.overrideCursor() is None
    send(QEvent.Type.MouseMove, -300, Qt.MouseButton.NoButton)  # the pan is over
    assert bar.value() == 50


def test_adding_a_plugin_scrolls_to_it(window):
    refs = installed(window)
    track = window.editor.add_midi_track(instrument=None)
    window.selection.select_track(track.id)
    for _ in range(12):
        window.editor.add_device(track.id, "utility")
    panel = window.devices
    bar = panel.scroll.horizontalScrollBar()
    assert wait_until(lambda: bar.maximum() > 0)
    viewport = panel.scroll.viewport()

    def in_view(device_id):
        widget = panel.widgets[device_id]
        left = widget.mapTo(viewport, QPoint(0, 0)).x()
        return left >= 0 and left + widget.width() <= viewport.width()

    # Added from the browser while scrolled to the start: the chain scrolls to it.
    bar.setValue(0)
    window.add_device_to_selected_track(PLUGIN_KIND, refs["GIL Test Effect"])
    added = track.devices[-1].id
    assert wait_until(lambda: bar.value() > 0) and in_view(added)

    # Dropped on the chain, where the user is looking, it doesn't scroll, even for
    # an instrument, which goes first.
    bar.setValue(bar.maximum())
    mime = QMimeData()
    mime.setData("application/x-gilstudio-plugin", json.dumps([asdict(refs["GIL Test Synth"])]).encode())
    last = panel.widgets[added]
    panel.dropEvent(drop(QPointF(last.mapTo(panel, QPoint(2, last.height() // 2))), mime))
    assert track.devices[0].plugin == refs["GIL Test Synth"]
    QTest.qWait(50)
    assert in_view(added) and not in_view(track.devices[0].id)
