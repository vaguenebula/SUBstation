"""Plug-ins: where they are (the scan), their states, their editors (open,
shown with the selected track's devices, hidden and closed), and what they
report (edits made in their editors, parameters changed or rebuilt)."""

from __future__ import annotations

import base64
from dataclasses import replace

from ... import _engine as ge
from ...model.project import iter_devices

# Plug-in editors of tracks not shown are hidden but keep running (animating,
# messaging their processors); this many at most, then the ones hidden longest close.
MAX_HIDDEN_EDITORS = 8


class PluginHost:
    """Plug-ins' states, editors and events. Part of EngineBridge (engine_bridge/__init__.py)."""

    def set_known_plugins(self, plugins) -> None:
        """The scanned plug-ins (PluginInfo): lets projects find plug-ins that moved.
        Devices whose plug-in wasn't found get another try."""
        self.known_plugins = {p.uid: p.path for p in plugins}
        reloaded_tracks = set()
        for key, (track_id, devices) in self._model_chains().items():
            chain = self._devices.get(key)
            if chain is None or not any(pid is None for _, pid in chain):
                continue
            by_id = {d.id: d for d in devices}
            reloaded = []
            for device_id, processor_id in chain:
                device = by_id.get(device_id)
                if processor_id is None and device is not None and device.is_plugin and self.plugin_path(device.plugin):
                    processor_id = self._pids[device_id] = self._load_plugin(self._chains[key], device)
                reloaded.append((device_id, processor_id))
            if reloaded != chain:
                self._devices[key] = reloaded
                self.engine.set_chain_order(self._chains[key], [pid for _, pid in reloaded if pid is not None])
                reloaded_tracks.add(track_id)
        for track_id in reloaded_tracks:
            self._push_enabled(self.project.track(track_id))
            self._push_automation(track_id)
            self.devices_loaded.emit(track_id)

    def plugin_state(self, track_id: str, device_id: str) -> bytes | None:
        """A plug-in device's current state (a .vstpreset), None if it isn't loaded."""
        engine_id = self.engine_device_id(track_id, device_id)
        if engine_id is None or engine_id not in self._plugin_ids:
            return None
        return self.engine.processor_state(engine_id)

    def store_plugin_states(self, device_ids=None) -> None:
        """Copy every plug-in's state (or those of `device_ids`) into the model, for
        saving the project (or copying the devices)."""
        for track in self.project.all_tracks():
            for device in iter_devices(track.devices):
                if device_ids is not None and device.id not in device_ids:
                    continue
                engine_id = self.engine_device_id(track.id, device.id)
                if engine_id is None or engine_id not in self._plugin_ids:
                    continue
                try:
                    device.state = base64.b64encode(self.engine.processor_state(engine_id)).decode("ascii")
                except RuntimeError as exc:
                    self.status_message.emit(f"{device.plugin.name}: {exc}")
                path = self._plugin_ids[engine_id]
                if device.plugin.path != path:  # found somewhere else: remember where
                    device.plugin = replace(device.plugin, path=path)

    def param_id(self, processor_id: int, index: int) -> str | None:
        ids = self._param_ids.get(processor_id)
        if ids is None:
            ids = self._param_ids[processor_id] = [p.id for p in self.engine.processor_params(processor_id)]
        return ids[index] if 0 <= index < len(ids) else None

    def _editor_title(self, track_id: str, device_id: str) -> str:
        device = self.project.device(track_id, device_id)
        name = device.plugin.name if device.plugin else device.kind
        return f"{name} - {self.project.track(track_id).name}"

    def open_plugin_editor(self, track_id: str, device_id: str, report: bool = True) -> bool:
        engine_id = self.engine_device_id(track_id, device_id)
        if engine_id is None:
            return False
        self._busy += 1
        try:
            opened = self.engine.open_editor(engine_id, self.owner_window(), self._editor_title(track_id, device_id))
        finally:
            self._busy -= 1
        if opened:
            self._editors_wanted.add(device_id)
        else:
            self._editors_wanted.discard(device_id)
            if report:
                self.status_message.emit(f"{self.project.device(track_id, device_id).plugin.name} has no editor.")
        self.plugin_editor_changed.emit(track_id, device_id)
        return opened

    def close_plugin_editor(self, track_id: str, device_id: str) -> None:
        self._editors_wanted.discard(device_id)
        engine_id = self.engine_device_id(track_id, device_id)
        if engine_id is not None:
            if engine_id in self._hidden_editors:
                self._hidden_editors.remove(engine_id)
            self.engine.close_editor(engine_id)
            self.plugin_editor_changed.emit(track_id, device_id)

    def request_plugin_editor(self, track_id: str, device_id: str) -> None:
        """Open a plug-in's editor now if its track is the one shown, or when it is."""
        if track_id == self._editors_track:
            self.open_plugin_editor(track_id, device_id, report=False)  # having none is fine here
        else:
            self._editors_wanted.add(device_id)

    def show_plugin_editors(self, track_id: str | None) -> None:
        """Show the editors of one track (the selected one): the ones the user left
        open there come back where they were, and every other track's are hidden
        until that track is shown again. Hidden editors keep running (they come back
        as they were), but only MAX_HIDDEN_EDITORS of them: beyond that, the ones
        hidden longest are closed, and open again (where they were) when shown."""
        if track_id == self._editors_track:
            return
        self._editors_track = track_id
        # A copy: opening an editor may run a message loop that changes the chains.
        for chain_track, chain in [(self._chain_owner.get(k), list(c)) for k, c in self._devices.items()]:
            for device_id, processor_id in chain:
                if processor_id not in self._plugin_ids:
                    continue
                if chain_track != track_id:
                    if self.engine.is_editor_open(processor_id):
                        self.engine.set_editor_visible(processor_id, False)
                        self._hidden_editors.append(processor_id)
                        self.plugin_editor_changed.emit(chain_track, device_id)
                elif device_id in self._editors_wanted and not self.engine.is_editor_open(processor_id):
                    if processor_id in self._hidden_editors:
                        self._hidden_editors.remove(processor_id)
                    if self.engine.set_editor_visible(processor_id, True):
                        self.plugin_editor_changed.emit(chain_track, device_id)
                    else:  # it was closed meanwhile (too many hidden, or its plug-in reloaded)
                        self.open_plugin_editor(chain_track, device_id, report=False)
        while len(self._hidden_editors) > MAX_HIDDEN_EDITORS:
            self.engine.close_editor(self._hidden_editors.pop(0))  # still wanted: it reopens when shown

    def is_plugin_editor_open(self, track_id: str, device_id: str) -> bool:
        engine_id = self.engine_device_id(track_id, device_id)
        return engine_id is not None and self.engine.is_editor_open(engine_id)

    def close_all_editors(self) -> None:
        self._hidden_editors.clear()
        for chain in self._devices.values():
            for _, processor_id in chain:
                if processor_id in self._plugin_ids:
                    self.engine.close_editor(processor_id)

    def _update_editor_titles(self, track_id: str) -> None:
        for key in self._owned_chains(track_id):
            for device_id, processor_id in self._devices.get(key, []):
                if processor_id in self._plugin_ids:  # hidden editors too; no-op without one
                    self.engine.set_editor_title(processor_id, self._editor_title(track_id, device_id))

    def _dispatch_processor_events(self) -> None:
        events = self.engine.take_processor_events()
        if not events:
            return
        places = {pid: (self._chain_owner[key], did) for key, chain in self._devices.items() for did, pid in chain
                  if pid is not None and key in self._chain_owner}
        changed: dict[tuple[str, str], None] = {}
        dirty = False
        kind = ge.ProcessorEventType
        for event in events:
            place = places.get(event.processor_id)
            if place is None:
                continue
            if event.type in (kind.PARAM_EDITED, kind.PARAM_TOUCHED) and not self.engine.is_editor_open(
                    event.processor_id):
                # Not the user (who edits in the editor): the plug-in itself, as some do when
                # their state is restored. Not an edit to undo; its parameters show anew.
                changed[place] = None
                continue
            if event.type == kind.PARAM_EDITED:
                param_id = self.param_id(event.processor_id, event.param_index)
                if param_id is not None:
                    self.plugin_param_edited.emit(*place, param_id, event.value, event.old_value, event.gesture)
            elif event.type == kind.PARAM_TOUCHED:
                param_id = self.param_id(event.processor_id, event.param_index)
                if param_id is not None:
                    self.plugin_param_touched.emit(*place, param_id)
            elif event.type in (kind.PARAMS_CHANGED, kind.LATENCY_CHANGED):
                changed[place] = None
            elif event.type == kind.PARAM_INFO_CHANGED:
                self._param_ids.pop(event.processor_id, None)
                self._param_infos.pop(event.processor_id, None)
                self._param_specs.pop(event.processor_id, None)
                self._push_automation(place[0])  # its parameters may be elsewhere in the list now
                self.plugin_params_rebuilt.emit(*place)
            elif event.type == kind.EDITOR_CLOSED:
                self._editors_wanted.discard(place[1])
                self.plugin_editor_changed.emit(*place)
            elif event.type == kind.EDITOR_REQUESTED:  # like any editor, shown with its track
                self.request_plugin_editor(*place)
            elif event.type == kind.STATE_DIRTY:
                dirty = True
        for place in changed:
            self.plugin_params_changed.emit(*place)
        if dirty:
            self.plugin_state_dirty.emit()

    def poll_plugins(self) -> None:
        """The engine's housekeeping, and what plug-ins reported since."""
        self.engine.idle()
        if not self._busy:  # not from a message loop inside a plug-in call
            self._dispatch_processor_events()
