"""Automation in the application, driven through the real main window offscreen:
showing lanes, editing envelopes with the mouse, and what the engine plays."""

import numpy as np
import pytest
from PySide6.QtCore import QPoint, QPointF, Qt
from PySide6.QtGui import QKeySequence, QMouseEvent
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication

from gilstudio.model import automation
from gilstudio.model.automation import MASTER, MIXER_PAN, MIXER_VOLUME, AutomationPoint

from .conftest import SAMPLE_RATE
from .test_ui_smoke import drag, wait_until, write_wav

SPB = SAMPLE_RATE // 2  # samples per beat at 120 BPM
ALT = Qt.KeyboardModifier.AltModifier


def env(*points):
    return tuple(AutomationPoint(*p) for p in points)


@pytest.fixture
def tracks(window):
    """Two MIDI tracks; the view zoomed so a beat is 40 px."""
    a = window.editor.add_midi_track()
    b = window.editor.add_midi_track()
    window.arrangement.view.px_per_beat = 40.0
    window.arrangement.view.changed.emit()
    QApplication.processEvents()
    return a, b


def area(window, owner, key=None):
    host = window.arrangement.master_lane if owner == MASTER else window.arrangement.lanes
    return next(a for a in host.envelope_areas() if a.owner == owner and (key is None or a.key == key))


def point(window, owner, beat, value, key=None) -> QPoint:
    lane = area(window, owner, key)
    return QPoint(round(window.arrangement.view.beat_to_x(beat)), round(lane.y(value)))


def press_move_release(widget, start: QPoint, end: QPoint, modifiers) -> None:
    """A drag holding `modifiers` throughout (QTest.mouseMove can't send them)."""
    QTest.mousePress(widget, Qt.MouseButton.LeftButton, modifiers, start)
    for at in (start + (end - start) / 2, end):
        QApplication.sendEvent(widget, QMouseEvent(QMouseEvent.Type.MouseMove, QPointF(at),
                                                   QPointF(widget.mapToGlobal(at)), Qt.MouseButton.NoButton,
                                                   Qt.MouseButton.LeftButton, modifiers))
    QTest.mouseRelease(widget, Qt.MouseButton.LeftButton, modifiers, end)


def test_a_shows_and_hides_every_lane(window, tracks):
    a, b = tracks
    [action] = [act for act in window.findChildren(type(window.re_enable_action))
                if act.shortcut() == QKeySequence("A")]
    assert not window.arrangement.layout_model.rows[0].automation
    action.trigger()
    rows = window.arrangement.layout_model.rows
    assert all(r.automation for r in rows) and window.project.master_automation_view.shown
    assert [(x.owner, x.key) for x in window.arrangement.lanes.envelope_areas()] == [(a.id, MIXER_VOLUME),
                                                                                     (b.id, MIXER_VOLUME)]
    assert window.arrangement.master_lane.height() > 40  # room for its lane and choosers
    assert window.arrangement.headers.headers[a.id].automation.main.param.text() == "Track Volume"
    action.trigger()
    assert not any(r.automation for r in window.arrangement.layout_model.rows)
    assert window.arrangement.lanes.envelope_areas() == []


def test_changing_a_parameter_shows_its_lane(window, tracks):
    a, _ = tracks
    window.selection.select_track(a.id)
    utility = window.editor.add_device(a.id, "utility")
    knob = window.devices.widgets[utility.id].knobs["gain"][0]
    drag(knob, QPoint(17, 17), QPoint(17, 5))
    key = automation.device_key(utility.id, "gain")
    assert window.project.track(a.id).automation_view.shown
    assert window.project.track(a.id).automation_view.key == key
    assert window.arrangement.headers.headers[a.id].automation.main.device.text() == "Utility"
    window.editor.set_track_param(a.id, "pan", 0.5)  # the header's controls too
    assert window.project.track(a.id).automation_view.key == MIXER_PAN
    window.editor.set_master_volume(-3.0)
    assert window.project.master_automation_view == automation.AutomationView(True, MIXER_VOLUME)


def click(widget, at: QPoint, modifiers=Qt.KeyboardModifier.NoModifier) -> None:
    QTest.mouseClick(widget, Qt.MouseButton.LeftButton, modifiers, at)


