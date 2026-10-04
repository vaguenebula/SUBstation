"""Small vector icons drawn with QPainter, so they stay crisp at any DPI."""

from __future__ import annotations

from functools import cache

from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QColor, QIcon, QPainter, QPainterPath, QPen, QPixmap

from .. import theme

SIZE = 64


def _canvas() -> tuple[QPixmap, QPainter]:
    pixmap = QPixmap(SIZE, SIZE)
    pixmap.fill(Qt.GlobalColor.transparent)
    painter = QPainter(pixmap)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing)
    return pixmap, painter


def _icon(draw, color: str) -> QIcon:
    icon = QIcon()
    for mode, c in ((QIcon.Mode.Normal, color), (QIcon.Mode.Disabled, theme.TEXT_DISABLED)):
        pixmap, painter = _canvas()
        draw(painter, QColor(c))
        painter.end()
        icon.addPixmap(pixmap, mode)
    return icon


@cache
def play(color: str = theme.TEXT) -> QIcon:
    def draw(p: QPainter, c: QColor):
        path = QPainterPath(QPointF(18, 12))
        path.lineTo(52, 32)
        path.lineTo(18, 52)
        path.closeSubpath()
        p.fillPath(path, c)
    return _icon(draw, color)


@cache
def stop(color: str = theme.TEXT) -> QIcon:
    return _icon(lambda p, c: p.fillRect(QRectF(16, 16, 32, 32), c), color)


@cache
def record(color: str = "#ff5a4d") -> QIcon:
    def draw(p: QPainter, c: QColor):
        p.setBrush(c)
        p.setPen(Qt.PenStyle.NoPen)
        p.drawEllipse(QRectF(16, 16, 32, 32))
    return _icon(draw, color)


@cache
def metronome(color: str = theme.TEXT) -> QIcon:
    def draw(p: QPainter, c: QColor):
        body = QPainterPath(QPointF(24, 10))
        body.lineTo(40, 10)
        body.lineTo(52, 54)
        body.lineTo(12, 54)
        body.closeSubpath()
        p.setPen(QPen(c, 5, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin))
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawPath(body)
        p.drawLine(QPointF(32, 44), QPointF(46, 18))
    return _icon(draw, color)


