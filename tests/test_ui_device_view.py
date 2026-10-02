"""Folding devices, and cutting, copying and pasting them: in the model (copies
are new devices, sidechains kept where they can be, one undo step; folding is
saved view state, not undone) and in the device view (the fold button, a
folded rack hiding its chain, Ctrl+C/X/V/D while it has the focus)."""

import pytest
from PySide6.QtCore import QPoint, Qt
from PySide6.QtGui import QUndoStack
from PySide6.QtTest import QTest

from gilstudio.model.editor import ProjectEditor, device_is_instrument
from gilstudio.model.project import MacroMapping, Project, Sidechain, iter_devices
from gilstudio.model.serialization import load_into, project_to_dict
from gilstudio.ui.device_panel import FOLDED_WIDTH, RackWidget


@pytest.fixture
def editor(app):
    return ProjectEditor(Project(), QUndoStack())


def ids(devices):
    return [d.id for d in devices]


# --- The model -------------------------------------------------------------------------------


def test_pasted_devices_are_new_devices_like_the_copied_ones(editor):
    p = editor.project
    source, track, other = editor.add_audio_track(), editor.add_audio_track(), editor.add_audio_track()
    a, b, c = (editor.add_device(track.id, "utility") for _ in range(3))
    rack = editor.group_devices(track.id, [b.id, c.id])
    editor.map_macro(track.id, rack.id, 0, c.id, "gain")
    editor.set_device_param(track.id, a.id, "gain", 0.25)
    editor.set_device_sidechain(track.id, a.id, Sidechain(source.id))
    # A device in a selected rack goes along with the rack.
    copied = editor.copy_devices(track.id, [c.id, a.id, rack.id])
    assert ids(copied) == [a.id, rack.id]
    editor.set_device_param(track.id, a.id, "gain", 0.5)  # (after copying: the copy is as it was)

    existing = editor.add_device(other.id, "utility")
    steps = editor.undo_stack.count()
    pasted = editor.paste_devices(other.id, copied, index=0)
    assert editor.undo_stack.count() == steps + 1 and editor.undo_stack.undoText() == "Paste Devices"
    devices = p.track(other.id).devices
    assert ids(devices) == [*ids(pasted), existing.id]
    old = {d.id for d in iter_devices(copied)}
    assert not old & {d.id for d in iter_devices(pasted)}  # new devices, chains too
    assert not {ch.id for ch in rack.chains} & {ch.id for ch in pasted[1].chains}
    assert devices[0].params["gain"] == 0.25 and devices[0].sidechain == Sidechain(source.id)
    inner = pasted[1].chains[0].devices
    assert pasted[1].macros == (MacroMapping(0, inner[1].id, "gain", 0.0, 1.0),)  # the macro follows its device
    # Pasted onto the sidechain's own source, the sidechain would close a cycle: it goes.
    editor.paste_devices(source.id, copied)
    assert p.track(source.id).devices[0].sidechain is None
    editor.undo_stack.undo()
    editor.undo_stack.undo()
    assert ids(p.track(other.id).devices) == [existing.id]


def test_pasted_instruments_go_where_instruments_go(editor):
    p = editor.project
    midi = editor.add_midi_track()  # with its synth
    synth = p.track(midi.id).devices[0]
    effect = editor.add_device(midi.id, "utility")
    copied = editor.copy_devices(midi.id, [synth.id, effect.id])
    audio = editor.add_audio_track()
    pasted = editor.paste_devices(audio.id, copied)  # no instrument on an audio track
    assert ids(pasted) == [p.track(audio.id).devices[0].id] and not device_is_instrument(pasted[0])
    other = editor.add_midi_track()
    pasted = editor.paste_devices(other.id, copied)  # the instrument replaces the one there, first
    devices = p.track(other.id).devices
    assert ids(devices) == ids(pasted) and device_is_instrument(devices[0]) and len(devices) == 2


def test_folding_devices_is_saved_but_not_undone(editor):
    p = editor.project
    track = editor.add_audio_track()
    a, b = editor.add_device(track.id, "utility"), editor.add_device(track.id, "utility")
    steps = editor.undo_stack.count()
    folded = []
    p.devices_folded.connect(folded.append)
    editor.set_devices_folded(track.id, [a.id, b.id], True)
    editor.set_devices_folded(track.id, [b.id], False)
    assert p.is_device_folded(a.id) and not p.is_device_folded(b.id)
    assert folded == [track.id, track.id] and editor.undo_stack.count() == steps
    editor.undo_stack.undo()  # (b's adding): a stays folded
    assert p.is_device_folded(a.id)
    # Pasted, a folded device's copy is folded too.
    [copy] = editor.paste_devices(track.id, editor.copy_devices(track.id, [a.id]), folded={a.id})
    assert p.is_device_folded(copy.id)
    data = project_to_dict(p)
    assert data["folded_devices"] == sorted([a.id, copy.id])  # b isn't there any more
    loaded = Project()
    load_into(loaded, data)
    assert loaded.folded_devices == {a.id, copy.id}
    del data["folded_devices"]  # older files: nothing folded
    load_into(loaded, data)
    assert not loaded.folded_devices


