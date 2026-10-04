"""Device presets in the window: every device's save button saves it to the
library under a name asked for; the browser's Presets section lists presets by
device; dragged onto the device view a preset goes in as a new device, or loads
into the device it is dropped onto if it is of its kind (one undo step);
double-clicked it goes on the selected track (an instrument with no MIDI track
selected: on a new one); dropped on a track in the arrangement, on that track.
Presets are renamed and deleted from the browser."""

import time
import types
from pathlib import Path

from PySide6.QtCore import QPoint, QPointF, Qt
from PySide6.QtGui import QDropEvent
from PySide6.QtWidgets import QInputDialog, QMessageBox

from substation.model.project import Device
from substation.model.serialization import PRESET_EXTENSION, load_preset, save_preset
from substation.ui.browser import browser_panel
from substation.ui.browser.browser_models import preset_paths
from substation.ui.browser.preset_index import PresetIndex
from substation.ui.device_panel import RackWidget

from .test_ui_smoke import settle


def drop(pos, mime) -> QDropEvent:
    return QDropEvent(QPointF(pos), Qt.DropAction.CopyAction, mime, Qt.MouseButton.LeftButton,
                      Qt.KeyboardModifier.NoModifier)


def center(panel, widget) -> QPoint:
    return widget.mapTo(panel, widget.rect().center())


def shown_track(window, kinds=("utility",)):
    window.insert_track()
    track = window.project.tracks[0]
    window.selection.select_track(track.id)
    return track, [window.editor.add_device(track.id, kind) for kind in kinds]


def sidebar_entry(browser, *names):
    items = [browser.sidebar.topLevelItem(i) for i in range(browser.sidebar.topLevelItemCount())]
    item = next(i for i in items if i.text(0) == names[0])
    for name in names[1:]:
        item = next(item.child(j) for j in range(item.childCount()) if item.child(j).text(0) == name)
    return item


def listed(browser):
    settle(browser)
    model = browser.list_model
    return {model.item(model.index(i)).name: model.index(i) for i in range(model.rowCount())}


def save(window, device_id, name, monkeypatch) -> Path:
    """The device's save button, answered with `name`."""
    monkeypatch.setattr(QInputDialog, "getText", lambda *args, **kwargs: (name, True))
    window.devices.widgets[device_id].save.click()
    return Path(window.browser.preset_index.root) / "Utility" / f"{name}{PRESET_EXTENSION}"


def test_the_save_button_saves_to_the_library_and_the_browser_lists_it(window, monkeypatch):
    track, (utility,) = shown_track(window)
    window.editor.set_device_param(track.id, utility.id, "gain", -7.0)
    path = save(window, utility.id, "Quieter", monkeypatch)
    assert load_preset(path).params["gain"] == -7.0
    assert "Saved the preset Quieter" in window.statusBar().currentMessage()

    browser = window.browser
    entry = sidebar_entry(browser, "Presets", "Utility")
    browser.sidebar.setCurrentItem(entry)
    rows = listed(browser)
    assert list(rows) == ["Quieter"]
    assert preset_paths(browser.list_model.mimeData([rows["Quieter"]])) == [str(path)]
    assert browser.status.text() == "1 preset"
    # Searching "All" finds it too.
    browser.focus_search()
    browser.search.setText("quieter")
    assert "Quieter" in listed(browser)

    # Saving again under that name asks first; "No" keeps the preset as it was.
    window.editor.set_device_param(track.id, utility.id, "gain", -1.0)
    monkeypatch.setattr(QMessageBox, "question", lambda *args: QMessageBox.StandardButton.No)
    save(window, utility.id, "Quieter", monkeypatch)
    assert load_preset(path).params["gain"] == -7.0
    monkeypatch.setattr(QMessageBox, "question", lambda *args: QMessageBox.StandardButton.Yes)
    save(window, utility.id, "Quieter", monkeypatch)
    assert load_preset(path).params["gain"] == -1.0


