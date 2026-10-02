"""Return tracks and sends in the window: Ctrl+Alt+T, the returns' rows above
the master, send knobs on every header (tracks, groups, returns), their
pre/post-fader tap, send automation lanes, and deleting a return."""

from PySide6.QtCore import QPoint, Qt
from PySide6.QtTest import QTest

from gilstudio.model import automation
from gilstudio.model.project import Send
from gilstudio.model.serialization import load_into, project_to_dict


def press_key(window, key, modifiers=Qt.KeyboardModifier.NoModifier) -> None:
    QTest.keyClick(window.arrangement.lanes, key, modifiers)
    QTest.qWait(10)


def drag_knob(knob, pixels: int) -> None:
    start = QPoint(knob.width() // 2, knob.height() // 2)
    QTest.mousePress(knob, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, start)
    QTest.mouseMove(knob, start - QPoint(0, pixels // 2))
    QTest.mouseMove(knob, start - QPoint(0, pixels))
    QTest.mouseRelease(knob, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, start - QPoint(0, pixels))
    QTest.qWait(10)


def test_ctrl_alt_t_inserts_a_return_above_the_master(window):
    project, arrangement = window.project, window.arrangement
    track = window.editor.add_audio_track()
    window.activateWindow()
    press_key(window, Qt.Key.Key_T, Qt.KeyboardModifier.ControlModifier | Qt.KeyboardModifier.AltModifier)
    assert len(project.returns) == 1
    ret = project.returns[0]
    assert ret.name == "A Return" and project.tracks == [track]
    assert window.selection.track_id == ret.id
    assert window.devices.track_id == ret.id  # the device view shows its effects
    lane, header = arrangement.return_row(ret.id)
    assert header.isVisible() and lane.isVisible()
    assert header.activator.text() == "A"
    # Above the master, beside its lane.
    assert header.mapTo(window, QPoint(0, 0)).y() < arrangement.master_header.mapTo(window, QPoint(0, 0)).y()
    assert lane.height() == header.height()
    window.grab()  # the rows paint
    window.undo_stack.undo()
    QTest.qWait(10)
    assert project.returns == [] and arrangement.return_row(ret.id) is None
    assert not arrangement.return_headers.isVisible()


def test_send_knobs_on_track_headers(window):
    project, editor = window.project, window.editor
    track = editor.add_audio_track()
    group = editor.group_tracks([track.id])
    a, b = editor.add_return_track(), editor.add_return_track()
    QTest.qWait(10)
    headers = window.arrangement.headers.headers
    sends = headers[track.id].sends
    assert list(sends.knobs) == [a.id, b.id]
    label, knob = sends.knobs[a.id]
    assert label.text() == "A" and knob.isVisible() and knob.isEnabled()
    assert headers[group.id].sends.knobs[b.id][1].isVisible()  # groups send too
    assert knob.value() == 0.0  # no send yet: silent
    drag_knob(knob, 120)
    send = project.track(track.id).sends[a.id]
    assert send.level_db > automation.MIN_VOLUME_DB and not send.pre_fader
    assert knob.value() == automation.volume_to_normalized(send.level_db)
    assert window.undo_stack.undoText() == "Change Send"
    window.undo_stack.undo()  # one drag, one step
    assert project.track(track.id).sends == {}
    # Pre-fader: the letter shows in the accent colour.
    editor.set_send(track.id, a.id, -6.0, pre_fader=True)
    QTest.qWait(10)
    assert "color: #" in label.styleSheet() and sends.knobs[a.id][1].value() == automation.volume_to_normalized(-6.0)
    # The send knobs sit below volume and pan; with automation shown, the choosers below them.
    editor.show_automation(track.id)
    QTest.qWait(10)
    row = window.arrangement.layout_model.row_for(track.id)
    assert row.main_height >= 100
    window.grab()


def test_returns_send_to_returns_but_not_into_a_cycle(window):
    project, editor = window.project, window.editor
    a, b = editor.add_return_track(), editor.add_return_track()
    QTest.qWait(10)
    header_a = window.arrangement.return_row(a.id)[1]
    header_b = window.arrangement.return_row(b.id)[1]
    assert not header_a.sends.knobs[a.id][1].isEnabled()  # not into itself
    assert header_a.sends.knobs[b.id][1].isEnabled()
    drag_knob(header_a.sends.knobs[b.id][1], 120)
    assert b.id in project.track(a.id).sends
    QTest.qWait(10)
    assert not header_b.sends.knobs[a.id][1].isEnabled()  # a feeds b: b can't send into a


def test_send_automation_lanes(window):
    editor, bridge = window.editor, window.bridge
    track = editor.add_audio_track()
    ret = editor.add_return_track()
    key = automation.send_key(ret.id)
    mixer = next(specs for group, _name, specs in bridge.param_groups(track.id) if group == "mixer")
    assert [s.name for s in mixer] == ["Track Volume", "Track Pan", "Send A"]
    assert bridge.param_spec(track.id, key).name == "Send A"
    assert [s.name for s in next(specs for g, _n, specs in bridge.param_groups(ret.id) if g == "mixer")] == [
        "Track Volume", "Track Pan"]  # a return can't send to itself
    editor.show_automation(track.id, key)
    editor.add_automation_point(track.id, key, 0.0, 0.5)
    QTest.qWait(10)
    assert bridge.is_automated(track.id, key)
    knob = window.arrangement.headers.headers[track.id].sends.knobs[ret.id][1]
    assert knob.automation() == "on"
    # Turning the knob by hand overrides the automation, as for volume.
    drag_knob(knob, 60)
    assert bridge.is_overridden(track.id, key)
    window.grab()


def test_deleting_a_return_with_delete(window):
    project, editor = window.project, window.editor
    track = editor.add_audio_track()
    ret = editor.add_return_track()
    editor.set_send(track.id, ret.id, -3.0)
    QTest.qWait(10)
    header = window.arrangement.return_row(ret.id)[1]
    QTest.mouseClick(header, Qt.MouseButton.LeftButton, pos=QPoint(40, 10))
    assert window.selection.track_id == ret.id
    window.delete_selection()
    QTest.qWait(10)
    assert project.returns == [] and project.track(track.id).sends == {}
    assert window.arrangement.headers.headers[track.id].sends.knobs == {}
    window.undo_stack.undo()
    QTest.qWait(10)
    assert [r.id for r in project.returns] == [ret.id]
    assert project.track(track.id).sends == {ret.id: Send(-3.0)}


def test_a_loaded_project_shows_its_returns(window):
    project, editor = window.project, window.editor
    track = editor.add_audio_track()
    ret = editor.add_return_track()
    editor.set_send(track.id, ret.id, -3.0)
    data = project_to_dict(project)
    load_into(project, {**data, "returns": []})
    QTest.qWait(10)
    assert window.arrangement.return_row(ret.id) is None
    load_into(project, data)
    QTest.qWait(10)
    assert window.arrangement.return_row(ret.id) is not None
    assert list(window.arrangement.headers.headers[track.id].sends.knobs) == [ret.id]