# --- The device view -------------------------------------------------------------------------


def shown_track(window, devices=3):
    window.insert_track()
    track = window.project.tracks[-1]
    window.selection.select_track(track.id)
    return track, [window.editor.add_device(track.id, "utility") for _ in range(devices)]


def test_folding_devices_in_the_device_view(window):
    panel = window.devices
    track, (a, b, c) = shown_track(window)
    width = panel.widgets[a.id].width()
    panel.widgets[a.id].fold_button.click()
    widget = panel.widgets[a.id]
    assert window.project.is_device_folded(a.id) and widget.folded
    assert widget.width() == FOLDED_WIDTH < width and widget.folded_bar.isVisibleTo(widget)
    assert not widget.header_bar.isVisibleTo(widget) and not widget.body_widget.isVisibleTo(widget)
    QTest.mouseDClick(widget, Qt.MouseButton.LeftButton, pos=QPoint(FOLDED_WIDTH // 2, 60))  # unfolds it
    assert not window.project.is_device_folded(a.id) and not panel.widgets[a.id].folded
    title = panel.widgets[a.id].title
    QTest.mouseDClick(title, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.ControlModifier,
                      title.rect().center())  # Ctrl+double-click folds it...
    assert window.project.is_device_folded(a.id)
    QTest.mouseDClick(panel.widgets[a.id], Qt.MouseButton.LeftButton, Qt.KeyboardModifier.ControlModifier,
                      QPoint(FOLDED_WIDTH // 2, 60))  # ...and unfolds it
    assert not window.project.is_device_folded(a.id)
    panel.select_device(c.id)
    # With several selected, folding one of them folds them all.
    panel.select_device(a.id)
    panel.select_device(b.id, Qt.KeyboardModifier.ControlModifier)
    panel.widgets[b.id].fold_button.click()
    assert [window.project.is_device_folded(d.id) for d in (a, b, c)] == [True, True, False]
    assert panel.selected == [a.id, b.id]  # still selected
    # A folded rack hides its chain (and what is in it).
    rack = window.editor.group_devices(track.id, [c.id])
    assert c.id in panel.widgets and rack.chains[0].id in panel._chain_views
    panel.widgets[rack.id].toggle_fold()
    assert isinstance(panel.widgets[rack.id], RackWidget) and panel.widgets[rack.id].folded
    assert c.id not in panel.widgets and not panel._chain_views
    panel.widgets[rack.id].toggle_fold()
    assert c.id in panel.widgets


def test_cut_copy_paste_and_duplicate_devices(window):
    panel, project = window.devices, window.project
    track, (a, b, c) = shown_track(window)
    window.editor.set_device_param(track.id, a.id, "gain", 0.3)
    panel.select_device(a.id)
    assert window.selection.focus == "devices"
    window.copy()  # Ctrl+C
    panel.select_device(b.id)
    window.paste()  # Ctrl+V: after the selected device, and selected
    devices = project.track(track.id).devices
    assert len(devices) == 4 and ids(devices)[:2] == [a.id, b.id] and ids(devices)[3] == c.id
    pasted = devices[2]
    assert pasted.id not in (a.id, b.id, c.id) and pasted.params["gain"] == 0.3
    assert panel.selected == [pasted.id]
    window.cut()  # Ctrl+X: the pasted one goes (to the clipboard)
    assert ids(project.track(track.id).devices) == [a.id, b.id, c.id]
    window.undo_stack.undo()
    assert len(project.track(track.id).devices) == 4
    window.undo_stack.undo()
    assert ids(project.track(track.id).devices) == [a.id, b.id, c.id]
    panel.select_device(b.id)
    window.duplicate()  # Ctrl+D: a copy right after it; the clipboard keeps what was cut
    devices = project.track(track.id).devices
    assert len(devices) == 4 and ids(devices)[1] == b.id and panel.selected == [devices[2].id]
    assert len(panel.clipboard) == 1 and panel.clipboard[0].params["gain"] == 0.3

    # Onto another track: click beside its devices (the device view takes the focus), then paste.
    other, _ = shown_track(window, devices=0)
    window.selection.select_track(other.id, focus_track=True)
    assert window.selection.focus == "track"
    QTest.mouseClick(panel, Qt.MouseButton.LeftButton, pos=QPoint(panel.width() - 5, panel.height() // 2))
    assert window.selection.focus == "devices"
    window.paste()
    [copy] = project.track(other.id).devices
    assert copy.params["gain"] == 0.3 and panel.selected == [copy.id]
    clips = window.arrangement.lanes.clipboard
    assert clips is None  # (the clips' clipboard is another)