def test_clicking_a_parameter_shows_its_lane(window, tracks):
    a, _ = tracks
    window.selection.select_track(a.id)
    synth = window.project.track(a.id).devices[0]
    widget = window.devices.widgets[synth.id]
    knob = widget.knobs["attack"][0]
    QTest.mousePress(knob, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, QPoint(17, 17))
    QTest.mouseRelease(knob, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, QPoint(17, 17))
    view = window.project.track(a.id).automation_view
    assert view.shown and view.key == automation.device_key(synth.id, "attack")
    assert window.project.track(a.id).devices[0].params == synth.params  # nothing changed
    assert window.undo_stack.count() == 2  # (the two tracks)
    choice = widget.choices["wave"]
    QTest.mousePress(choice, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, QPoint(5, 5))
    choice.hidePopup()  # (the press opened its list)
    assert window.project.track(a.id).automation_view.key == automation.device_key(synth.id, "wave")
    # A control taken hold of in a plug-in's own editor (VST3 beginEdit) does the same.
    window.bridge.plugin_param_touched.emit(a.id, synth.id, "cutoff")
    assert window.project.track(a.id).automation_view.key == automation.device_key(synth.id, "cutoff")


def test_click_on_the_line_adds_breakpoints_and_dragging_moves_them(window, tracks):
    a, _ = tracks
    window.editor.show_automation(a.id, MIXER_PAN)
    lanes = window.arrangement.lanes
    click(lanes, point(window, a.id, 2.1, 0.9))  # off the line (centred pan: 0.5)
    assert window.project.envelope(a.id, MIXER_PAN) == ()
    # Over the line, the cursor says a click adds a breakpoint, and where it would go shows.
    lanes._update_cursor(QPointF(point(window, a.id, 2.1, 0.5)), Qt.KeyboardModifier.NoModifier)
    hover = lanes._hover_point
    assert hover.index is None and hover.beat == pytest.approx(2.0) and hover.value == pytest.approx(0.5)
    assert lanes.cursor().shape() == Qt.CursorShape.BitmapCursor
    click(lanes, point(window, a.id, 2.1, 0.5))  # snaps to the grid (a 16th here)
    [added] = window.project.envelope(a.id, MIXER_PAN)
    assert added.beat == pytest.approx(2.0) and added.value == pytest.approx(0.5)
    assert window.selection.points == (a.id, MIXER_PAN, frozenset({0}))
    click(lanes, point(window, a.id, 6.0, 0.75))  # off the line again
    assert len(window.project.envelope(a.id, MIXER_PAN)) == 1
    click(lanes, point(window, a.id, 6.0, 0.5))
    assert [p.beat for p in window.project.envelope(a.id, MIXER_PAN)] == [2.0, 6.0]
    # Drag the first one right and up: it stops at its neighbour, and it is one undo step.
    steps = window.undo_stack.count()
    drag(lanes, point(window, a.id, 2.0, added.value), point(window, a.id, 9.0, 1.0))
    moved = window.project.envelope(a.id, MIXER_PAN)[0]
    assert moved.beat == pytest.approx(6.0) and moved.value == pytest.approx(1.0, abs=0.05)
    assert window.undo_stack.count() == steps + 1
    window.undo_stack.undo()
    assert window.project.envelope(a.id, MIXER_PAN)[0] == added


def test_pressing_on_the_line_adds_a_breakpoint_that_a_drag_places(window, tracks):
    a, _ = tracks
    window.editor.show_automation(a.id, MIXER_PAN)
    lanes = window.arrangement.lanes
    steps = window.undo_stack.count()
    drag(lanes, point(window, a.id, 2.0, 0.5), point(window, a.id, 3.0, 0.9))
    [placed] = window.project.envelope(a.id, MIXER_PAN)
    assert placed.beat == pytest.approx(3.0) and placed.value == pytest.approx(0.9, abs=0.05)
    assert window.selection.points == (a.id, MIXER_PAN, frozenset({0}))
    assert window.undo_stack.count() == steps + 1  # adding and placing: one step
    window.undo_stack.undo()
    assert window.project.envelope(a.id, MIXER_PAN) == ()


