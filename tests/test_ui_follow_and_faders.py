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
    move(knob, 70)  # 30 px up: a twentieth of the range
    assert knob.value() == pytest.approx(0.55)
    move(knob, 40, SHIFT)  # 30 px more, fine: a tenth of that (not back to near where it started)
    assert knob.value() == pytest.approx(0.555)
    move(knob, 10)  # and Shift let go of: coarse again from there
    assert knob.value() == pytest.approx(0.605)
    QTest.mouseRelease(knob, Qt.MouseButton.LeftButton, NONE, QPoint(10, 10))

    box = ValueBox(0.0, -70.0, 6.0, step=0.25, decimals=3)
    QTest.mousePress(box, Qt.MouseButton.LeftButton, NONE, QPoint(10, 100))
    move(box, 60)  # 40 px up at a quarter of a step each: 2.5
    assert box.value() == pytest.approx(2.5)
    for y in range(59, 39, -1):  # 20 px, fine, a pixel at a time: they add up
        move(box, y, SHIFT)
    assert box.value() == pytest.approx(2.625)
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


def test_the_cursor_hides_while_dragging_and_comes_back_where_it_was(app):
    for widget in (Knob(0.0, 1.0, 0.5), ValueBox(0.0, -70.0, 6.0, step=0.25)):
        QTest.mousePress(widget, Qt.MouseButton.LeftButton, NONE, QPoint(10, 100))
        assert QApplication.overrideCursor() is None  # a click alone leaves it be
        move(widget, 80)
        assert QApplication.overrideCursor().shape() == Qt.CursorShape.BlankCursor
        move(widget, 60)
        QTest.mouseRelease(widget, Qt.MouseButton.LeftButton, NONE, QPoint(10, 60))
        assert QApplication.overrideCursor() is None


def test_a_drag_carries_on_past_the_bottom_of_the_screen(app):
    """The hidden cursor jumps to the middle of the screen at its edge (and the
    drag goes on from there), so a slow fine drag can go all the way."""
    box = ValueBox(0.0, -70.0, 6.0, step=0.25, decimals=2)
    box.show()
    area = box.screen().geometry()
    start = box.mapFromGlobal(QPoint(area.center().x(), area.center().y()))
    QTest.mousePress(box, Qt.MouseButton.LeftButton, NONE, start)
    middle = start.y()
    bottom = box.mapFromGlobal(QPoint(0, area.bottom())).y()
    for _ in range(3):  # to the bottom: the cursor jumps back to the middle each time
        move(box, middle + 1, SHIFT)
        move(box, bottom, SHIFT)
    expected = -3 * (bottom - middle) * 0.25 * 0.025
    assert box.value() == pytest.approx(round(expected, 2), abs=0.01)
    QTest.mouseRelease(box, Qt.MouseButton.LeftButton, NONE, start)
    box.close()


def test_a_popup_mid_drag_ends_the_drag(app):
    """A right-click menu opening while dragging gets the release: the cursor
    comes back now, and the knob stops following the mouse."""
    from PySide6.QtWidgets import QMenu

    knob = Knob(0.0, 1.0, 0.5)
    knob.show()
    QTest.mousePress(knob, Qt.MouseButton.LeftButton, NONE, QPoint(10, 100))
    move(knob, 70)
    assert QApplication.overrideCursor() is not None
    value = knob.value()
    menu = QMenu()
    menu.addAction("Show Automation")
    menu.popup(knob.mapToGlobal(QPoint(10, 70)))
    assert QApplication.overrideCursor() is None
    menu.close()
    move(knob, 20)
    assert knob.value() == value
    QTest.mouseRelease(knob, Qt.MouseButton.LeftButton, NONE, QPoint(10, 20))
    assert QApplication.overrideCursor() is None
    knob.close()
