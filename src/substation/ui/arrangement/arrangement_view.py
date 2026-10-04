"""Composes the arrangement: ruler on top, lanes with track headers on the right
(as in Ableton), the return tracks and the master pinned at the bottom (the
returns above the master, a compact row each), shared scrollbars.

Automation shows per track (and for the master): 'A' shows or hides it all, and
changing a parameter by hand shows its track's automation with that parameter
in the track's own lane."""

from __future__ import annotations

from PySide6.QtCore import QRect, Qt, Signal
from PySide6.QtGui import QColor, QMouseEvent, QPainter
from PySide6.QtWidgets import QGridLayout, QScrollBar, QSizePolicy, QVBoxLayout, QWidget

from ... import theme
from ...audio.engine_bridge import EngineBridge
from ...model.automation import MASTER
from ...model.editor import ProjectEditor
from ...model.project import routing_graph
from ..clip_view import ClipView
from .bus_tracks import (
    MASTER_HEIGHT,
    BusLane,
    MasterHeader,
    MasterLane,
    ReturnHeader,
    return_rows,
)
from .lanes_canvas import LanesCanvas
from .ruler import TimelineRuler
from .track_headers import NAME_ROW, TrackHeaderColumn
from .view_state import Selection, TrackLayout, ViewState, automation_rows
from .waveform_cache import WaveformCache

HEADER_WIDTH = 252
DROP_ZONE = 120  # empty space below the last track for dropping files


class GridInfo(QWidget):
    """Corner above the headers: shows the grid size; click toggles snapping."""

    def __init__(self, view: ViewState, parent: QWidget | None = None):
        super().__init__(parent)
        self.view = view
        self.setToolTip("Grid (Ctrl+1 narrower, Ctrl+2 wider). Click to toggle snapping (Ctrl+4).")
        view.changed.connect(self.update)
        view.grid_changed.connect(self.update)
        view.project.settings_changed.connect(self.update)

    def paintEvent(self, _event) -> None:
        p = QPainter(self)
        p.fillRect(self.rect(), QColor(theme.PANEL))
        p.fillRect(QRect(0, self.height() - 1, self.width(), 1), QColor(theme.BORDER))
        p.fillRect(QRect(0, 0, 1, self.height()), QColor(theme.BORDER))
        step = self.view.grid_step()
        bar = self.view.project.time_signature.beats_per_bar
        if step >= bar:
            label = f"{round(step / bar)} Bar" + ("s" if step > bar else "")
        else:
            label = f"1/{round(4 / step)}"
        p.setPen(QColor(theme.TEXT if self.view.snap else theme.TEXT_DISABLED))
        p.drawText(self.rect().adjusted(10, 0, -8, 0), Qt.AlignmentFlag.AlignVCenter,
                   f"Grid {label}" + ("" if self.view.snap else " (off)"))

    def mousePressEvent(self, _event: QMouseEvent) -> None:
        self.view.set_snap(not self.view.snap)


def _stack() -> tuple[QWidget, QVBoxLayout]:
    """A column of rows of fixed height (the returns' lanes, or their headers)."""
    widget = QWidget()
    layout = QVBoxLayout(widget)
    layout.setContentsMargins(0, 0, 0, 0)
    layout.setSpacing(0)
    widget.setSizePolicy(QSizePolicy.Policy.Preferred, QSizePolicy.Policy.Fixed)
    return widget, layout