def test_lanes_show_parameters_changed_by_hand(window, tracks, monkeypatch):
    a, b = tracks
    window.editor.show_automation(a.id)
    synth = window.project.track(a.id).devices[0]
    lanes = window.arrangement.lanes
    repaints = []
    monkeypatch.setattr(lanes, "update", lambda *args: repaints.append(args))
    window.editor.set_device_param(a.id, synth.id, "attack", 50.0)
    assert repaints
    repaints.clear()
    window.bridge.plugin_params_changed.emit(b.id, "x")  # (a plug-in's values) on a track not showing any
    assert not repaints


def test_a_new_breakpoint_goes_on_a_sloped_line(window, tracks):
    a, _ = tracks
    window.editor.show_automation(a.id, MIXER_PAN)
    window.editor.set_envelope(a.id, MIXER_PAN, env((0.0, 0.0), (8.0, 1.0)))
    lanes = window.arrangement.lanes
    click(lanes, point(window, a.id, 4.0, 0.2))  # well below the line
    assert len(window.project.envelope(a.id, MIXER_PAN)) == 2
    click(lanes, point(window, a.id, 4.0, 0.5) + QPoint(0, 2))  # a little off it counts
    points = window.project.envelope(a.id, MIXER_PAN)
    assert [p.beat for p in points] == [0.0, 4.0, 8.0] and points[1].value == pytest.approx(0.5)


def test_alt_drag_bends_a_segment(window, tracks):
    a, _ = tracks
    window.editor.show_automation(a.id, MIXER_PAN)
    window.editor.set_envelope(a.id, MIXER_PAN, env((0.0, 0.0), (8.0, 1.0)))
    lanes = window.arrangement.lanes
    start = point(window, a.id, 4.0, 0.5)
    press_move_release(lanes, start, start - QPoint(0, 60), ALT)
    points = window.project.envelope(a.id, MIXER_PAN)
    assert points[0].curve > 0.2 and len(points) == 2  # bent, not a new point
    assert automation.value_at(points, 4.0) > 0.5  # bulging upward
    window.undo_stack.undo()
    assert window.project.envelope(a.id, MIXER_PAN)[0].curve == 0.0


def test_deleting_breakpoints(window, tracks):
    a, _ = tracks
    window.editor.show_automation(a.id, MIXER_PAN)
    window.editor.set_envelope(a.id, MIXER_PAN, env((1.0, 0.2), (3.0, 0.8), (5.0, 0.4), (7.0, 0.6)))
    lanes = window.arrangement.lanes
    click(lanes, point(window, a.id, 3.0, 0.8))  # a click deletes a breakpoint
    assert [p.beat for p in window.project.envelope(a.id, MIXER_PAN)] == [1.0, 5.0, 7.0]
    window.undo_stack.undo()
    # Each click of a double-click counts: the first deletes, the second (off the line now) does nothing.
    at = point(window, a.id, 3.0, 0.8)
    for kind in (QMouseEvent.Type.MouseButtonPress, QMouseEvent.Type.MouseButtonRelease,
                 QMouseEvent.Type.MouseButtonDblClick, QMouseEvent.Type.MouseButtonRelease):
        buttons = Qt.MouseButton.NoButton if kind == QMouseEvent.Type.MouseButtonRelease else Qt.MouseButton.LeftButton
        QApplication.sendEvent(lanes, QMouseEvent(kind, QPointF(at), QPointF(lanes.mapToGlobal(at)),
                                                  Qt.MouseButton.LeftButton, buttons, Qt.KeyboardModifier.NoModifier))
    assert [p.beat for p in window.project.envelope(a.id, MIXER_PAN)] == [1.0, 5.0, 7.0]
    # Shift- (or Ctrl-) clicks select instead; Delete deletes the selected ones.
    click(lanes, point(window, a.id, 1.0, 0.2), Qt.KeyboardModifier.ShiftModifier)
    click(lanes, point(window, a.id, 7.0, 0.6), Qt.KeyboardModifier.ShiftModifier)
    assert window.selection.points == (a.id, MIXER_PAN, frozenset({0, 2}))
    assert len(window.project.envelope(a.id, MIXER_PAN)) == 3
    window.delete_selection()
    assert [p.beat for p in window.project.envelope(a.id, MIXER_PAN)] == [5.0]


