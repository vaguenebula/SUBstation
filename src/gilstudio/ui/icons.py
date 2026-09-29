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
def app_icon() -> QIcon:
    def draw(p: QPainter, _c: QColor):
        p.setBrush(QColor(theme.PANEL_ALT))
        p.setPen(Qt.PenStyle.NoPen)
        p.drawRoundedRect(QRectF(2, 2, 60, 60), 12, 12)
        p.setPen(QPen(QColor(theme.ACCENT), 6, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
        for x, h in ((14, 12), (24, 30), (34, 40), (44, 22), (52, 10)):
            p.drawLine(QPointF(x, 32 - h / 2), QPointF(x, 32 + h / 2))
    return _icon(draw, theme.ACCENT)
