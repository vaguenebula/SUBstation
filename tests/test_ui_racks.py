"""Racks in the device view: Ctrl+G groups the selected devices into a rack
(Ctrl+Shift+G ungroups it), which shows its macros and chains; the chain
clicked shows its devices beside the rack, where devices are dropped and
selected as on the track's own chain; chain mixers and macros edit the model;
mapping a parameter to a macro from its menu; the view's height stays put."""

from PySide6.QtCore import QMimeData, QPoint, QPointF, Qt
from PySide6.QtGui import QDropEvent
from PySide6.QtTest import QTest

from substation.model import automation
from substation.model.project import macro_param
from substation.ui.arrangement.lanes_canvas import DEVICE_MOVE_MIME
from substation.ui.browser.browser_models import DEVICE_MIME
from substation.ui.device_panel import RackWidget


def drop(pos: QPoint, mime) -> QDropEvent:
    return QDropEvent(QPointF(pos), Qt.DropAction.CopyAction, mime, Qt.MouseButton.LeftButton,
                      Qt.KeyboardModifier.NoModifier)


def center(panel, widget) -> QPoint:
    return widget.mapTo(panel, widget.rect().center())


def shown_track(window, devices=2):
    window.insert_track()
    track = window.project.tracks[0]
    window.selection.select_track(track.id)
    utilities = [window.editor.add_device(track.id, "utility") for _ in range(devices)]
    return track, utilities


def test_grouping_and_ungrouping_in_the_device_view(window, app):
    panel = window.devices
    height = panel.height()
    track, (a, b) = shown_track(window)
    panel.select_device(a.id)
    panel.select_device(b.id, Qt.KeyboardModifier.ShiftModifier)
    assert window.selection.focus == "devices"
    window.group_selected_tracks()  # Ctrl+G, in the device view
    devices = window.project.track(track.id).devices
    assert len(devices) == 1 and devices[0].is_rack
    rack = devices[0]
    widget = panel.widgets[rack.id]
    assert isinstance(widget, RackWidget) and panel.selected == [rack.id]
    assert widget.title.text() == "Audio Effect Rack"
    assert widget.chains.chain_ids() == [rack.chains[0].id]
    # Its chain shows beside it, with its devices, which select as any.
    assert set(panel.widgets) == {rack.id, a.id, b.id}
    assert rack.chains[0].id in panel._chain_views
    panel.select_device(b.id)
    assert panel.selected == [b.id]
    panel.select_device(rack.id, Qt.KeyboardModifier.ControlModifier)  # another chain's: a selection of its own
    assert panel.selected == [rack.id]
    app.processEvents()
    assert panel.height() == height  # nested devices fit as the others do
    window.ungroup_selected_tracks()  # Ctrl+Shift+G
    assert [d.id for d in window.project.track(track.id).devices] == [a.id, b.id]
    assert not any(isinstance(w, RackWidget) for w in panel.widgets.values())


def test_chains_in_the_device_view(window, app):
    panel = window.devices
    track, (a, b) = shown_track(window)
    rack = window.editor.group_devices(track.id, [a.id])
    widget = panel.widgets[rack.id]
    widget.chains.add.click()  # + Chain
    rack = window.project.device(track.id, rack.id)
    first, second = rack.chains
    widget = panel.widgets[rack.id]
    assert widget.chains.chain_ids() == [first.id, second.id]
    assert first.id in panel._chain_views and second.id not in panel._chain_views  # the first one shows
    QTest.mouseClick(widget.chains.rows[second.id], Qt.MouseButton.LeftButton)  # shows the other one
    widget = panel.widgets[rack.id]
    assert second.id in panel._chain_views and first.id not in panel._chain_views
    assert widget.chains.rows[second.id].selected
    # Dropped into the chain shown: into it (an empty chain shows its hint).
    app.processEvents()
    view = panel._chain_views[second.id]
    assert panel.drop_target(center(panel, view)) == (second.id, 0)
    mime = QMimeData()
    mime.setData(DEVICE_MIME, b'["utility"]')
    panel.dropEvent(drop(center(panel, view), mime))
    added = window.project.chain(track.id, second.id).devices
    assert len(added) == 1 and added[0].id in panel.widgets
    # Dragged from the track's chain onto a chain's row: into it, last.
    app.processEvents()
    row = panel.widgets[rack.id].chains.rows[first.id]
    assert panel.drop_target(center(panel, row)) == (first.id, 1)
    moving = QMimeData()
    moving.setData(DEVICE_MOVE_MIME, f"{track.id}\n{b.id}".encode())
    panel.dropEvent(drop(center(panel, row), moving))
    assert [d.id for d in window.project.chain(track.id, first.id).devices] == [a.id, b.id]
    assert [d.id for d in window.project.track(track.id).devices] == [rack.id]
    # Its mixer.
    row = panel.widgets[rack.id].chains.rows[second.id]
    row.solo.click()
    row.activator.click()
    chain = window.project.chain(track.id, second.id)
    assert chain.solo and chain.mute
    row = panel.widgets[rack.id].chains.rows[second.id]
    assert row.solo.isChecked() and not row.activator.isChecked()
    row.volume.valueChanged.emit(-6.0, None)
    assert window.project.chain(track.id, second.id).volume_db == -6.0
    window.editor.rename_chain(track.id, second.id, "Wet")
    assert panel.widgets[rack.id].chains.rows[second.id].name.toolTip() == "Wet"
    window.bridge.chain_meters[second.id] = (0.5, 0.5)
    panel._refresh_displays()  # (no error: its meter falls from there)


def test_macros_in_the_device_view(window):
    panel = window.devices
    track, (a, _b) = shown_track(window)
    rack = window.editor.group_devices(track.id, [a.id])
    window.editor.map_macro(track.id, rack.id, 0, a.id, "gain")
    widget = panel.widgets[rack.id]
    assert "Utility: Gain" in widget.macros.knobs[0].toolTip()
    widget.macros.knobs[0].valueChanged.emit(1.0, None)
    assert window.project.device(track.id, rack.id).params[macro_param(0)] == 1.0
    assert window.project.device(track.id, a.id).params["gain"] == 24.0
    assert panel.widgets[rack.id].macros.knobs[0].value() == 1.0
    assert panel.widgets[a.id].enclosing_rack() == rack.id
    window.undo_stack.undo()
    assert window.project.device(track.id, a.id).params["gain"] == 0.0


def test_chain_faders_are_automation_targets(window):
    track, (a, _b) = shown_track(window)
    rack = window.editor.group_devices(track.id, [a.id])
    chain = rack.chains[0]
    groups = {group_id: [s.key for s in specs] for group_id, _, specs in window.bridge.param_groups(track.id)}
    assert groups[rack.id] == [automation.chain_key(rack.id, chain.id, automation.CHAIN_VOLUME),
                               automation.chain_key(rack.id, chain.id, automation.CHAIN_PAN)]
    assert automation.device_key(a.id, "gain") in groups[a.id]  # the devices in it too