def test_a_time_selection_in_a_lane_clears_and_duplicates(window, tracks):
    a, _ = tracks
    window.editor.show_automation(a.id, MIXER_PAN)
    window.editor.set_envelope(a.id, MIXER_PAN, env((0.0, 0.0), (2.0, 1.0), (4.0, 0.0)))
    lanes = window.arrangement.lanes
    drag(lanes, point(window, a.id, 1.0, 0.9), point(window, a.id, 3.0, 0.9))
    assert window.selection.time_range == (1.0, 3.0, (a.id,))
    assert window.selection.lanes == ((a.id, MIXER_PAN),)
    window.duplicate()  # Ctrl+D: after the range, over what was there
    assert window.selection.time_range[:2] == (3.0, 5.0)
    points = window.project.envelope(a.id, MIXER_PAN)
    assert automation.value_at(points, 4.0) == pytest.approx(automation.value_at(points, 2.0))
    window.undo_stack.undo()
    window.selection.set_time_range(1.0, 3.0, [a.id], lanes=[(a.id, MIXER_PAN)])
    window.delete_selection()
    points = window.project.envelope(a.id, MIXER_PAN)
    assert automation.value_at(points, 2.0) == pytest.approx(0.5)  # straight across the range
    assert automation.value_at(points, 0.5) == pytest.approx(0.25)  # the rest as it was


def test_automation_plays_until_changed_by_hand(window, tracks, tmp_path):
    path = str(write_wav(tmp_path / "t.wav", np.full((4 * SAMPLE_RATE, 2), 0.5)))
    track_id = window.editor.add_clips(None, 0.0, [(path, 4.0)])[0][0]
    assert wait_until(lambda: window.bridge.source(path) is not None)
    silent_then_loud = env((0.0, 0.0), (1.0, 0.0), (1.0, automation.volume_to_normalized(0.0)))
    window.editor.set_envelope(track_id, MIXER_VOLUME, silent_then_loud)
    out = window.engine.render_offline(0.0, 2 * SPB)
    assert np.abs(out[: SPB - 10]).max() == 0.0 and out[SPB + 100, 0] == pytest.approx(0.5, rel=1e-3)
    assert window.bridge.is_automated(track_id, MIXER_VOLUME)
    assert not window.transport.re_enable.isEnabled()
    # Setting the volume by hand overrides the automation...
    window.editor.set_track_param(track_id, "volume_db", -6.0)
    assert window.bridge.is_overridden(track_id, MIXER_VOLUME) and window.transport.re_enable.isEnabled()
    assert window.arrangement.headers.headers[track_id].volume.automation() == "off"
    out = window.engine.render_offline(0.0, 2 * SPB)
    assert out[1000, 0] == pytest.approx(0.5 * 10 ** (-6 / 20), rel=1e-3)  # (after the clip's fade-in)
    # ... until it is re-enabled.
    window.transport.re_enable.click()
    assert not window.bridge.has_overrides and not window.transport.re_enable.isEnabled()
    assert np.abs(window.engine.render_offline(0.0, SPB - 10)).max() == 0.0
    # Its envelope deleted, the track plays at its own volume again.
    window.editor.clear_envelope(track_id, MIXER_VOLUME)
    assert window.engine.render_offline(0.0, 2000)[1000, 0] == pytest.approx(0.5 * 10 ** (-6 / 20), rel=1e-3)


def test_controls_follow_their_automation(window, tracks):
    a, _ = tracks
    window.selection.select_track(a.id)
    synth = window.project.track(a.id).devices[0]
    attack = automation.device_key(synth.id, "attack")
    window.editor.set_envelope(a.id, MIXER_VOLUME, env((0.0, 0.0), (4.0, 1.0)))
    window.editor.set_envelope(a.id, attack, env((0.0, 0.0), (4.0, 1.0)))
    window.bridge.locate(2.0)
    QApplication.processEvents()
    header = window.arrangement.headers.headers[a.id]
    assert header.volume.automation() == "on"
    assert header.volume.value() == pytest.approx(automation.normalized_to_volume(0.5), abs=0.05)
    knob = window.devices.widgets[synth.id].knobs["attack"][0]
    assert knob.automation() == "on"
    assert knob.value() == pytest.approx((1 * 5000) ** 0.5, rel=1e-3)  # halfway on a log scale
    window.bridge.locate(4.0)
    QApplication.processEvents()
    assert knob.value() == pytest.approx(5000.0, rel=1e-3)
    window.editor.clear_envelope(a.id, attack)  # back to its own value
    assert knob.automation() is None and knob.value() == pytest.approx(3.0)