@cache
def loop(color: str = theme.TEXT) -> QIcon:
    def draw(p: QPainter, c: QColor):
        p.setPen(QPen(c, 6, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
        p.drawArc(QRectF(10, 16, 44, 32), 30 * 16, 300 * 16)
        arrow = QPainterPath(QPointF(46, 8))
        arrow.lineTo(58, 20)
        arrow.lineTo(42, 24)
        arrow.closeSubpath()
        p.fillPath(arrow, c)
    return _icon(draw, color)


@cache
def follow(color: str = theme.TEXT) -> QIcon:
    def draw(p: QPainter, c: QColor):
        p.setPen(QPen(c, 5, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
        p.drawLine(QPointF(14, 32), QPointF(46, 32))
        head = QPainterPath(QPointF(40, 18))
        head.lineTo(56, 32)
        head.lineTo(40, 46)
        head.closeSubpath()
        p.fillPath(head, c)
        p.drawLine(QPointF(10, 12), QPointF(10, 52))
    return _icon(draw, color)


@cache
def re_enable_automation(color: str = theme.TEXT) -> QIcon:
    """An envelope with a loop back: automation plays again."""
    def draw(p: QPainter, c: QColor):
        p.setPen(QPen(c, 5, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin))
        p.setBrush(Qt.BrushStyle.NoBrush)
        line = QPainterPath(QPointF(8, 50))
        line.lineTo(24, 26)
        line.lineTo(38, 40)
        line.lineTo(56, 14)
        p.drawPath(line)
        p.setBrush(c)
        p.setPen(Qt.PenStyle.NoPen)
        for x, y in ((8, 50), (24, 26), (38, 40), (56, 14)):
            p.drawEllipse(QPointF(x, y), 6, 6)
    return _icon(draw, color)


@cache
def lock_envelopes(color: str = theme.TEXT) -> QIcon:
    """A padlock: closed when on (automation stays put), open when off."""
    def draw_lock(closed: bool):
        def draw(p: QPainter, c: QColor):
            p.setPen(QPen(c, 5, Qt.PenStyle.SolidLine, Qt.PenCapStyle.FlatCap))
            p.setBrush(Qt.BrushStyle.NoBrush)
            # The shackle: in the body when closed; raised, its left leg free, when open.
            top = 10.0 if closed else 3.0
            shackle = QPainterPath(QPointF(21, 32 if closed else top + 17))
            shackle.lineTo(21, top + 12)
            shackle.arcTo(QRectF(21, top, 22, 24), 180, -180)
            shackle.lineTo(43, 32)
            p.drawPath(shackle)
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(c)
            p.drawRoundedRect(QRectF(12, 30, 40, 28), 5, 5)
        return draw

    icon = QIcon()
    for state, closed in ((QIcon.State.On, True), (QIcon.State.Off, False)):
        for mode, c in ((QIcon.Mode.Normal, color), (QIcon.Mode.Disabled, theme.TEXT_DISABLED)):
            pixmap, painter = _canvas()
            draw_lock(closed)(painter, QColor(c))
            painter.end()
            icon.addPixmap(pixmap, mode, state)
    return icon


@cache
def headphones(color: str = theme.TEXT) -> QIcon:
    def draw(p: QPainter, c: QColor):
        p.setPen(QPen(c, 5, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
        p.drawArc(QRectF(12, 10, 40, 40), 0, 180 * 16)
        p.setBrush(c)
        p.drawRoundedRect(QRectF(10, 32, 10, 20), 3, 3)
        p.drawRoundedRect(QRectF(44, 32, 10, 20), 3, 3)
    return _icon(draw, color)


@cache
def folder(color: str = theme.TEXT_DIM) -> QIcon:
    def draw(p: QPainter, c: QColor):
        path = QPainterPath(QPointF(8, 16))
        path.lineTo(26, 16)
        path.lineTo(31, 22)
        path.lineTo(56, 22)
        path.lineTo(56, 50)
        path.lineTo(8, 50)
        path.closeSubpath()
        p.fillPath(path, c)
    return _icon(draw, color)


@cache
def waveform(color: str = theme.TEXT_DIM) -> QIcon:
    def draw(p: QPainter, c: QColor):
        p.setPen(QPen(c, 5, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
        for x, h in ((12, 8), (22, 24), (32, 36), (42, 18), (52, 10)):
            p.drawLine(QPointF(x, 32 - h / 2), QPointF(x, 32 + h / 2))
    return _icon(draw, color)


@cache
def plugin(color: str = theme.TEXT_DIM) -> QIcon:
    def draw(p: QPainter, c: QColor):
        p.setPen(QPen(c, 5))
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawRoundedRect(QRectF(12, 14, 40, 36), 5, 5)
        p.drawLine(QPointF(24, 14), QPointF(24, 6))
        p.drawLine(QPointF(40, 14), QPointF(40, 6))
    return _icon(draw, color)


@cache
def preset(color: str = theme.TEXT_DIM) -> QIcon:
    """Three sliders, set: a device's settings saved."""
    def draw(p: QPainter, c: QColor):
        p.setPen(QPen(c, 5, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
        for y, x in ((14, 40), (32, 20), (50, 34)):
            p.drawLine(QPointF(8, y), QPointF(56, y))
            p.setBrush(c)
            p.drawEllipse(QPointF(x, y), 5, 5)
    return _icon(draw, color)


@cache
def plugin_window(color: str = theme.TEXT) -> QIcon:
    """A window with a title bar: shows a plug-in's own editor."""
    def draw(p: QPainter, c: QColor):
        p.setPen(QPen(c, 5))
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawRoundedRect(QRectF(8, 12, 48, 40), 4, 4)
        p.fillRect(QRectF(8, 12, 48, 11), c)
    return _icon(draw, color)


@cache
def sidechain(color: str = theme.TEXT) -> QIcon:
    """An arrow coming in from the side: a device's sidechain input."""
    def draw(p: QPainter, c: QColor):
        p.setPen(QPen(c, 6, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin))
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawLine(QPointF(48, 10), QPointF(48, 54))  # what it goes into
        p.drawLine(QPointF(8, 32), QPointF(36, 32))
        head = QPainterPath(QPointF(26, 20))
        head.lineTo(38, 32)
        head.lineTo(26, 44)
        p.drawPath(head)
    return _icon(draw, color)


@cache
def snowflake(color: str = theme.FROZEN) -> QIcon:
    """Six spokes with a pair of twigs each: a frozen track."""
    def draw(p: QPainter, c: QColor):
        p.setPen(QPen(c, 4.5, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
        p.translate(32, 32)
        for _ in range(6):
            p.drawLine(QPointF(0, 0), QPointF(0, -26))
            p.drawLine(QPointF(0, -15), QPointF(-8, -23))
            p.drawLine(QPointF(0, -15), QPointF(8, -23))
            p.rotate(60)
    return _icon(draw, color)


@cache
def save(color: str = theme.TEXT) -> QIcon:
    """A floppy disk."""
    def draw(p: QPainter, c: QColor):
        body = QPainterPath(QPointF(10, 10))
        body.lineTo(46, 10)
        body.lineTo(54, 18)
        body.lineTo(54, 54)
        body.lineTo(10, 54)
        body.closeSubpath()
        p.setPen(QPen(c, 5, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin))
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawPath(body)
        p.fillRect(QRectF(20, 10, 22, 13), c)
        p.drawRect(QRectF(19, 35, 26, 19))
    return _icon(draw, color)


@cache
def link(color: str = theme.TEXT) -> QIcon:
    """Two chain links: one side follows the other."""
    def draw(p: QPainter, c: QColor):
        p.setPen(QPen(c, 6, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin))
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.save()
        p.translate(32, 32)
        p.rotate(-45)
        p.drawRoundedRect(QRectF(-26, -9, 30, 18), 9, 9)
        p.drawRoundedRect(QRectF(-4, -9, 30, 18), 9, 9)
        p.restore()
    return _icon(draw, color)


@cache
def infinity(color: str = theme.TEXT) -> QIcon:
    """A lemniscate: what is held goes round for ever (a delay's Freeze)."""
    def draw(p: QPainter, c: QColor):
        path = QPainterPath(QPointF(32, 32))
        path.cubicTo(QPointF(42, 16), QPointF(58, 20), QPointF(58, 32))
        path.cubicTo(QPointF(58, 44), QPointF(42, 48), QPointF(32, 32))
        path.cubicTo(QPointF(22, 16), QPointF(6, 20), QPointF(6, 32))
        path.cubicTo(QPointF(6, 44), QPointF(22, 48), QPointF(32, 32))
        p.setPen(QPen(c, 5, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin))
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawPath(path)
    return _icon(draw, color)


@cache
def expand(color: str = theme.TEXT) -> QIcon:
    """Two arrows apart: show it bigger, in a window of its own."""
    def draw(p: QPainter, c: QColor):
        p.setPen(QPen(c, 5, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin))
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawLine(QPointF(14, 50), QPointF(50, 14))
        for corner, dx, dy in (((50, 14), -1, 1), ((14, 50), 1, -1)):
            path = QPainterPath(QPointF(corner[0] + 18 * dx, corner[1]))
            path.lineTo(QPointF(*corner))
            path.lineTo(QPointF(corner[0], corner[1] + 18 * dy))
            p.drawPath(path)
    return _icon(draw, color)


@cache
def sliders(color: str = theme.TEXT) -> QIcon:
    """Three faders: show a device's controls."""
    def draw(p: QPainter, c: QColor):
        p.setPen(QPen(c, 4, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
        faders = ((16, 40), (32, 22), (48, 34))  # (x, the knob's y)
        for x, _knob in faders:
            p.drawLine(QPointF(x, 10), QPointF(x, 54))
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(c)
        for x, knob in faders:
            p.drawRoundedRect(QRectF(x - 8, knob - 5, 16, 10), 3, 3)
    return _icon(draw, color)


@cache
def fold(folded: bool, color: str = theme.TEXT) -> QIcon:
    """A device's fold button: a triangle pointing down while it is open, right while folded."""
    def draw(p: QPainter, c: QColor):
        points = ((22, 14), (46, 32), (22, 50)) if folded else ((14, 22), (50, 22), (32, 46))
        path = QPainterPath(QPointF(*points[0]))
        for point in points[1:]:
            path.lineTo(QPointF(*point))
        path.closeSubpath()
        p.fillPath(path, c)
    return _icon(draw, color)


@cache
def search(color: str = theme.TEXT_DIM) -> QIcon:
    def draw(p: QPainter, c: QColor):
        p.setPen(QPen(c, 5, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawEllipse(QRectF(10, 10, 30, 30))
        p.drawLine(QPointF(37, 37), QPointF(54, 54))
    return _icon(draw, color)


@cache
def app_icon() -> QIcon:
    def draw(p: QPainter, _c: QColor):
        p.setBrush(QColor(theme.PANEL_ALT))
        p.setPen(Qt.PenStyle.NoPen)
        p.drawRoundedRect(QRectF(2, 2, 60, 60), 12, 12)
        p.setPen(QPen(QColor(theme.ACCENT), 6, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
        for x, h in ((14, 12), (24, 30), (34, 40), (44, 22), (52, 10)):
            p.drawLine(QPointF(x, 32 - h / 2), QPointF(x, 32 + h / 2))
    return _icon(draw, theme.ACCENT)
