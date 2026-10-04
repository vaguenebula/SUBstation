"""Following the playhead while scrolling by hand, and dragging knobs and value
boxes (Shift for fine control, also pressed mid-drag)."""

import pytest
from PySide6.QtCore import QPoint, QPointF, Qt
from PySide6.QtGui import QMouseEvent
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication

from substation.ui.widgets import Knob, ValueBox

SHIFT = Qt.KeyboardModifier.ShiftModifier
NONE = Qt.KeyboardModifier.NoModifier


def move(widget, y: int, modifiers=NONE) -> None:
    """A mouse move with the left button held (QTest.mouseMove can't send modifiers)."""
    point = QPointF(10, y)
    QApplication.sendEvent(widget, QMouseEvent(QMouseEvent.Type.MouseMove, point, widget.mapToGlobal(point),
                                               Qt.MouseButton.NoButton, Qt.MouseButton.LeftButton, modifiers))


def test_pressing_shift_mid_drag_carries_on_from_there(app):
    knob = Knob(0.0, 1.0, 0.5)
    QTest.mousePress(knob, Qt.MouseButton.LeftButton, NONE, QPoint(10, 100))
    move(knob, 70)  # 30 px up: a tenth of the range
    assert knob.value() == pytest.approx(0.6)
    move(knob, 40, SHIFT)  # 30 px more, fine: a hundredth (not back to near where it started)
    assert knob.value() == pytest.approx(0.61)
    move(knob, 10)  # and Shift let go of: coarse again from there
    assert knob.value() == pytest.approx(0.71)
    QTest.mouseRelease(knob, Qt.MouseButton.LeftButton, NONE, QPoint(10, 10))

    box = ValueBox(0.0, -70.0, 6.0, step=0.25, decimals=2)
    QTest.mousePress(box, Qt.MouseButton.LeftButton, NONE, QPoint(10, 100))
    move(box, 60)  # 40 px up at half a step each: 5
    assert box.value() == pytest.approx(5.0)
    for y in range(59, 39, -1):  # 20 px, fine, a pixel at a time: they add up
        move(box, y, SHIFT)
    assert box.value() == pytest.approx(5.25)
    QTest.mouseRelease(box, Qt.MouseButton.LeftButton, NONE, QPoint(10, 40))


def test_scrolling_by_hand_stops_following_the_playhead_until_playback_restarts(window):
    arrangement = window.arrangement
    view, bridge = arrangement.view, window.bridge
    assert view.follow
    bridge.play()
    assert bridge.is_playing
    try:
        far = view.x_to_beat(arrangement.lanes.width() * 2)
        arrangement._on_position(far)  # off the right edge: the view follows it
        assert view.scroll_beats > 0
        lanes = arrangement.lanes
        ctrl_alt = Qt.KeyboardModifier.ControlModifier | Qt.KeyboardModifier.AltModifier
        QTest.mousePress(lanes, Qt.MouseButton.LeftButton, ctrl_alt, QPoint(200, 20))
        QTest.mouseMove(lanes, QPoint(260, 20))  # pans back toward the start
        scrolled = view.scroll_beats
        arrangement._on_position(far + 64)  # the playhead off screen: the view stays where it was put
        assert view.scroll_beats == scrolled and view.follow_paused
        QTest.mouseRelease(lanes, Qt.MouseButton.LeftButton, ctrl_alt, QPoint(260, 20))
        arrangement._on_position(far + 128)
        assert view.scroll_beats == scrolled
    finally:
        bridge.stop()
    assert not view.follow_paused  # stopping (or starting) playback follows again
    bridge.play()
    try:
        arrangement._on_position(far + 128)
        assert view.scroll_beats > scrolled
    finally:
        bridge.stop()