def test_master_automation(window, tracks, tmp_path):
    path = str(write_wav(tmp_path / "m.wav", np.full((2 * SAMPLE_RATE, 2), 0.5)))
    window.editor.add_clips(None, 0.0, [(path, 2.0)])
    assert wait_until(lambda: window.bridge.source(path) is not None)
    window.editor.show_automation(MASTER, MIXER_PAN)
    master = window.arrangement.master_lane
    click(master, point(window, MASTER, 0.0, 0.5))  # on its line: pan centred
    drag(master, point(window, MASTER, 0.0, 0.5), point(window, MASTER, 0.0, 0.0))
    [added] = window.project.envelope(MASTER, MIXER_PAN)
    assert added.value == pytest.approx(0.0, abs=0.03)
    out = window.engine.render_offline(0.0, 1000)
    assert out[500, 0] == pytest.approx(0.5, rel=1e-3) and abs(out[500, 1]) < 0.02  # panned left
    assert window.arrangement.master_header.pan.automation() == "on"


def test_lanes_below_a_track(window, tracks):
    a, _ = tracks
    window.editor.show_automation(a.id)
    header = window.arrangement.headers.headers[a.id]
    before = window.arrangement.layout_model.rows[1].top
    header.automation.main.button.click()  # "+"
    row = window.arrangement.layout_model.rows[0]
    assert [lane.key for lane in row.lanes] == [MIXER_PAN]
    assert window.arrangement.layout_model.rows[1].top == before + row.lanes[0].height
    lane = area(window, a.id, MIXER_PAN)
    assert lane.lane == 0 and lane.rect.top() >= row.main_height - window.arrangement.view.scroll_y
    click(window.arrangement.lanes, point(window, a.id, 1.0, 0.5, MIXER_PAN))
    assert window.project.envelope(a.id, MIXER_PAN)[0].value == pytest.approx(0.5)
    assert not window.project.envelope(a.id, MIXER_VOLUME)
    header.automation._choose(header.automation.lanes[0], MIXER_VOLUME)
    assert window.project.track(a.id).automation_view.lanes == (MIXER_VOLUME,)
    header.automation.lanes[0].button.click()  # "-"
    assert window.arrangement.layout_model.rows[0].lanes == ()
    assert window.arrangement.layout_model.rows[1].top == before


def test_automation_is_saved(window, tracks, tmp_path):
    a, _ = tracks
    window.editor.set_envelope(a.id, MIXER_PAN, env((0.0, 0.1), (4.0, 0.9, 0.3)))
    window.editor.set_master_pan(0.25)
    window.editor.show_automation(a.id, MIXER_PAN)
    target = tmp_path / "auto.gilproj"
    assert window._save_to(target)
    window.new_project()
    window.open_project(str(target))
    reopened = window.project.tracks[0]
    assert reopened.automation == {MIXER_PAN: env((0.0, 0.1), (4.0, 0.9, 0.3))}
    assert reopened.automation_view.shown and window.project.master_pan == 0.25
    assert window.bridge.is_automated(reopened.id, MIXER_PAN)
    assert [(x.owner, x.key) for x in window.arrangement.lanes.envelope_areas()] == [(reopened.id, MIXER_PAN)]


