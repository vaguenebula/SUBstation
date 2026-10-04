"""UiFacts on the main window: what the intelligence layer may know of the UI
(the selection, the device shown, the plug-ins the scan found, the browser's
places), as plain values. The layer never imports the UI; the window hands it this."""

from __future__ import annotations

from typing import TYPE_CHECKING

from ...intel.facts import AvailablePlugin, SelectionFacts

if TYPE_CHECKING:
    from ..main_window import MainWindow


class WindowFacts:
    def __init__(self, window: MainWindow):
        self.window = window

    def selection(self) -> SelectionFacts:
        s = self.window.selection
        time_range, range_tracks = None, ()
        if s.time_range is not None:
            start, end, track_ids = s.time_range
            time_range, range_tracks = (start, end), tuple(track_ids)
        points = None
        if s.points is not None:
            owner, key, indices = s.points
            points = (owner, key, tuple(sorted(indices)))
        panel = self.window.devices
        device = (panel.track_id, panel.selected[0]) if panel.track_id and panel.selected else None
        return SelectionFacts(
            time_range=time_range, range_tracks=range_tracks, clips=tuple(sorted(s.clips)),
            lanes=tuple(s.lanes), points=points, tracks=tuple(s.track_ids), track=s.track_id, device=device,
            insert_beat=s.insert_beat)

    def available_plugins(self) -> list[AvailablePlugin]:
        return [AvailablePlugin(uid=p.uid, name=p.name, vendor=p.vendor, category=p.category,
                                instrument=p.instrument, path=p.path, format=p.format)
                for p in self.window.browser.plugin_index.plugins]

    def places(self) -> list[str]:
        return list(self.window.browser.places)
