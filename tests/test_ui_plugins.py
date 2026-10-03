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
from PySide6.QtGui import (
    QAction,
    QDragLeaveEvent,
    QDragMoveEvent,
    QDropEvent,
    QKeySequence,
    QMouseEvent,
)
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication, QFileDialog, QWidget

from gilstudio import _engine as ge
from gilstudio.audio import engine_bridge
from gilstudio.model import automation
from gilstudio.model.automation import AutomationPoint
from gilstudio.model.project import PLUGIN_KIND, PluginRef
from gilstudio.ui.arrangement.lanes_canvas import DEVICE_MOVE_MIME
from gilstudio.ui.browser.browser_models import plugin_refs
from gilstudio.ui.browser.file_index import plugin_ref
from gilstudio.ui.device_panel import PluginDeviceWidget

from .conftest import TEST_PLUGINS
from .test_ui_smoke import drag, settle, wait_until
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
    settle(browser)
    names = [browser.list_model.item(browser.list_model.index(i)).name for i in range(browser.list_model.rowCount())]
    assert names == ["GIL Test Effect", "GIL Test Mono", "GIL Test Sidechain"]
    assert browser.status.text() == "3 plug-ins"
    browser.sidebar.setCurrentItem(plugins.child(0))
    settle(browser)
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
    # parameters), four at a time in a 2x2 grid.
    assert list(widget.knobs) == [GAIN, 6, 7] and list(widget.choices) == [WAVE]
    assert widget.choices[WAVE].currentText() == "Sine" and widget.pages == 3
    assert widget.page_label.text() == "1/3" and not widget.previous.isEnabled()
    positions = [widget.params.getItemPosition(i)[:2] for i in range(widget.params.count())]
    assert positions == [(0, 0), (0, 1), (1, 0), (1, 1)]
    widget.next.click()
    widget.next.click()
    assert list(widget.knobs) == [12, 13, 14, 15] and widget.page_label.text() == "3/3"
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
    assert window.devices.widgets[track.devices[0].id].pages == 3
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


class _Reports:
    """The engine, but with these plug-in reports to take (as if the plug-ins sent them)."""

    def __init__(self, engine, events):
        self._engine, self._events = engine, events

    def take_processor_events(self):
        events, self._events = self._events, []
        return events + list(self._engine.take_processor_events())

    def __getattr__(self, name):
        return getattr(self._engine, name)


def test_a_plugin_reporting_its_own_changes_makes_no_undo_steps(window):
    """Some plug-ins report their parameters as edited while their state is
    restored (a track with them duplicated, pasted, undone): only edits made in
    a shown editor are the user's."""
    track = window.editor.add_audio_track(name="Bus")
    window.selection.select_track(track.id, focus_track=True)
    device = window.editor.add_device(track.id, PLUGIN_KIND, plugin=installed(window)["GIL Test Effect"])
    window.editor.group_devices(track.id, [device.id])
    window.duplicate()
    [copy] = [t for t in window.project.tracks if t.id in window.selection.track_ids]
    copied = copy.devices[0].chains[0].devices[0]
    pid = engine_id(window, copy, copied)
    assert pid is not None and not window.engine.is_editor_open(pid)

    def report(processor_id, gesture=0):
        edited = SimpleNamespace(type=ge.ProcessorEventType.PARAM_EDITED, processor_id=processor_id,
                                 param_index=FX_GAIN, value=0.4, old_value=0.5, gesture=gesture)
        engine = window.bridge.engine
        window.bridge.engine = _Reports(engine, [edited])
        try:
            poll(window)
        finally:
            window.bridge.engine = engine

    steps = window.undo_stack.count()
    report(pid)
    assert window.undo_stack.count() == steps and window.undo_stack.undoText() == "Duplicate Track"
    # From a shown editor, it is the user's: an undo step.
    window.bridge.open_plugin_editor(copy.id, copied.id)
    assert window.engine.is_editor_open(pid)
    report(pid, gesture=7)
    assert window.undo_stack.count() == steps + 1 and window.undo_stack.undoText() == "Change Device Parameter"


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