def test_unlocked_automation_moves_with_a_dragged_clip(window, tracks):
    a, _ = tracks
    window.editor.add_midi_clip(a.id, 0.0, 4.0)
    window.editor.set_envelope(a.id, MIXER_PAN, env((0.0, 0.0), (4.0, 1.0)))
    lanes = window.arrangement.lanes
    row = window.arrangement.layout_model.rows[0]
    title = int(row.top - window.arrangement.view.scroll_y + 5)  # the clip's title bar

    def drag_clip(start: float, end: float) -> None:
        drag(lanes, QPoint(round(window.arrangement.view.beat_to_x(start + 0.5)), title),
             QPoint(round(window.arrangement.view.beat_to_x(end + 0.5)), title))

    drag_clip(0.0, 8.0)
    assert window.project.track(a.id).clips[0].start_beat == 8.0
    assert [(p.beat, p.value) for p in window.project.envelope(a.id, MIXER_PAN)] == [(8.0, 0.0), (12.0, 1.0)]
    window.undo_stack.undo()  # clip and automation together
    assert window.project.track(a.id).clips[0].start_beat == 0.0
    assert window.project.envelope(a.id, MIXER_PAN) == env((0.0, 0.0), (4.0, 1.0))
    # Locked (the transport's button), the automation stays.
    window.transport.lock_envelopes.click()
    assert window.project.automation_locked and window.lock_action.isChecked()
    drag_clip(0.0, 8.0)
    assert window.project.track(a.id).clips[0].start_beat == 8.0
    assert window.project.envelope(a.id, MIXER_PAN) == env((0.0, 0.0), (4.0, 1.0))


def test_lane_screenshot(window, tracks, tmp_path):
    """Everything paints (lanes, curves, steps, readouts) without errors."""
    a, b = tracks
    window.selection.select_track(a.id)
    wave = automation.device_key(window.project.track(a.id).devices[0].id, "wave")
    window.editor.set_envelope(a.id, wave, env((0.0, 0.0), (8.0, 1.0)))  # discrete: steps
    window.editor.set_envelope(b.id, MIXER_VOLUME, env((0.0, 0.2, 0.8), (8.0, 0.9)))
    window.editor.toggle_all_automation()
    window.editor.show_automation(a.id, wave)
    window.editor.add_automation_lane(b.id)
    lanes = window.arrangement.lanes
    start = point(window, b.id, 8.0, 0.9)
    QTest.mousePress(lanes, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, start)
    QTest.mouseMove(lanes, start + QPoint(0, 20))  # a readout shows while dragging
    image = window.grab()
    QTest.mouseRelease(lanes, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, start + QPoint(0, 20))
    assert not image.isNull()


def test_dragging_up_from_a_lane_into_the_clips_selects_them(window, tracks):
    """From an automation lane, up into the clips' title band (or past the top
    track) selects the clips, as a drag in a lane without automation does."""
    a, b = tracks
    clip = window.editor.add_midi_clip(a.id, 0.0, 8.0)
    window.editor.show_automation(a.id, MIXER_PAN)
    lanes = window.arrangement.lanes
    start = point(window, a.id, 1.0, 0.1)
    for end in (QPoint(round(window.arrangement.view.beat_to_x(3.0)), -10),  # past the top track
                QPoint(round(window.arrangement.view.beat_to_x(3.0)), 5)):  # its title band
        window.selection.clear()
        drag(lanes, start, end)
        assert window.selection.clips == {clip}
        assert window.selection.time_range[2] == (a.id,)
    # Kept to the lanes, it is a range on the automation.
    drag(lanes, start, point(window, a.id, 3.0, 0.1))
    assert window.selection.lanes == ((a.id, MIXER_PAN),) and not window.selection.clips


