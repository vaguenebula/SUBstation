"""Loading a project's plug-ins after it opens: the project shows (and plays,
without them) at once, and its plug-ins load one at a time, each in a turn of
the event loop of its own, a little apart, so the window goes on between them.

They load on the UI thread all the same: plug-ins must be created and set up
there (VST3 says so, and JUCE plug-ins take the thread that creates them for
their message thread), and some hang when loaded on another. What takes long
is each plug-in, not the project around it."""

from __future__ import annotations

PLUGIN_GAP_MS = 20  # between two plug-ins: the window's turn (it paints, takes the mouse and keys)
PLUGIN_RETRY_MS = 50  # a plug-in's call is running a message loop: the next one loads after it


class PluginLoader:
    """Plug-ins waiting to load (a project just opened). Part of EngineBridge (engine_bridge/__init__.py)."""

    # Devices whose plug-in waits (in the order they load) are in the engine
    # with no processor, as a plug-in that isn't installed is (None in their
    # chains), until their turn: they are then new to their chain (wherever
    # they are by then) and get their processor as a device added does.

    def _defer_plugin(self, device_id: str) -> None:
        self._pending_plugins[device_id] = None
        self._plugins_total += 1

    def _start_loading_plugins(self) -> None:
        if self._pending_plugins:
            self.plugins_loading.emit(0, self._plugins_total)
            self._plugin_timer.start(0)

    def _stop_loading_plugins(self) -> None:
        self._plugin_timer.stop()
        self._pending_plugins.clear()
        self._plugins_total = 0

    def plugin_pending(self, device_id: str) -> bool:
        """Whether a device's plug-in is still waiting to load."""
        return device_id in self._pending_plugins

    @property
    def plugins_pending(self) -> int:
        """How many plug-ins are still waiting to load."""
        return len(self._pending_plugins)

    def prioritize_plugins(self, track_id: str | None) -> None:
        """A track's plug-ins load next (the one shown in the device view)."""
        first = [d for d in self._pending_plugins if self._chain_owner.get(self._where.get(d, "")) == track_id]
        if first:
            self._pending_plugins = dict.fromkeys(first + [d for d in self._pending_plugins if d not in first])

    def load_plugin_now(self, device_id: str) -> None:
        """A waiting plug-in loads now (its editor is asked for)."""
        if self._pending_plugins.pop(device_id, 0) is None:
            self._load_deferred(device_id)
            self._report_plugins()

    def load_pending_plugins(self) -> None:
        """Every waiting plug-in loads now (before rendering offline)."""
        for device_id in list(self._pending_plugins):
            if self._pending_plugins.pop(device_id, 0) is None:
                self._load_deferred(device_id)
        self._report_plugins()

    def _load_next_plugin(self) -> None:
        """The timer's: the next plug-in that waits loads (one a turn)."""
        if self._busy or self._syncing:  # (inside a plug-in's message loop, or a chain being synced)
            self._plugin_timer.start(PLUGIN_RETRY_MS)
            return
        while self._pending_plugins:
            device_id = next(iter(self._pending_plugins))
            del self._pending_plugins[device_id]
            if self._load_deferred(device_id):
                break
        self._report_plugins()
        if self._pending_plugins:
            self._plugin_timer.start(PLUGIN_GAP_MS)

    def _report_plugins(self) -> None:
        if not self._plugins_total:
            return
        if self._pending_plugins:
            self.plugins_loading.emit(self._plugins_total - len(self._pending_plugins), self._plugins_total)
        else:
            self._stop_loading_plugins()
            self.plugins_loading.emit(0, 0)  # all done

    def _load_deferred(self, device_id: str) -> bool:
        """A waiting plug-in gets its processor where its device is now. False if
        there was nothing to load: it went (or came back loaded: undo)."""
        key = self._where.get(device_id)
        if key is None or self._pids.get(device_id, 0) is not None:
            return False
        track_id = self._chain_owner.get(key)
        if track_id is None or track_id not in self._chains:
            return False
        if track_id in self._syncing:  # (its chain is being synced: it waits a little longer)
            self._pending_plugins[device_id] = None
            return False
        # New to its chain: the sync gives it its processor, in its place, with its automation and sidechain.
        self._devices[key] = [(d, p) for d, p in self._devices.get(key, []) if d != device_id]
        del self._where[device_id]
        self._pids.pop(device_id, None)
        self._sync_devices(track_id)
        return True
