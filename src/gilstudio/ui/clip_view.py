"""Clip view: opened by double-clicking a clip (or Shift+Tab), it overlays the
arrangement with clip controls on the left and large waveforms on the right.

It edits every selected clip at once. Knobs move all clips by the same amount
(transposing up 2 semitones transposes each clip by 2, whatever it was at);
switches and the warp mode set the same value on all of them.

Warp locks a clip to the beat grid: its audio is taken to be at the segment BPM
and is stretched to follow the project tempo. Transpose and detune shift the
pitch without changing the speed, warped or not (except in Re-Pitch mode, where
speed and pitch move together like a turntable and transposing does nothing).
"""

from __future__ import annotations

import math
from dataclasses import replace

from PySide6.QtCore import QPointF, QRectF, Qt, Signal
from PySide6.QtGui import QColor, QKeyEvent, QPainter, QPen
from PySide6.QtWidgets import (
    QComboBox,
    QFrame,
    QGridLayout,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QScrollArea,
    QVBoxLayout,
    QWidget,
)

from .. import theme
from ..audio.engine_bridge import EngineBridge
from ..model.editor import ClipRef, ProjectEditor
from ..model.project import WARP_MODES, Clip
from ..model.timebase import beats_to_seconds, format_db, format_pan
from .arrangement.waveform_cache import WaveformCache
from .widgets import Knob, ToggleButton, ValueBox

WARP_MODE_TIPS = {
    "Beats": "Beats: keeps drum hits and other transients tight.",
    "Tones": "Tones: for pitched material with a clear melody or bass line.",
    "Texture": "Texture: smooth, for pads, ambience and noisy sounds.",
    "Re-Pitch": "Re-Pitch: no stretching; speed and pitch change together, like a turntable.",
    "Complex": "Complex: for full mixes and mixed material.",
    "Complex Pro": "Complex Pro: like Complex, and keeps the formants (vocal character) when transposing.",
}
TRANSPOSE_TIP = "Pitch shift in semitones; the speed stays the same"
DETUNE_TIP = "Fine pitch shift in cents"
REPITCH_NOTE = "Re-Pitch: the pitch follows the speed, so Transpose and Detune have no effect."
CONTROLS_WIDTH = 260
HEADER_HEIGHT = 30
RULER_HEIGHT = 20
MIN_BAND_HEIGHT = 40  # each clip's waveform when several are open
TIME_STEPS = [0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.25, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600]
BPM_RANGE = (20.0, 999.0)


def _section(title: str) -> tuple[QFrame, QVBoxLayout]:
    frame = QFrame()
    frame.setObjectName("clipSection")
    frame.setStyleSheet(f"#clipSection {{ background: {theme.PANEL_ALT}; border: 1px solid {theme.BORDER};"
                        f" border-radius: 4px; }}")
    label = QLabel(title)
    label.setFont(theme.ui_font(9, bold=True))
    layout = QVBoxLayout(frame)
    layout.setContentsMargins(8, 6, 8, 8)
    layout.setSpacing(4)
    layout.addWidget(label)
    return frame, layout


def _caption(text: str) -> QLabel:
    label = QLabel(text)
    label.setAlignment(Qt.AlignmentFlag.AlignCenter)
    label.setStyleSheet(f"color: {theme.TEXT_DIM}; font-size: 8pt;")
    return label


def _format_time(seconds: float, step: float) -> str:
    minutes, secs = divmod(seconds, 60.0)
    decimals = 0 if step >= 1 else min(3, math.ceil(-math.log10(step) - 1e-9))
    text = f"{secs:.{decimals}f}"
    return f"{int(minutes)}:{text.zfill(decimals + 3 if decimals else 2)}" if minutes else text