def test_dragging_near_a_segment_moves_its_two_breakpoints(window, tracks):
    a, _ = tracks
    window.editor.show_automation(a.id, MIXER_PAN)
    window.editor.set_envelope(a.id, MIXER_PAN, env((0.0, 0.2), (2.0, 0.4), (6.0, 0.4), (8.0, 0.8)))
    lanes = window.arrangement.lanes
    near = point(window, a.id, 4.0, 0.4) + QPoint(0, 10)  # below the line, not on it
    lanes._update_cursor(QPointF(near), Qt.KeyboardModifier.NoModifier)
    assert lanes._hover_point.kind == "segment" and lanes._hover_point.index == 1
    assert lanes.cursor().shape() == Qt.CursorShape.ArrowCursor  # only the segment lights up
    far = point(window, a.id, 4.0, 0.4) + QPoint(0, 30)
    lanes._update_cursor(QPointF(far), Qt.KeyboardModifier.NoModifier)
    assert lanes._hover_point is None
    # A click selects the two; a drag moves them together, as one undo step.
    click(lanes, near)
    assert window.selection.points == (a.id, MIXER_PAN, frozenset({1, 2}))
    assert len(window.project.envelope(a.id, MIXER_PAN)) == 4
    steps = window.undo_stack.count()
    height = area(window, a.id).values.height()
    drag(lanes, near, near + QPoint(0, -round(height * 0.25)))
    points = window.project.envelope(a.id, MIXER_PAN)
    assert [p.beat for p in points] == [0.0, 2.0, 6.0, 8.0]
    assert points[1].value == pytest.approx(0.65, abs=0.02) and points[2].value == pytest.approx(0.65, abs=0.02)
    assert points[0].value == 0.2 and points[3].value == 0.8
    assert window.undo_stack.count() == steps + 1


def test_dragging_a_selected_range_moves_its_automation(window, tracks):
    a, _ = tracks
    window.editor.show_automation(a.id, MIXER_PAN)
    ramp = env((0.0, 0.0), (8.0, 1.0))
    window.editor.set_envelope(a.id, MIXER_PAN, ramp)
    lanes = window.arrangement.lanes
    drag(lanes, point(window, a.id, 2.0, 0.9), point(window, a.id, 4.0, 0.9))  # select 2..4
    assert window.selection.time_range[:2] == (2.0, 4.0)
    inside = point(window, a.id, 3.0, 0.8)
    lanes._update_cursor(QPointF(inside), Qt.KeyboardModifier.NoModifier)
    assert lanes._hover_point.kind == "range" and lanes.cursor().shape() == Qt.CursorShape.ArrowCursor
    # Up: the inside moves, with steps at the edges; the outside stays.
    height = area(window, a.id).values.height()
    steps = window.undo_stack.count()
    drag(lanes, inside, inside + QPoint(0, -round(height * 0.25)))
    points = window.project.envelope(a.id, MIXER_PAN)
    assert automation.value_at(points, 3.0) == pytest.approx(0.625, abs=0.02)
    assert automation.value_at(points, 1.0) == pytest.approx(0.125)
    assert automation.value_at(points, 5.0) == pytest.approx(0.625)
    assert [p.beat for p in points].count(2.0) == 2 and [p.beat for p in points].count(4.0) == 2
    assert window.selection.time_range[:2] == (2.0, 4.0) and window.undo_stack.count() == steps + 1
    window.undo_stack.undo()
    assert window.project.envelope(a.id, MIXER_PAN) == ramp
    # Right: it lands 2 beats later (over what was there); the range goes along.
    drag(lanes, inside, point(window, a.id, 5.0, 0.8))
    points = window.project.envelope(a.id, MIXER_PAN)
    assert automation.value_at(points, 5.0) == pytest.approx(0.375)  # what was at 3
    assert automation.value_at(points, 7.0) == pytest.approx(0.875)  # the rest as it was
    assert window.selection.time_range[:2] == (4.0, 6.0)


def test_a_step_is_a_segment_to_drag(window, tracks):
    a, _ = tracks
    window.editor.show_automation(a.id, MIXER_PAN)
    window.editor.set_envelope(a.id, MIXER_PAN, env((0.0, 0.2), (4.0, 0.2), (4.0, 0.8), (8.0, 0.8)))
    lanes = window.arrangement.lanes
    for at in (point(window, a.id, 4.0, 0.5), point(window, a.id, 4.0, 0.5) + QPoint(8, 0)):  # on it, and beside
        lanes._update_cursor(QPointF(at), Qt.KeyboardModifier.NoModifier)
        assert lanes._hover_point.kind == "segment" and lanes._hover_point.index == 1
    drag(lanes, point(window, a.id, 4.0, 0.5), point(window, a.id, 6.0, 0.5))
    points = window.project.envelope(a.id, MIXER_PAN)
    assert [(p.beat, p.value) for p in points] == [(0.0, 0.2), (6.0, 0.2), (6.0, 0.8), (8.0, 0.8)]