class ArrangementView(QWidget):
    locate_requested = Signal(float)
    status_message = Signal(str)

    def __init__(self, editor: ProjectEditor, selection: Selection, bridge: EngineBridge,
                 parent: QWidget | None = None):
        super().__init__(parent)
        self.editor = editor
        self.project = editor.project
        self.selection = selection
        self.bridge = bridge
        self.view = ViewState(self.project, self)
        self.layout_model = TrackLayout(self.project)
        self.waveforms = WaveformCache()

        self.ruler = TimelineRuler(self.view, editor, selection)
        self.lanes = LanesCanvas(editor, self.view, self.layout_model, selection, bridge, self.waveforms)
        self.headers = TrackHeaderColumn(editor, self.view, self.layout_model, selection, bridge)
        self.headers.setFixedWidth(HEADER_WIDTH)
        self.grid_info = GridInfo(self.view)
        self.grid_info.setFixedWidth(HEADER_WIDTH)
        self.master_lane = MasterLane(editor, self.view, selection, bridge)
        self.master_header = MasterHeader(editor, bridge, selection)
        self.master_header.setFixedWidth(HEADER_WIDTH)
        self._update_master_height()
        # The return tracks: a lane and a header each, above the master's.
        self.return_lanes, self._return_lane_layout = _stack()
        self.return_headers, self._return_header_layout = _stack()
        self.return_headers.setFixedWidth(HEADER_WIDTH)
        self._returns: dict[str, tuple[BusLane, ReturnHeader]] = {}
        self.hbar = QScrollBar(Qt.Orientation.Horizontal)
        self.vbar = QScrollBar(Qt.Orientation.Vertical)
        for bar in (self.hbar, self.vbar):
            bar.setFocusPolicy(Qt.FocusPolicy.NoFocus)

        grid = QGridLayout(self)
        grid.setContentsMargins(0, 0, 0, 0)
        grid.setSpacing(0)
        grid.addWidget(self.ruler, 0, 0)
        grid.addWidget(self.grid_info, 0, 1, 1, 2)
        grid.addWidget(self.lanes, 1, 0)
        grid.addWidget(self.headers, 1, 1)
        grid.addWidget(self.vbar, 1, 2, 3, 1)
        grid.addWidget(self.return_lanes, 2, 0)
        grid.addWidget(self.return_headers, 2, 1)
        grid.addWidget(self.master_lane, 3, 0)
        grid.addWidget(self.master_header, 3, 1)
        grid.addWidget(self.hbar, 4, 0)
        grid.setColumnStretch(0, 1)
        grid.setRowStretch(1, 1)

        p = self.project
        for signal in (p.track_inserted, p.track_removed, p.tracks_arranged):
            signal.connect(self._on_structure_changed)
        for signal in (p.return_inserted, p.return_removed):
            signal.connect(self._on_returns_changed)
        p.reset.connect(self._on_reset)
        p.track_changed.connect(self._on_track_changed)
        p.devices_changed.connect(lambda _tid: self._refresh_sends())  # (a sidechain is a routing edge too)
        p.automation_view_changed.connect(self._on_automation_view_changed)
        p.automation_changed.connect(lambda *_args: self.selection.prune(self.project))
        editor.parameter_touched.connect(self._on_parameter_touched)
        p.clips_changed.connect(lambda _tid: (self.selection.prune(self.project), self._update_hbar()))
        p.settings_changed.connect(self._update_hbar)
        self.view.changed.connect(self._update_hbar)
        self.view.vscroll_changed.connect(self._sync_vbar_value)
        self.hbar.valueChanged.connect(self._on_hbar)
        self.vbar.valueChanged.connect(self.view.set_scroll_y)
        self.ruler.locate_requested.connect(self.locate_requested)
        self.lanes.status_message.connect(self.status_message)

        # Double-clicking a clip opens the clip view over the whole arrangement.
        self.clip_view = ClipView(editor, bridge, self, selection=self.selection)
        self.clip_view.hide()
        self.lanes.clip_view_requested.connect(
            lambda track_id, clip_id: self.open_clips(self.selection.clips, lead=(track_id, clip_id)))
        self.clip_view.closed.connect(self.lanes.setFocus)
        self.clip_view.locate_requested.connect(self.locate_requested)
        bridge.position_changed.connect(self._on_position)
        bridge.transport_changed.connect(self._on_transport)
        self._on_reset()

    # --- Model changes -----------------------------------------------------------

    def _on_reset(self) -> None:
        self.waveforms.clear()
        self.view.scroll_beats = 0.0
        self.view.scroll_y = 0
        self._sync_returns()
        self._on_structure_changed()
        self.headers.refresh_all()
        self._update_master_height()
        self.view.changed.emit()

    def _on_automation_view_changed(self, owner: str) -> None:
        if owner == MASTER:
            self._update_master_height()
        elif self.project.has_return(owner):
            self._update_return_height(owner)
        elif self.project.has_track(owner):
            self._on_track_changed(owner)
            self.headers.relayout()

    def _on_returns_changed(self, *_args) -> None:
        """A return came or went: its rows, and every header's send knobs (their
        letters, and how tall a track's lane is while its automation shows)."""
        self._sync_returns()
        self.headers.refresh_all()
        self._on_structure_changed()
        self.headers.relayout()

    def _sync_returns(self) -> None:
        """A lane and a header for each return, in their order."""
        wanted = [r.id for r in self.project.returns]
        for return_id in [r for r in self._returns if r not in wanted]:
            for widget in self._returns.pop(return_id):
                widget.hide()
                widget.deleteLater()
        for index, return_id in enumerate(wanted):
            if return_id not in self._returns:
                lane = BusLane(return_id, self.editor, self.view, self.selection, self.bridge)
                header = ReturnHeader(return_id, self.editor, self.bridge, self.selection)
                header.setFixedWidth(HEADER_WIDTH)
                self._returns[return_id] = (lane, header)
            lane, header = self._returns[return_id]
            self._return_lane_layout.insertWidget(index, lane)
            self._return_header_layout.insertWidget(index, header)
            header.refresh()
            self._update_return_height(return_id)
        self.return_lanes.setVisible(bool(wanted))
        self.return_headers.setVisible(bool(wanted))

    def _update_return_height(self, return_id: str) -> None:
        if return_id not in self._returns:
            return
        main_height, lanes = return_rows(self.project, return_id)
        height = main_height + sum(lane.height for lane in lanes)
        for widget in self._returns[return_id]:
            widget.setFixedHeight(height)
            widget.set_rows(main_height, lanes)

    def return_row(self, return_id: str) -> tuple[BusLane, ReturnHeader] | None:
        """A return's lane and header (for tests)."""
        return self._returns.get(return_id)

    def _update_master_height(self) -> None:
        main_height, lanes = automation_rows(self.project.master.automation_view, 0, MASTER_HEIGHT)
        height = main_height + sum(lane.height for lane in lanes)
        self.master_lane.setFixedHeight(height)
        self.master_header.setFixedHeight(height)
        self.master_lane.set_rows(main_height, lanes)
        self.master_header.set_rows(main_height, lanes)

    def _on_parameter_touched(self, owner: str, key: str) -> None:
        """A parameter changed by hand: its automation shows (as the lane's parameter)."""
        view = self.project.automation_view(owner) if self.project.has_owner(owner) else None
        if view is None or (view.shown and view.key == key):
            return
        if self.bridge.can_automate(owner, key):
            self.editor.show_automation(owner, key)

    def _on_structure_changed(self, *_args) -> None:
        self.layout_model.rebuild()
        self.headers.sync()
        self._refresh_sends()  # a track into or out of a group: which sends would close a cycle
        self.selection.prune(self.project)
        self._update_vbar()
        self.lanes.update()

    def _refresh_sends(self) -> None:
        """Every send knob: which can be used depends on the whole routing graph
        (groups, sends, inputs and sidechains)."""
        graph = routing_graph(self.project.tracks, self.project.returns)  # once, not per knob
        for _lane, header in self._returns.values():
            header.sends.refresh(graph)
        for header in self.headers.headers.values():
            header.sends.refresh(graph)

    def _on_track_changed(self, track_id: str) -> None:
        for track in self.project.tracks:  # what takes its output as its input shows its name
            if track.input_track == track_id:
                self.headers.refresh(track.id)
        self._refresh_sends()  # its sends or its input: which sends would close a cycle
        if track_id in self._returns:  # its name and mixer
            self._returns[track_id][1].refresh()
            return
        old_height = self.layout_model.total_height
        old_row = self.layout_model.row_for(track_id)
        self.layout_model.rebuild()
        self.headers.refresh(track_id)
        if self.layout_model.total_height != old_height or self.layout_model.row_for(track_id) != old_row:
            self.headers.relayout()
            self._update_vbar()
        self.lanes.update()

    def rename_track(self, track_id: str) -> bool:
        """Ctrl+R: rename a track (or a return) in place, scrolled into view (False: it can't be)."""
        if track_id in self._returns:
            self._returns[track_id][1].start_rename()
            return True
        header = self.headers.headers.get(track_id)
        row = self.layout_model.row_for(track_id)
        if header is None or row is None:  # the master, or a track folded away in its group
            return False
        if not self.view.scroll_y <= row.top <= self.view.scroll_y + self.headers.height() - NAME_ROW:
            self.view.set_scroll_y(row.top)
        header.start_rename()
        return True

    # --- Scrolling ---------------------------------------------------------------

    def _update_hbar(self, *_args) -> None:
        width = max(1, self.lanes.width())
        ppb = self.view.px_per_beat
        bar = self.project.time_signature.beats_per_bar
        content_end = max(self.project.end_beat(), self.project.loop_end,
                          self.view.x_to_beat(width)) + 16 * bar
        self.hbar.blockSignals(True)
        self.hbar.setRange(0, max(0, int(content_end * ppb - width)))
        self.hbar.setPageStep(width)
        self.hbar.setSingleStep(max(1, width // 20))
        self.hbar.setValue(int(self.view.scroll_beats * ppb))
        self.hbar.blockSignals(False)

    def _on_hbar(self, value: int) -> None:
        self.view.scroll_by_hand(value / self.view.px_per_beat)

    def _update_vbar(self) -> None:
        viewport = max(1, self.lanes.height())
        maximum = max(0, self.layout_model.total_height + DROP_ZONE - viewport)
        self.view.max_scroll_y = maximum
        self.vbar.blockSignals(True)
        self.vbar.setRange(0, maximum)
        self.vbar.setPageStep(viewport)
        self.vbar.setSingleStep(24)
        self.vbar.blockSignals(False)
        if self.view.scroll_y > maximum:
            self.view.set_scroll_y(maximum)
        self._sync_vbar_value()

    def _sync_vbar_value(self) -> None:
        if self.view.scroll_y > self.view.max_scroll_y:
            self.view.set_scroll_y(self.view.max_scroll_y)
            return
        self.vbar.blockSignals(True)
        self.vbar.setValue(self.view.scroll_y)
        self.vbar.blockSignals(False)

    def resizeEvent(self, event) -> None:
        super().resizeEvent(event)
        self.clip_view.setGeometry(self.rect())
        self._update_hbar()
        self._update_vbar()

    # --- Playhead ------------------------------------------------------------------

    def _on_transport(self, _playing: bool) -> None:
        self.view.follow_paused = False  # stopping or starting playback follows again
        self._on_position(self.bridge.position)

    def _on_position(self, beat: float) -> None:
        shown = beat if self.bridge.is_playing else None  # stopped: only the start marker shows
        self.lanes.set_playhead(shown)
        self.ruler.set_playhead(shown)
        self.master_lane.set_playhead(shown)
        for lane, _header in self._returns.values():
            lane.set_playhead(shown)
        if self.view.follow and not self.view.follow_paused and self.bridge.is_playing:
            x = self.view.beat_to_x(beat)
            width = self.lanes.width()
            if x > width * 0.96 or x < 0:
                self.view.set_scroll_beats(beat - width * 0.04 / self.view.px_per_beat)

    # --- Clip view ------------------------------------------------------------------

    def open_clips(self, refs, lead: tuple[str, str] | None = None) -> None:
        self.clip_view.setGeometry(self.rect())
        self.clip_view.open_clips(refs, lead)

    def toggle_clip_view(self) -> None:
        """Shift+Tab: open the selected clips, or close the clip view."""
        if self.clip_view.isVisible():
            self.clip_view.close_view()
        elif self.selection.clips:
            self.open_clips(self.selection.clips)

    # --- Commands -----------------------------------------------------------------

    def zoom(self, factor: float) -> None:
        playhead_x = self.view.beat_to_x(self.bridge.position)
        anchor = playhead_x if 0 <= playhead_x <= self.lanes.width() else self.lanes.width() / 2
        self.view.zoom_at(anchor, factor)

    def zoom_to_arrangement(self) -> None:
        end = max(self.project.end_beat(), self.project.time_signature.beats_per_bar * 8)
        self.view.zoom_to_fit(0.0, end * 1.05, self.lanes.width())