def test_every_kind_of_device_has_a_working_save_button(window, monkeypatch):
    _track, (utility, compressor) = shown_track(window, ("utility", "compressor"))
    window.devices.select_device(compressor.id)
    assert window.devices.group_selected()
    rack = next(w for w in window.devices.widgets.values() if isinstance(w, RackWidget))
    monkeypatch.setattr(QInputDialog, "getText", lambda *args, **kwargs: ("Mine", True))
    rack.save.click()
    window.devices.widgets[utility.id].save.click()
    root = Path(window.browser.preset_index.root)
    assert (root / "Audio Effect Rack" / f"Mine{PRESET_EXTENSION}").exists()
    assert (root / "Utility" / f"Mine{PRESET_EXTENSION}").exists()
    assert [sidebar_entry(window.browser, "Presets").child(i).text(0) for i in range(2)] == [
        "Audio Effect Rack", "Utility"]


def test_dropping_a_preset_on_the_device_view(window, monkeypatch):
    track, (source, compressor) = shown_track(window, ("utility", "compressor"))
    editor, devices, project = window.editor, window.devices, window.project
    editor.set_device_param(track.id, source.id, "gain", -11.0)
    path = save(window, source.id, "Down", monkeypatch)
    editor.remove_device(track.id, source.id)
    mime = window.browser.list_model.mimeData([])  # (an empty drag, filled in below)
    mime.setData("application/x-substation-preset", f'["{path.as_posix()}"]'.encode())

    # Between devices (here: before the compressor): a new device.
    devices.dropEvent(drop(QPoint(4, 20), mime))
    assert [d.kind for d in project.track(track.id).devices] == ["utility", "compressor"]
    new = project.track(track.id).devices[0]
    assert new.params["gain"] == -11.0 and window.undo_stack.undoText() == "Load Preset Down"

    # Onto a device of another kind: a new device too, where it was dropped (its middle: before it).
    devices.dropEvent(drop(center(devices, devices.widgets[compressor.id]), mime))
    assert [d.kind for d in project.track(track.id).devices] == ["utility", "utility", "compressor"]
    window.undo_stack.undo()

    # Onto a device of its kind: loads into it, one undo step.
    target = editor.add_device(track.id, "utility")
    count = window.undo_stack.count()
    pos = center(devices, devices.widgets[target.id])
    devices.dragEnterEvent(drop(pos, mime))  # (a QDropEvent is a QDragMoveEvent: it serves for both)
    assert devices.load_marker.isVisible() and not devices.drop_marker.isVisible()
    devices.dropEvent(drop(pos, mime))
    assert not devices.load_marker.isVisible()
    assert [d.id for d in project.track(track.id).devices][-1] == target.id
    assert project.device(track.id, target.id).params["gain"] == -11.0
    assert window.undo_stack.count() == count + 1
    window.undo_stack.undo()
    assert project.device(track.id, target.id).params["gain"] == 0.0


def test_double_clicking_presets(window, monkeypatch):
    browser, project = window.browser, window.project
    track, (utility,) = shown_track(window)
    save(window, utility.id, "Plain", monkeypatch)
    browser.sidebar.setCurrentItem(sidebar_entry(browser, "Presets"))
    browser._activate_list(listed(browser)["Plain"])
    assert [d.kind for d in project.track(track.id).devices] == ["utility", "utility"]

    # An instrument preset with an audio track selected: on a new MIDI track, one undo step.
    keys = window.editor.add_midi_track()
    window.selection.select_track(keys.id)
    monkeypatch.setattr(QInputDialog, "getText", lambda *args, **kwargs: ("Pad", True))
    window.devices.widgets[keys.devices[0].id].save.click()
    window.selection.select_track(track.id)
    tracks = len(project.tracks)
    count = window.undo_stack.count()
    browser._activate_list(listed(browser)["Pad"])
    assert len(project.tracks) == tracks + 1 and window.undo_stack.count() == count + 1
    new = project.track(window.selection.track_id)
    assert new.is_midi and [d.kind for d in new.devices] == [keys.devices[0].kind]


def test_dropping_a_preset_on_a_track_in_the_arrangement(window, monkeypatch):
    _track, (utility,) = shown_track(window)
    path = save(window, utility.id, "Plain", monkeypatch)
    window.insert_track()
    other = window.project.tracks[1]
    mime = window.browser.list_model.mimeData([])
    mime.setData("application/x-substation-preset", f'["{path.as_posix()}"]'.encode())
    arrangement = window.arrangement
    row = next(r for r in arrangement.layout_model.rows if r.track_id == other.id)
    arrangement.lanes.dropEvent(drop(QPointF(40, row.top - arrangement.view.scroll_y + 10), mime))
    assert [d.kind for d in window.project.track(other.id).devices] == ["utility"]
    assert window.selection.track_id == other.id


