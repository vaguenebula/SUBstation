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


def test_click_adds_breakpoints_and_dragging_moves_them(window, tracks):
    a, _ = tracks
    window.editor.show_automation(a.id, MIXER_PAN)
    lanes = window.arrangement.lanes
    QTest.mouseClick(lanes, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier,
                     point(window, a.id, 2.1, 0.25))  # snaps to the grid (a 16th here)
    [added] = window.project.envelope(a.id, MIXER_PAN)
    assert added.beat == pytest.approx(2.0) and added.value == pytest.approx(0.25, abs=0.03)
    assert window.selection.points == (a.id, MIXER_PAN, frozenset({0}))
    QTest.mouseClick(lanes, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, point(window, a.id, 6.0, 0.75))
    assert [p.beat for p in window.project.envelope(a.id, MIXER_PAN)] == [2.0, 6.0]
    # Drag the first one right and up: it stops at its neighbour, and it is one undo step.
    steps = window.undo_stack.count()
    drag(lanes, point(window, a.id, 2.0, added.value), point(window, a.id, 9.0, 1.0))
    moved = window.project.envelope(a.id, MIXER_PAN)[0]
    assert moved.beat == pytest.approx(6.0) and moved.value == pytest.approx(1.0, abs=0.05)
    assert window.undo_stack.count() == steps + 1
    window.undo_stack.undo()
    assert window.project.envelope(a.id, MIXER_PAN)[0] == added


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
    QTest.mouseDClick(lanes, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, point(window, a.id, 3.0, 0.8))
    assert [p.beat for p in window.project.envelope(a.id, MIXER_PAN)] == [1.0, 5.0, 7.0]
    # Select two (Shift adds) and press Delete.
    QTest.mouseClick(lanes, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, point(window, a.id, 1.0, 0.2))
    QTest.mouseClick(lanes, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.ShiftModifier,
                     point(window, a.id, 7.0, 0.6))
    assert window.selection.points == (a.id, MIXER_PAN, frozenset({0, 2}))
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
    QTest.mouseClick(master, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier,
                     point(window, MASTER, 0.0, 0.0))
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
    QTest.mouseClick(window.arrangement.lanes, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier,
                     point(window, a.id, 1.0, 1.0, MIXER_PAN))
    assert window.project.envelope(a.id, MIXER_PAN)[0].value == pytest.approx(1.0, abs=0.05)
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