class KnobControl(QWidget):
    """Captioned knob with a value readout. With several clips it sits on the first
    clip's value and the readout shows the range of values."""

    def __init__(self, name: str, minimum: float, maximum: float, default: float, formatter, *,
                 bipolar: bool = False, tooltip: str = "", parent: QWidget | None = None):
        super().__init__(parent)
        self._format = formatter
        self.minimum = minimum
        self.maximum = maximum
        self.knob = Knob(minimum, maximum, default, default=default, bipolar=bipolar, formatter=formatter)
        self.knob.setFixedSize(38, 38)
        self.readout = QLabel(formatter(default))
        self.readout.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.readout.setStyleSheet("font-size: 8pt;")
        self.setToolTip(tooltip)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(1)
        layout.addWidget(_caption(name))
        layout.addWidget(self.knob, 0, Qt.AlignmentFlag.AlignHCenter)
        layout.addWidget(self.readout)

    def set_values(self, values: list[float]) -> None:
        self.knob.setValue(values[0])
        low, high = min(values), max(values)
        text = self._format(low) if low == high else f"{self._format(low)} … {self._format(high)}"
        self.readout.setText(text)
        self.readout.setToolTip("" if low == high else "Clips differ: changes move them all by the same amount.")


class ClipWaveform(QWidget):
    """Each open clip's whole source file, fitted to the width, with the part the
    clip plays marked. One clip gets a time ruler; several are stacked in bands."""

    def __init__(self, bridge: EngineBridge, parent: QWidget | None = None):
        super().__init__(parent)
        self.bridge = bridge
        self.clips: list[tuple[Clip, QColor]] = []
        self.waveforms = WaveformCache(max_tiles=200)
        self.setMinimumSize(200, 120)
        bridge.source_ready.connect(lambda *_args: self.update())
        bridge.source_failed.connect(lambda *_args: self.update())

    def set_clips(self, clips: list[tuple[Clip, str]]) -> None:
        self.clips = [(clip, QColor(color)) for clip, color in clips]
        self.update()

    def paintEvent(self, _event) -> None:
        p = QPainter(self)
        p.fillRect(self.rect(), QColor(theme.LANE))
        if len(self.clips) == 1:
            clip, color = self.clips[0]
            area = QRectF(0, RULER_HEIGHT, self.width(), self.height() - RULER_HEIGHT)
            total_sec = self._draw_band(p, clip, color, area)
            if total_sec:
                self._draw_ruler(p, total_sec)
            return
        band_height = max(MIN_BAND_HEIGHT, self.height() / max(1, len(self.clips)))
        shown = min(len(self.clips), max(1, int(self.height() // band_height)))
        for i, (clip, color) in enumerate(self.clips[:shown]):
            band = QRectF(0, i * band_height, self.width(), band_height - 1)
            self._draw_band(p, clip, color, band)
            self._draw_label(p, band, clip.name)
            p.fillRect(QRectF(0, band.bottom(), self.width(), 1), QColor(theme.BORDER))
        if shown < len(self.clips):
            p.setPen(QColor(theme.TEXT))
            p.drawText(QRectF(self.rect()).adjusted(0, 0, -8, -4),
                       Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignBottom,
                       f"+{len(self.clips) - shown} more")

    def _draw_band(self, p: QPainter, clip: Clip, color: QColor, area: QRectF) -> float | None:
        """Draw one clip's source in `area`; returns the source length in seconds."""
        source = self.bridge.source(clip.path)
        if source is None or source.frames <= 0:
            p.setPen(QColor(theme.TEXT_DIM))
            p.drawText(area, Qt.AlignmentFlag.AlignCenter,
                       "Missing file" if self.bridge.load_error(clip.path) else "Loading…")
            return None
        total_sec = source.frames / source.sample_rate

        def x_at(seconds: float) -> float:
            return area.left() + seconds / total_sec * area.width()

        # Clip region, waveform, then dim what the clip doesn't play.
        x0, x1 = x_at(clip.offset_sec), x_at(clip.offset_sec + clip.duration_sec)
        region = QColor(color)
        region.setAlpha(45)
        p.fillRect(QRectF(x0, area.top(), x1 - x0, area.height()), region)
        split = area.height() >= 60
        mids = ((area.top() + area.height() / 4, area.top() + area.height() * 3 / 4) if split
                else (area.center().y(),))
        p.setPen(QColor(theme.GRID_BEAT))
        for mid in mids:
            p.drawLine(QPointF(area.left(), mid), QPointF(area.right(), mid))
        p.save()
        p.setClipRect(area)
        self.waveforms.draw(p, source, area, area.left(), 0.0, source.frames / max(1.0, area.width()),
                            color, split_channels=split, visible=area)
        p.restore()
        dim = QColor(0, 0, 0, 120)
        p.fillRect(QRectF(area.left(), area.top(), x0 - area.left(), area.height()), dim)
        p.fillRect(QRectF(x1, area.top(), area.right() - x1, area.height()), dim)

        # Start / end markers with flags, as in Ableton's sample editor.
        p.setFont(theme.ui_font(7.5, bold=True))
        for x, label in ((x0, "S"), (x1, "E")):
            p.fillRect(QRectF(round(x) - (1 if label == "E" else 0), area.top(), 1, area.height()),
                       QColor(theme.TEXT))
            flag = QRectF(x if label == "S" else x - 12, area.top(), 12, 12)
            p.fillRect(flag, QColor(theme.TEXT))
            p.setPen(QColor(theme.ACCENT_TEXT))
            p.drawText(flag, Qt.AlignmentFlag.AlignCenter, label)
        return total_sec

    def _draw_label(self, p: QPainter, band: QRectF, name: str) -> None:
        p.setFont(theme.ui_font(8))
        metrics = p.fontMetrics()
        text = metrics.elidedText(name, Qt.TextElideMode.ElideRight, int(band.width() / 2))
        box = QRectF(band.left() + 4, band.bottom() - metrics.height() - 5,
                     metrics.horizontalAdvance(text) + 8, metrics.height() + 2)
        p.fillRect(box, QColor(0, 0, 0, 150))
        p.setPen(QColor(theme.TEXT))
        p.drawText(box, Qt.AlignmentFlag.AlignCenter, text)

    def _draw_ruler(self, p: QPainter, total_sec: float) -> None:
        p.fillRect(QRectF(0, 0, self.width(), RULER_HEIGHT), QColor(theme.PANEL))
        px_per_sec = self.width() / total_sec
        step = next((s for s in TIME_STEPS if s * px_per_sec >= 70), TIME_STEPS[-1])
        p.setFont(theme.ui_font(7.5))
        for i in range(int(total_sec / step) + 1):
            seconds = i * step
            x = round(seconds * px_per_sec)
            p.fillRect(QRectF(x, RULER_HEIGHT - 6, 1, 6), QColor(theme.TEXT_DIM))
            p.setPen(QColor(theme.TEXT_DIM))
            p.drawText(QPointF(x + 3, RULER_HEIGHT - 7), _format_time(seconds, step))
        p.setPen(QPen(QColor(theme.BORDER), 1))
        p.drawLine(QPointF(0, RULER_HEIGHT - 0.5), QPointF(self.width(), RULER_HEIGHT - 0.5))


class ClipView(QFrame):
    """Overlay over the arrangement; hidden until clips are opened."""

    closed = Signal()

    def __init__(self, editor: ProjectEditor, bridge: EngineBridge, parent: QWidget | None = None):
        super().__init__(parent)
        self.editor = editor
        self.project = editor.project
        self.clip_refs: list[ClipRef] = []  # the first one leads: knobs show its values
        # (gesture key, attribute, {clip id: value when the gesture started})
        self._baseline: tuple[object, str, dict[str, float]] | None = None
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.setAutoFillBackground(False)

        # --- Header: colour, name, info, close ---
        self.name = QLabel()
        self.name.setFont(theme.ui_font(10, bold=True))
        self.info = QLabel()
        self.info.setStyleSheet(f"color: {theme.TEXT_DIM};")
        close = QPushButton("×")
        close.setProperty("role", "flat")
        close.setFont(theme.ui_font(14))
        close.setFixedSize(24, 24)
        close.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        close.setToolTip("Back to the arrangement (Esc)")
        close.clicked.connect(self.close_view)
        header = QHBoxLayout()
        header.setContentsMargins(12, 0, 6, 0)
        header.setSpacing(10)
        header.addWidget(self.name)
        header.addWidget(self.info)
        header.addStretch(1)
        header.addWidget(close)
        header_widget = QWidget()
        header_widget.setFixedHeight(HEADER_HEIGHT)
        header_widget.setLayout(header)

        # --- Warp ---
        warp_box, warp_layout = _section("Warp")
        self.warp = ToggleButton("Warp", role="activator",
                                 tooltip="Lock the clip to the beat grid, so it follows the project tempo")
        self.warp.setFixedHeight(20)
        self.warp.toggled.connect(self._set_warp)
        self.mode = QComboBox()
        self.mode.addItems(WARP_MODES)
        self.mode.setPlaceholderText("Mixed")
        self.mode.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        for i, name in enumerate(WARP_MODES):
            self.mode.setItemData(i, WARP_MODE_TIPS[name], Qt.ItemDataRole.ToolTipRole)
        self.mode.setToolTip("How the clip is stretched (and transposed)")
        self.mode.setSizeAdjustPolicy(QComboBox.SizeAdjustPolicy.AdjustToMinimumContentsLengthWithIcon)
        self.mode.setMinimumContentsLength(8)
        self.mode.activated.connect(lambda i: self._set_all("warp_mode", WARP_MODES[i], "Change Warp Mode"))
        self.bpm = ValueBox(120.0, *BPM_RANGE, step=0.01, decimals=2, sample_text="999.00",
                            formatter=lambda v: f"{v:.2f}")
        self.bpm.setToolTip("Segment BPM: the tempo of the audio in the clip. Warped clips play at\n"
                            "project tempo ÷ segment BPM speed.")
        self.bpm.setFixedHeight(20)
        self.bpm.valueChanged.connect(lambda v, key: self._set_all("segment_bpm", v, "Change Segment BPM", key))
        halve = QPushButton(":2")
        double = QPushButton("×2")
        for button, factor in ((halve, 0.5), (double, 2.0)):
            button.setFocusPolicy(Qt.FocusPolicy.NoFocus)
            button.setFixedHeight(20)
            button.setMinimumWidth(34)
            button.setToolTip(("Halve" if factor < 1 else "Double") + " each clip's segment BPM"
                              f" (warped clips play {'twice as fast' if factor < 1 else 'half as fast'})")
            button.clicked.connect(lambda _=False, f=factor: self._scale_bpm(f))
        grid = QGridLayout()
        grid.setHorizontalSpacing(6)
        grid.setVerticalSpacing(4)
        grid.addWidget(self.warp, 0, 0)
        grid.addWidget(self.mode, 1, 0, 1, 3)
        grid.addWidget(_caption("Seg. BPM"), 2, 0)
        grid.addWidget(self.bpm, 2, 1, 1, 2)
        grid.addWidget(halve, 3, 1)
        grid.addWidget(double, 3, 2)
        warp_layout.addLayout(grid)

        # --- Pitch ---
        pitch_box, pitch_layout = _section("Pitch")
        self.transpose = KnobControl("Transpose", -48.0, 48.0, 0.0, lambda v: f"{round(v):+d} st",
                                     bipolar=True, tooltip=TRANSPOSE_TIP)
        self.detune = KnobControl("Detune", -50.0, 50.0, 0.0, lambda v: f"{round(v):+d} ct",
                                  bipolar=True, tooltip=DETUNE_TIP)
        row = QHBoxLayout()
        row.addWidget(self.transpose)
        row.addWidget(self.detune)
        pitch_layout.addLayout(row)

        # --- Mix ---
        mix_box, mix_layout = _section("Mix")
        self.gain = KnobControl("Volume", -70.0, 24.0, 0.0, format_db)
        self.pan = KnobControl("Pan", -1.0, 1.0, 0.0, format_pan, bipolar=True)
        row = QHBoxLayout()
        row.addWidget(self.gain)
        row.addWidget(self.pan)
        mix_layout.addLayout(row)

        for control, attr, text, integer in ((self.transpose, "transpose", "Transpose Clips", True),
                                             (self.detune, "detune", "Detune Clips", False),
                                             (self.gain, "gain_db", "Change Clip Volume", False),
                                             (self.pan, "pan", "Change Clip Pan", False)):
            control.knob.valueChanged.connect(
                lambda v, key, c=control, a=attr, t=text, i=integer: self._nudge_all(c, a, v, key, t, i))

        hint = QLabel("Warped clips follow the project tempo. Transpose keeps the speed.")
        hint.setWordWrap(True)
        hint.setStyleSheet(f"color: {theme.TEXT_DISABLED};")

        controls = QWidget()
        controls.setAutoFillBackground(False)
        column = QVBoxLayout(controls)
        column.setContentsMargins(8, 8, 8, 8)
        column.setSpacing(6)
        for box in (warp_box, pitch_box, mix_box):
            column.addWidget(box)
        column.addWidget(hint)
        column.addStretch(1)
        scroll = QScrollArea()
        scroll.setWidget(controls)
        scroll.setWidgetResizable(True)
        scroll.setFixedWidth(CONTROLS_WIDTH)
        scroll.setFrameShape(QFrame.Shape.NoFrame)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        scroll.setStyleSheet("QScrollArea { background: transparent; }")
        scroll.viewport().setAutoFillBackground(False)

        self.waveform = ClipWaveform(bridge)

        body = QHBoxLayout()
        body.setContentsMargins(0, 0, 0, 0)
        body.setSpacing(0)
        body.addWidget(scroll)
        body.addWidget(self.waveform, 1)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)
        layout.addWidget(header_widget)
        layout.addLayout(body, 1)

        self.project.clips_changed.connect(self._on_clips_changed)
        self.project.settings_changed.connect(self._refresh)
        self.project.track_changed.connect(self._refresh)
        self.project.reset.connect(self.close_view)
        self.project.track_removed.connect(lambda _tid, _i: self._drop_missing())

    def paintEvent(self, _event) -> None:
        p = QPainter(self)
        p.fillRect(self.rect(), QColor(theme.PANEL))
        p.fillRect(QRectF(0, HEADER_HEIGHT - 1, self.width(), 1), QColor(theme.BORDER))
        p.fillRect(QRectF(CONTROLS_WIDTH, HEADER_HEIGHT, 1, self.height()), QColor(theme.BORDER))
        clips = self._clips()
        if clips:
            p.fillRect(QRectF(0, 0, 5, HEADER_HEIGHT - 1), QColor(self.project.track(clips[0][0]).color))

    # --- Opening & closing -------------------------------------------------------

    def _clips(self) -> list[tuple[str, Clip]]:
        """(track id, clip) for each open clip that still exists, in order."""
        result = []
        for track_id, clip_id in self.clip_refs:
            if self.project.has_track(track_id):
                clip = next((c for c in self.project.track(track_id).clips if c.id == clip_id), None)
                if clip is not None:
                    result.append((track_id, clip))
        return result

    def open_clips(self, refs) -> None:
        """Show `refs` over the arrangement, ordered top track first, then by time."""
        self.clip_refs = list(refs)
        clips = sorted(self._clips(), key=lambda tc: (self.project.track_index(tc[0]), tc[1].start_beat))
        self.clip_refs = [(track_id, clip.id) for track_id, clip in clips]
        if not clips:
            self.close_view()
            return
        self._baseline = None
        self._refresh()
        self.show()
        self.raise_()
        self.setFocus()

    def close_view(self) -> None:
        was_open = self.isVisible()
        self.clip_refs = []
        self.waveform.set_clips([])
        self.hide()
        if was_open:
            self.closed.emit()

    def _drop_missing(self) -> None:
        """Forget deleted clips (or tracks); close when none are left."""
        if not self.clip_refs:
            return
        self.clip_refs = [(track_id, clip.id) for track_id, clip in self._clips()]
        if self.clip_refs:
            self._refresh()
        else:
            self.close_view()

    def _on_clips_changed(self, track_id: str) -> None:
        if any(ref[0] == track_id for ref in self.clip_refs):
            self._drop_missing()

    # --- Showing the clips' settings ------------------------------------------------

    def _segment_bpm(self, clip: Clip) -> float:
        return clip.segment_bpm or self.project.tempo

    def _refresh(self, *_args) -> None:
        items = self._clips()
        if not items:
            return
        clips = [clip for _, clip in items]
        lead = clips[0]
        if len(clips) == 1:
            self.name.setText(lead.name)
            tempo = self.project.tempo
            beats = lead.length_beats(tempo)
            self.info.setText(f"{beats_to_seconds(beats, tempo):.2f} s  ·  {beats:.2f} beats")
        else:
            tracks = len({track_id for track_id, _ in items})
            self.name.setText(f"{len(clips)} Clips")
            self.info.setText(f"on {tracks} track{'s' if tracks > 1 else ''}  ·  "
                              "changes apply to every selected clip")

        self.warp.set_checked_silently(all(c.warp for c in clips))
        modes = {c.warp_mode for c in clips}
        self.mode.setCurrentIndex(WARP_MODES.index(lead.warp_mode) if len(modes) == 1 else -1)
        self.bpm.setValue(self._segment_bpm(lead))
        self.transpose.set_values([c.transpose for c in clips])
        self.detune.set_values([c.detune for c in clips])
        repitch = all(c.is_warped and c.warp_mode == "Re-Pitch" for c in clips)
        for control, tip in ((self.transpose, TRANSPOSE_TIP), (self.detune, DETUNE_TIP)):
            control.setEnabled(not repitch)
            control.setToolTip(REPITCH_NOTE if repitch else tip)
        self.gain.set_values([c.gain_db for c in clips])
        self.pan.set_values([c.pan for c in clips])
        self.waveform.set_clips([(clip, self.project.track(track_id).color) for track_id, clip in items])
        self.update()

    # --- Editing all open clips -------------------------------------------------------

    def _set_warp(self, on: bool) -> None:
        """Warping a clip whose segment BPM was never set takes the project tempo,
        so the clip keeps its length and speed until the tempo changes."""
        self.editor.update_clips(
            self.clip_refs, lambda c: replace(c, warp=on, segment_bpm=c.segment_bpm or self.project.tempo),
            "Toggle Warp")

    def _set_all(self, attr: str, value, text: str, merge_key: object | None = None) -> None:
        """Give every open clip the same value (warp on/off, mode, segment BPM)."""
        self.editor.update_clips(self.clip_refs, lambda c: replace(c, **{attr: value}), text, merge_key)

    def _scale_bpm(self, factor: float) -> None:
        low, high = BPM_RANGE
        self.editor.update_clips(
            self.clip_refs,
            lambda c: replace(c, segment_bpm=round(max(low, min(high, self._segment_bpm(c) * factor)), 2)),
            "Change Segment BPM")

    def _nudge_all(self, control: KnobControl, attr: str, value: float, key: object, text: str,
                   integer: bool) -> None:
        """The knob shows the lead clip's value; move every clip by the lead's change.
        The change is measured from where the knob gesture started, so a clip held
        at a limit keeps its offset to the others when the knob comes back."""
        items = self._clips()
        if not items:
            return
        if self._baseline is None or self._baseline[0] is not key or self._baseline[1] != attr:
            self._baseline = (key, attr, {clip.id: getattr(clip, attr) for _, clip in items})
        start = self._baseline[2]
        delta = value - start[items[0][1].id]
        if integer:
            delta = round(delta)

        def change(clip: Clip) -> Clip:
            new = max(control.minimum, min(control.maximum, start.get(clip.id, getattr(clip, attr)) + delta))
            return replace(clip, **{attr: round(new) if integer else new})

        self.editor.update_clips(self.clip_refs, change, text, key)

    def keyPressEvent(self, event: QKeyEvent) -> None:
        if event.key() == Qt.Key.Key_Escape:
            self.close_view()
        else:
            super().keyPressEvent(event)