def test_renaming_and_deleting_presets_in_the_browser(window, monkeypatch):
    browser = window.browser
    _track, (utility,) = shown_track(window)
    path = save(window, utility.id, "Old", monkeypatch)
    browser.sidebar.setCurrentItem(sidebar_entry(browser, "Presets", "Utility"))
    assert list(listed(browser)) == ["Old"]
    new = browser.rename_preset(str(path), "New")
    assert Path(new).name == f"New{PRESET_EXTENSION}" and not path.exists()
    assert list(listed(browser)) == ["New"]
    # Ctrl+R with the list focused renames the preset selected there.
    browser.list_view.setCurrentIndex(browser.list_model.index(0, 0))
    browser.list_view.setFocus()
    monkeypatch.setattr(browser_panel.QInputDialog, "getText", lambda *_a, **_k: ("Newer", True))
    window.rename()
    new = str(Path(new).with_name(f"Newer{PRESET_EXTENSION}"))
    assert list(listed(browser)) == ["Newer"] and Path(new).exists()

    trashed = []  # (not the computer's recycle bin)
    monkeypatch.setattr(browser_panel, "QFile", types.SimpleNamespace(
        moveToTrash=lambda p: trashed.append(p) or Path(p).unlink() or True))
    assert browser.delete_preset(new, confirm=False)
    assert trashed == [new]
    # The library is empty: the Presets section has no groups, and lists nothing.
    assert sidebar_entry(browser, "Presets").childCount() == 0
    browser.sidebar.setCurrentItem(sidebar_entry(browser, "Presets"))
    assert listed(browser) == {}
    assert browser.status.text().startswith("No presets yet")


def test_save_as_default_preset(window):
    """Right-click a device › Save as Default Preset: new ones start like it (from
    the browser, too), until Clear Default Preset."""
    editor, project = window.editor, window.project
    track, (utility,) = shown_track(window)
    editor.set_device_param(track.id, utility.id, "gain", -5.0)
    widget = window.devices.widgets[utility.id]
    assert widget.save_as_default() is not None
    assert "New Utility devices will start like this one" in window.statusBar().currentMessage()
    window.add_device_to_selected_track("utility")
    assert project.track(track.id).devices[-1].params["gain"] == -5.0
    assert sidebar_entry(window.browser, "Presets").childCount() == 0  # (defaults aren't listed)
    window.devices.widgets[project.track(track.id).devices[-1].id].clear_default()
    window.add_device_to_selected_track("utility")
    assert project.track(track.id).devices[-1].params["gain"] == 0.0


def test_a_rack_is_titled_as_its_preset(window, monkeypatch):
    track, (_utility,) = shown_track(window)
    devices = window.devices
    devices.select_device(_utility.id)
    assert devices.group_selected()
    rack = next(w for w in devices.widgets.values() if isinstance(w, RackWidget))
    assert rack.title.text() == "Audio Effect Rack"
    monkeypatch.setattr(QInputDialog, "getText", lambda *args, **kwargs: ("Glue", True))
    rack.save.click()
    rack = devices.widgets[rack.device_id]
    assert rack.title.text() == "Glue" and window.undo_stack.undoText() == "Save Preset Glue"
    # Loaded elsewhere, it is titled as the preset too.
    path = Path(window.browser.preset_index.root) / "Audio Effect Rack" / f"Glue{PRESET_EXTENSION}"
    assert devices.insert_preset(str(path))
    new = window.project.track(track.id).devices[-1]
    assert devices.widgets[new.id].title.text() == "Glue"


def test_the_preset_index_sees_the_library_made(app, tmp_path):
    """No library yet: the nearest folder above it is watched, so the library made
    (outside the app) is listed."""
    root = tmp_path / "SUBstation" / "Presets"
    index = PresetIndex(root=root)
    assert index.items == [] and index._watcher.directories()
    (root / "Utility").mkdir(parents=True)
    save_preset(Device(id="u", kind="utility"), root / "Utility" / f"Warm{PRESET_EXTENSION}")
    deadline = time.monotonic() + 5
    while not index.items and time.monotonic() < deadline:
        app.processEvents()
        time.sleep(0.02)
    assert [item.name for item in index.items] == ["Warm"]