def test_automating_plugin_parameters(window):
    track, device = synth_track(window)
    pid = engine_id(window, track, device)
    knob, _readout = window.devices.widgets[device.id].knobs[GAIN]
    key = automation.device_key(device.id, str(GAIN))
    centre = QPoint(knob.width() // 2, knob.height() // 2)
    drag(knob, centre, centre + QPoint(0, 30))  # turning it shows its automation
    assert window.project.track(track.id).automation_view.key == key
    assert window.arrangement.headers.headers[track.id].automation.main.device.text() == "GIL Test Synth"
    assert window.bridge.param_spec(track.id, key).name == "Gain"
    # An envelope drives it: the plug-in, its controller and the knob follow.
    window.editor.set_envelope(track.id, key, (AutomationPoint(0.0, 0.25),))
    window.engine.render_offline(0.0, 256)
    poll(window)
    assert window.engine.processor_param(pid, GAIN) == pytest.approx(0.25)
    assert knob.value() == pytest.approx(0.25) and knob.automation() == "on"
    # Turning it by hand overrides the envelope, until automation is re-enabled.
    drag(knob, centre, centre + QPoint(0, 30))
    assert window.bridge.is_overridden(track.id, key) and knob.automation() == "off"
    window.engine.render_offline(0.0, 256)
    poll(window)
    assert window.engine.processor_param(pid, GAIN) == pytest.approx(knob.value(), abs=1e-3) != 0.25
    window.bridge.re_enable_automation()
    window.engine.render_offline(0.0, 256)
    poll(window)
    assert window.engine.processor_param(pid, GAIN) == pytest.approx(0.25)


def test_edits_in_a_plugin_editor_override_its_automation(window):
    track = window.editor.add_audio_track(name="Drums")
    window.selection.select_track(track.id)
    device = window.editor.add_device(track.id, PLUGIN_KIND, plugin=installed(window)["GIL Test Effect"])
    pid = engine_id(window, track, device)
    key = automation.device_key(device.id, str(FX_GAIN))
    window.editor.set_envelope(track.id, key, (AutomationPoint(0.0, 0.8),))
    window.engine.render_offline(0.0, 256)
    poll(window)
    assert window.engine.processor_param(pid, FX_GAIN) == pytest.approx(0.8)
    widget = window.devices.widgets[device.id]
    widget.edit.click()  # shows the editor
    view = user32.GetWindow(editor_window("GIL Test Effect - Drums"), 5)
    user32.SendMessageW(view, EDIT_GAIN, 0, 0)  # the editor sets 0.3
    poll(window)
    assert window.bridge.is_overridden(track.id, key)
    assert window.project.track(track.id).automation_view.key == key  # and its lane shows
    window.engine.render_offline(0.0, 256)
    poll(window)
    assert window.engine.processor_param(pid, FX_GAIN) == pytest.approx(0.3)


def test_devices_fit_the_device_view(window):
    """The tallest devices (a page of knobs with page arrows, a plug-in's long error)
    fit with the horizontal scroll bar showing: the chain never scrolls vertically."""
    window.resize(900, 700)
    window.show()
    track, _ = synth_track(window)
    window.editor.add_device(track.id, "ott")
    missing = PluginRef(format="VST3", uid="0" * 32, name="Missing", path="C:/nowhere/Missing.vst3")
    broken = window.editor.add_device(track.id, PLUGIN_KIND, plugin=missing)
    window.bridge.plugin_errors[broken.id] = "Could not load this plug-in. " * 20
    window.devices.show_track(None)
    window.devices.show_track(track.id)
    for _ in range(4):
        window.editor.add_device(track.id, "utility")
    QTest.qWait(10)
    scroll = window.devices.scroll
    assert scroll.horizontalScrollBar().isVisible()
    assert scroll.verticalScrollBar().maximum() == 0
    assert window.devices.chain.minimumSizeHint().height() <= scroll.viewport().height()


def test_double_click_opens_and_ctrl_w_closes_the_editor(window, monkeypatch):
    from gilstudio.ui import plugin_keys

    track = window.editor.add_audio_track(name="Keys")
    window.selection.select_track(track.id)
    effect = installed(window)["GIL Test Effect"]
    first = window.editor.add_device(track.id, PLUGIN_KIND, plugin=effect)
    second = window.editor.add_device(track.id, PLUGIN_KIND, plugin=effect)
    for device in (first, second):
        window.bridge.close_plugin_editor(track.id, device.id)  # adding it may have shown it
    assert plugin_keys.foremost_editor() is None
    widgets = [window.devices.widgets[d.id] for d in (first, second)]

    def double_click(widget) -> None:
        QTest.mouseDClick(widget, Qt.MouseButton.LeftButton, pos=widget.title.geometry().center())

    double_click(widgets[0])
    first_frame = plugin_keys.foremost_editor()
    assert first_frame and widgets[0].edit.isChecked()
    double_click(widgets[1])
    second_frame = plugin_keys.foremost_editor()
    assert second_frame not in (None, first_frame)
    double_click(widgets[0])  # open already: the same window comes to the front
    assert plugin_keys.foremost_editor() == first_frame

    # Ctrl+W in the main window closes the editor in front...
    [close] = [a for a in window.findChildren(QAction) if a.shortcut() == QKeySequence("Ctrl+W")]
    close.trigger()
    assert wait_until(lambda: not user32.IsWindow(first_frame))
    poll(window)
    assert not widgets[0].edit.isChecked() and widgets[1].edit.isChecked()
    assert plugin_keys.foremost_editor() == second_frame

    # ...and so does Ctrl+W in an editor (the plug-in's view has the focus).
    view = user32.GetWindow(second_frame, 5)  # GW_CHILD
    monkeypatch.setattr(plugin_keys, "pressed_modifiers", lambda: Qt.KeyboardModifier.ControlModifier)
    msg = wintypes.MSG(hWnd=view, message=plugin_keys.WM_KEYDOWN, wParam=ord("W"))
    assert window._plugin_shortcuts.nativeEventFilter(b"windows_generic_MSG", ctypes.addressof(msg))[0]
    assert wait_until(lambda: not user32.IsWindow(second_frame))
    poll(window)
    assert not widgets[1].edit.isChecked() and plugin_keys.foremost_editor() is None
    close.trigger()  # none left: nothing happens


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
    settle(browser)
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


def test_dragging_a_device_to_another_track_moves_its_processor(window):
    refs = installed(window)
    window.insert_track()
    window.insert_track()
    a, b = window.project.tracks
    utility = window.editor.add_device(b.id, "utility")
    effect = window.editor.add_device(a.id, PLUGIN_KIND, plugin=refs["GIL Test Effect"])
    pid = engine_id(window, a, effect)
    window.engine.set_processor_param(pid, FX_GAIN, 0.3)  # as if changed in its editor: only the plug-in knows
    key = automation.device_key(effect.id, str(FX_GAIN))
    window.editor.set_envelope(a.id, key, (AutomationPoint(0.0, 0.5),))
    chain_of = {t.id: window.engine.track_chain(window.bridge._track_ids[t.id]) for t in (a, b)}

    arrangement = window.arrangement
    row = next(r for r in arrangement.layout_model.rows if r.track_id == b.id)
    mime = QMimeData()
    mime.setData(DEVICE_MOVE_MIME, f"{a.id}\n{effect.id}".encode())
    arrangement.lanes.dropEvent(drop(QPointF(40, row.top - arrangement.view.scroll_y + 10), mime))
    a, b = window.project.tracks
    assert a.devices == [] and [d.id for d in b.devices] == [utility.id, effect.id]
    # The same processor, moved in the engine: nothing loaded again, nothing lost.
    assert engine_id(window, b, effect) == pid and engine_id(window, a, effect) is None
    assert window.engine.processor_chain(pid) == chain_of[b.id]
    assert window.engine.processor_param(pid, FX_GAIN) == pytest.approx(0.3)
    assert window.project.envelope(b.id, key) and not window.project.envelope(a.id, key)  # automation goes along
    assert window.selection.track_id == b.id  # shows where it went

    window.undo_stack.undo()  # one step: back where it was
    a, b = window.project.tracks
    assert [d.id for d in a.devices] == [effect.id] and [d.id for d in b.devices] == [utility.id]
    assert engine_id(window, a, effect) == pid and window.engine.processor_chain(pid) == chain_of[a.id]
    assert window.project.envelope(a.id, key) and not window.project.envelope(b.id, key)
    window.undo_stack.redo()
    assert engine_id(window, window.project.tracks[1], effect) == pid

    # The device panel starts such drags; dropped on its own track it does nothing here.
    arrangement.lanes.dropEvent(drop(QPointF(40, row.top - arrangement.view.scroll_y + 10), mime))
    assert [d.id for d in window.project.tracks[1].devices] == [utility.id, effect.id]


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
    window.selection.clear()  # selecting something else deselects the devices
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
    window.selection.select_clips(window.editor, {clip})
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


def test_plugin_folders_in_preferences(window, tmp_path):
    import shutil

    from gilstudio.plugins.settings import custom_folders
    from gilstudio.ui.dialogs import PreferencesDialog

    index = window.browser.plugin_index
    before = len(installed(window))
    extra = tmp_path / "More VST3"
    shutil.copytree(TEST_PLUGINS, extra / "Vendor" / TEST_PLUGINS.name)
    (extra / "Broken.vst3").write_bytes(b"not a plug-in")

    prefs = PreferencesDialog(window.bridge, window, plugins=index)
    page = prefs.plugins
    assert [prefs.tabs.tabText(i) for i in range(prefs.tabs.count())] == ["Audio", "MIDI", "Plug-ins"]
    assert page.folders.count() == 1 and "(standard)" in page.folders.item(0).text()
    page.folders.setCurrentRow(0)
    assert not page.remove_button.isEnabled()  # the standard folders stay

    # An added folder is kept, and its plug-ins (in folders inside it too) are found.
    page.add_folder(str(extra))
    page.add_folder(str(extra) + "\\")  # the same folder again: nothing changes
    assert custom_folders() == [str(extra)]
    assert page.folders.count() == 2 and page.remove_button.isEnabled()
    assert wait_until(lambda: not index.scanning, timeout=30)
    paths = {p.path for p in index.plugins}
    assert len(index.plugins) == 2 * before and str(extra / "Vendor" / TEST_PLUGINS.name) in paths
    assert [f.path for f in index.failures] == [str(extra / "Broken.vst3")]
    assert page.scan_status.text() == f"{2 * before} plug-ins found · 1 file could not be read"
    assert page.rescan_button.isEnabled()

    # A rescan reads everything again; a removed folder's plug-ins go.
    page.rescan()
    assert index.scanning and not page.rescan_button.isEnabled()
    assert wait_until(lambda: not index.scanning, timeout=30)
    assert len(index.plugins) == 2 * before
    page.remove_folder()
    assert custom_folders() == [] and page.folders.count() == 1
    assert wait_until(lambda: not index.scanning, timeout=30)
    assert len(index.plugins) == before and not index.failures
    assert page.scan_status.text() == f"{before} plug-ins found"
    prefs.reject()


def test_effects_on_the_master(window, tmp_path):
    """The master is selected by clicking its header; the device view then shows its
    chain, which takes effects (not instruments), plays and saves like a track's."""
    import numpy as np

    from gilstudio.model.automation import MASTER

    from .conftest import SAMPLE_RATE, write_wav

    path = str(write_wav(tmp_path / "dc.wav", np.full((SAMPLE_RATE, 2), 0.5)))
    window.editor.add_clips(None, 0.0, [(path, 1.0)])
    assert wait_until(lambda: window.bridge.source(path) is not None)
    header = window.arrangement.master_header
    QTest.mouseClick(header, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, QPoint(40, 10))
    assert window.selection.track_id == MASTER and header.selected
    assert window.devices.track_id == MASTER

    window.add_device_to_selected_track("utility")  # from the browser
    [utility] = window.project.master.devices
    assert list(window.devices.widgets) == [utility.id]
    processor = window.bridge.engine_device_id(MASTER, utility.id)
    assert processor is not None
    window.editor.set_device_param(MASTER, utility.id, "gain", -6.0206)
    assert window.engine.render_offline(0.0, 8000)[6000, 0] == pytest.approx(0.25, rel=1e-3)
    assert window.editor.add_device(MASTER, "synth") is None  # instruments go on MIDI tracks

    effect = window.editor.add_device(MASTER, PLUGIN_KIND, plugin=installed(window)["GIL Test Effect"])
    assert window.bridge.engine_device_id(MASTER, effect.id) is not None
    assert [d.id for d in window.project.master.devices] == [utility.id, effect.id]
    poll(window)
    assert window.bridge.plugin_errors == {}

    # Its devices can be automated, in the master's lanes.
    groups = [group for group, _name, _specs in window.bridge.param_groups(MASTER)]
    assert groups == ["mixer", utility.id, effect.id]
    key = automation.device_key(utility.id, "gain")
    window.editor.set_envelope(MASTER, key, (AutomationPoint(0.0, 0.0),))
    assert window.bridge.is_automated(MASTER, key)
    assert window.engine.render_offline(0.0, 8000)[6000, 0] < 0.01  # the utility's lowest gain

    # Saved and opened again: with its plug-in's state.
    target = tmp_path / "master.gilproj"
    assert window._save_to(target)
    window.new_project()
    assert window.project.master.devices == [] and window.devices.track_id is None
    window.open_project(str(target))
    assert [d.id for d in window.project.master.devices] == [utility.id, effect.id]
    assert window.bridge.engine_device_id(MASTER, effect.id) is not None
    assert window.bridge.is_automated(MASTER, key)

    # Undo takes a device off the master again.
    window.editor.remove_device(MASTER, effect.id)
    assert [d.id for d in window.project.master.devices] == [utility.id]
    window.undo_stack.undo()
    assert window.bridge.engine_device_id(MASTER, effect.id) is not None
