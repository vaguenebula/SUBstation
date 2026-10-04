"""Model -> engine for devices: the processors of every chain (a track's own,
and racks' chains), kept as devices move between chains; racks and their chains'
mixers; sidechains; parameters, states and on/off switches."""

from __future__ import annotations

import base64
import os

from PySide6.QtCore import QObject, QRunnable, Signal

from ... import _engine as ge
from ...model import automation
from ...model.automation import MASTER
from ...model.devices import device_is_instrument, device_name
from ...model.project import (
    POST_FADER,
    PRE_FADER,
    PRE_FX,
    Device,
    PluginRef,
    Track,
    iter_chains,
    iter_devices,
)
from ...model.timebase import db_to_gain


class _StateSignals(QObject):
    failed = Signal(str)  # a device's state couldn't be restored: why


class _StateTask(QRunnable):
    """Restores a built-in device's state off the UI thread: it may load files
    (a sampler's sample)."""

    def __init__(self, engine: ge.Engine, processor_id: int, name: str, state: bytes, signals: _StateSignals):
        super().__init__()
        self.engine = engine
        self.processor_id = processor_id
        self.name = name
        self.state = state
        self.signals = signals

    def run(self) -> None:
        try:
            self.engine.set_processor_state(self.processor_id, self.state)
        except ValueError:
            pass  # the device went meanwhile
        except RuntimeError as exc:
            self.signals.failed.emit(f"{self.name}: {exc}")


class DeviceSync:
    """Devices' processors, racks, sidechains, parameters and states. Part of EngineBridge (engine_bridge/__init__.py)."""

    # The engine's chains by key: a track's own chain by the track's id, a rack's
    # chain by the chain's (ids are unique in the project). Each device's
    # processor is in the chain the bridge last put it in (`_where`).
    @staticmethod
    def _loaded_devices(track: Track) -> list[Device]:
        """The devices of a track the engine has: none while it is frozen."""
        return [] if track.frozen is not None else track.devices

    def _model_chains(self) -> dict[str, tuple[str, list[Device]]]:
        """Every chain in the project the engine has, by key: (its track, its devices)."""
        chains = {}
        for track in self.project.all_tracks():
            devices = self._loaded_devices(track)
            chains[track.id] = (track.id, devices)
            for _rack, chain in iter_chains(devices):
                chains[chain.id] = (track.id, chain.devices)
        return chains

    def _owned_chains(self, track_id: str) -> list[str]:
        return [key for key, owner in self._chain_owner.items() if owner == track_id]

    def _sync_devices(self, track_id: str) -> None:
        """A track's devices (in racks too) to the engine. Devices still there keep
        their processors (a plug-in keeps its state and editor), and so do devices
        that moved here from another chain (of this track or another, into or out
        of a rack), or a rack with everything in it; new ones get one; the rest go."""
        if track_id not in self._chains or track_id in self._syncing:
            return
        self._syncing.add(track_id)
        changed: set[str] = set()  # chain keys whose devices changed
        try:
            track = self.project.track(track_id)
            devices = self._loaded_devices(track)
            before = {key: list(self._devices.get(key, [])) for key in self._owned_chains(track_id)}
            self._place(track_id, track_id, devices, changed)  # every chain, top down
            model = self._model_chains()
            for key, entries in before.items():  # what left its chain, and isn't in another of this track's
                wanted = {d.id for d in model[key][1]} if key in model else set()
                for device_id, processor_id in entries:
                    if device_id not in wanted and self._where.get(device_id) == key:
                        self._dispose(track_id, key, device_id, processor_id)
                        changed.add(key)
            for key in self._owned_chains(track_id):  # rack chains that went (their racks stay)
                if key not in model and key != track_id:
                    try:
                        self.engine.remove_rack_chain(self._chains[key])
                    except ValueError:
                        pass
                    self._drop_chain(key)
            for key in [track_id, *(c.id for _, c in iter_chains(devices))]:
                ids = [d for d, _ in self._devices.get(key, [])]
                if key in changed or ids != [d for d, _ in before.get(key, [])]:
                    self.engine.set_chain_order(self._chains[key], [p for _, p in self._devices[key] if p is not None])
                    changed.add(key)
            for rack in iter_devices(devices):
                processor_id = self._pids.get(rack.id)
                if rack.is_rack and processor_id is not None:
                    order = [self._chains[c.id] for c in rack.chains]
                    if self._rack_orders.get(rack.id) != order:
                        self.engine.set_rack_chain_order(processor_id, order)
                        self._rack_orders[rack.id] = order
        finally:
            self._syncing.discard(track_id)
        if changed:
            self._push_automation(track_id)  # its devices' envelopes go to the new processors
            self._update_editor_titles(track_id)
            self.devices_loaded.emit(track_id)
        self._push_enabled(track)
        self._push_chain_mixers(track_id)
        self._push_sidechains()  # (the new processors', or a sidechain changed)

    def _place(self, track_id: str, key: str, devices: list[Device], changed: set[str]) -> None:
        """A chain's devices into its engine chain (in no particular order yet),
        then the chains of the racks among them."""
        current = {d: p for d, p in self._devices.get(key, []) if self._where.get(d) == key}
        chain = []
        for device in devices:
            if device.id in current:
                processor_id = current[device.id]
            else:
                found, processor_id = self._take_over(track_id, key, device)
                if not found:
                    processor_id = self._create_processor(self._chains[key], device)
                    self._pids[device.id] = processor_id
                self._where[device.id] = key
                changed.add(key)
            chain.append((device.id, processor_id))
        self._devices[key] = chain
        for device, (_, processor_id) in zip(devices, chain, strict=True):
            if device.is_rack and processor_id is not None:
                self._place_rack(track_id, device, processor_id, changed)

    def _place_rack(self, track_id: str, rack: Device, processor_id: int, changed: set[str]) -> None:
        for chain in rack.chains:
            engine_chain = self._chains.get(chain.id)
            if engine_chain is not None and self._rack_of_chain.get(chain.id) != rack.id:
                # The chain is another rack's now: a new engine chain, with its devices.
                moved = self.engine.add_rack_chain(processor_id)
                for _, inner in self._devices.get(chain.id, []):
                    if inner is not None:
                        self.engine.move_processor(inner, moved)
                try:
                    self.engine.remove_rack_chain(engine_chain)
                except ValueError:
                    pass
                engine_chain = moved
            elif engine_chain is None:
                engine_chain = self.engine.add_rack_chain(processor_id)
                self._devices[chain.id] = []
                changed.add(chain.id)
            self._chains[chain.id] = engine_chain
            self._chain_owner[chain.id] = track_id
            self._rack_of_chain[chain.id] = rack.id
            self._place(track_id, chain.id, chain.devices, changed)

    def _take_over(self, track_id: str, key: str, device: Device) -> tuple[bool, int | None]:
        """A device new to a chain: whether its processor is in another chain (of
        any track: it moved here), and which then; it moves along in the engine
        (a rack with its chains and everything in them)."""
        old = self._where.get(device.id)
        if old is None or old == key or device.id not in self._pids:
            return False, None
        processor_id = self._pids[device.id]
        self._devices[old] = [(d, p) for d, p in self._devices.get(old, []) if d != device.id]
        if processor_id is not None:
            if self._chain_owner.get(old) != track_id:  # its sidechains come back once there (if they can)
                for inner in iter_devices([device]):
                    if self._pids.get(inner.id) is not None:
                        self._drop_sidechain(self._pids[inner.id])
            self.engine.move_processor(processor_id, self._chains[key])
        for _rack, chain in iter_chains([device]):
            if chain.id in self._chain_owner:
                self._chain_owner[chain.id] = track_id
        return True, processor_id

    def _dispose(self, track_id: str, key: str, device_id: str, processor_id: int | None) -> None:
        """A device left a chain of this track and isn't on it any more: if it went
        to another track, that track takes it over now; otherwise it goes (and a
        rack with what is in it, but what of that went elsewhere)."""
        owner = self.project.device_owner(device_id)
        if owner is not None and owner != track_id and owner in self._chains and owner not in self._syncing:
            self._sync_devices(owner)
            if self._where.get(device_id) != key:
                return
        for chain in self._chains_of(device_id):
            for inner, inner_id in list(self._devices.get(chain, [])):
                if self._where.get(inner) == chain:
                    self._dispose(track_id, chain, inner, inner_id)
        self._forget_processor(device_id, processor_id)

    def _chains_of(self, rack_id: str) -> list[str]:
        return [chain for chain, rack in self._rack_of_chain.items() if rack == rack_id]

    def _drop_chain(self, key: str) -> None:
        for mapping in (self._chains, self._devices, self._chain_owner, self._rack_of_chain, self._chain_mixer,
                        self.chain_meters):
            mapping.pop(key, None)

    def has_sidechain_input(self, track_id: str, device_id: str) -> bool:
        """Whether a device has a sidechain (aux) input (not while its plug-in isn't loaded)."""
        processor_id = self.engine_device_id(track_id, device_id)
        return processor_id is not None and self.engine.processor_info(processor_id).has_sidechain

    def _wanted_sidechain(self, device: Device, processor_id: int) -> tuple[int, ge.SidechainTap, int] | None:
        """The sidechain the engine should give a device's processor (None: none, or
        one from a track the engine hasn't yet)."""
        sidechain = device.sidechain
        if sidechain is None or sidechain.track_id == MASTER:
            return None
        source = self._track_ids.get(sidechain.track_id)
        if source is None or not self.engine.processor_info(processor_id).has_sidechain:
            return None
        if sidechain.tap == POST_FADER:
            return source, ge.SidechainTap.POST_FADER, 0
        if sidechain.tap == PRE_FX:
            # A MIDI track's own audio is its instrument's (or its instrument rack's): before its effects.
            devices = self.project.track(sidechain.track_id).devices
            if devices and device_is_instrument(devices[0]):
                instrument = self.engine_device_id(sidechain.track_id, devices[0].id)
                if instrument is not None:
                    return source, ge.SidechainTap.AFTER_DEVICE, instrument
            return source, ge.SidechainTap.PRE_FX, 0
        tapped = None if sidechain.tap == PRE_FADER else self.engine_device_id(sidechain.track_id, sidechain.tap)
        if tapped is None:  # before the fader (also while the device it is taken after isn't on the source)
            return source, ge.SidechainTap.PRE_FADER, 0
        return source, ge.SidechainTap.AFTER_DEVICE, tapped

    def _push_sidechains(self) -> None:
        """Every device's sidechain to the engine (devices in racks too): those
        changing go first, so that no step closes a cycle."""
        wanted = {}
        for track in self.project.all_tracks():
            for device in iter_devices(track.devices):
                processor_id = self.engine_device_id(track.id, device.id) if device.sidechain is not None else None
                if processor_id is not None:
                    state = self._wanted_sidechain(device, processor_id)
                    if state is not None:
                        wanted[processor_id] = state
        for processor_id in [p for p, state in self._sidechains.items() if wanted.get(p) != state]:
            self._drop_sidechain(processor_id)
        for processor_id, state in wanted.items():
            if processor_id in self._sidechains:
                continue
            try:
                self.engine.set_processor_sidechain(processor_id, *state)
            except ValueError:
                continue  # a cycle with a route another change hasn't undone yet: it comes with that change
            self._sidechains[processor_id] = state

    def _drop_sidechain(self, processor_id: int) -> None:
        if self._sidechains.pop(processor_id, None) is not None:
            self.engine.clear_processor_sidechain(processor_id)

    def _push_enabled(self, track: Track) -> None:
        for device in iter_devices(track.devices):
            processor_id = self.engine_device_id(track.id, device.id)
            if processor_id is not None and self._enabled.get(processor_id) != device.enabled:
                self.engine.set_processor_enabled(processor_id, device.enabled)
                self._enabled[processor_id] = device.enabled

    def _on_chain_changed(self, track_id: str, chain_id: str) -> None:
        chain = self.project.chain(track_id, chain_id)
        old = self._chain_mixer.get(chain_id)
        if old is not None:  # changed by hand while automated: its automation stops
            rack = self.project.chain_rack(track_id, chain_id)
            if chain.volume_db != old[0]:
                self.override_automation(track_id, automation.chain_key(rack.id, chain_id, automation.CHAIN_VOLUME))
            if chain.pan != old[1]:
                self.override_automation(track_id, automation.chain_key(rack.id, chain_id, automation.CHAIN_PAN))
        self._push_chain_mixers(track_id)

    def _push_chain_mixers(self, track_id: str) -> None:
        """A track's rack chains' faders to the engine (those that changed)."""
        if not self.project.has_owner(track_id):
            return
        for _rack, chain in iter_chains(self.project.track(track_id).devices):
            engine_chain = self._chains.get(chain.id)
            state = (chain.volume_db, chain.pan, chain.mute, chain.solo)
            old = self._chain_mixer.get(chain.id)
            if engine_chain is None or old == state:
                continue
            if old is None or old[0] != state[0]:
                self.engine.set_chain_gain(engine_chain, db_to_gain(chain.volume_db))
            if old is None or old[1] != state[1]:
                self.engine.set_chain_pan(engine_chain, chain.pan)
            if old is None or old[2] != state[2]:
                self.engine.set_chain_mute(engine_chain, chain.mute)
            if old is None or old[3] != state[3]:
                self.engine.set_chain_solo(engine_chain, chain.solo)
            self._chain_mixer[chain.id] = state

    def _create_processor(self, chain_id: int, device: Device) -> int | None:
        if device.is_rack:  # (its chains come next: _place_rack)
            try:
                return self.engine.add_rack(chain_id)
            except ValueError as exc:  # nested too deep (a file edited by hand)
                self.status_message.emit(str(exc))
                return None
        if device.is_plugin:
            return self._load_plugin(chain_id, device)
        try:
            processor_id = self.engine.add_builtin_processor(chain_id, device.kind)
        except (RuntimeError, ValueError):  # a device of a newer version (a preset, a project): missing, as a plug-in
            self._plugin_failed(device, f"{device_name(device)} is not a device this version of SUBstation has.")
            return None
        for param_id, value in device.params.items():
            self._set_param(processor_id, param_id, value)
        if device.state:
            self._set_builtin_state(processor_id, device)
        return processor_id

    def plugin_path(self, plugin: PluginRef) -> str | None:
        """Where a plug-in is now: where it was, or where the scan found it."""
        if plugin.path and os.path.exists(plugin.path):
            return plugin.path
        return self.known_plugins.get(plugin.uid)

    def _load_plugin(self, chain_id: int, device: Device) -> int | None:
        plugin = device.plugin
        path = self.plugin_path(plugin)
        if path is None:
            self._plugin_failed(device, f"{plugin.name} is not installed.")
            return None
        self._busy += 1
        try:
            processor_id = self.engine.add_plugin_processor(chain_id, plugin.format, path, plugin.uid)
        except (RuntimeError, ValueError) as exc:
            self._plugin_failed(device, f"{plugin.name} could not be loaded: {exc}")
            return None
        finally:
            self._busy -= 1
        self._plugin_ids[processor_id] = path
        self.plugin_errors.pop(device.id, None)
        # Its state as it was when the device went away (undo), else as saved.
        state = self._plugin_states.pop(device.id, None)
        if state is None and device.state:
            try:
                state = base64.b64decode(device.state)
            except ValueError:
                state = None
        if state:
            self._set_plugin_state(processor_id, plugin.name, state)
        return processor_id

    def _plugin_failed(self, device: Device, message: str) -> None:
        self.plugin_errors[device.id] = message
        self.status_message.emit(message)

    def _set_plugin_state(self, processor_id: int, name: str, state: bytes) -> None:
        self._busy += 1
        try:
            self.engine.set_processor_state(processor_id, state)
        except (RuntimeError, ValueError) as exc:
            self.status_message.emit(f"{name}: its settings could not be restored ({exc})")
        finally:
            self._busy -= 1

    def _forget_processor(self, device_id: str, processor_id: int | None, remove: bool = True) -> None:
        """A device's processor goes away (with its track if not `remove`; a rack
        with its chains and what is still in them). A plug-in's state is kept, in
        case the device comes back (undo), but not its editor: undo and redo
        don't open editors."""
        for chain in self._chains_of(device_id):  # (the engine removes them with the rack)
            for inner, inner_id in self._devices.get(chain, []):
                if self._where.get(inner) == chain:
                    self._forget_processor(inner, inner_id, remove=False)
            self._drop_chain(chain)
        self._rack_orders.pop(device_id, None)
        self._pids.pop(device_id, None)
        self._where.pop(device_id, None)
        self.plugin_errors.pop(device_id, None)
        self._editors_wanted.discard(device_id)
        if processor_id is None:
            return
        if processor_id in self._hidden_editors:
            self._hidden_editors.remove(processor_id)
        if processor_id in self._plugin_ids:
            try:
                self._plugin_states[device_id] = self.engine.processor_state(processor_id)
            except RuntimeError:
                pass
            del self._plugin_ids[processor_id]
        self._enabled.pop(processor_id, None)
        self._sidechains.pop(processor_id, None)
        self._param_ids.pop(processor_id, None)
        self._param_infos.pop(processor_id, None)
        self._param_specs.pop(processor_id, None)
        if remove:
            self.engine.remove_processor(processor_id)

    def _forget_chain_devices(self, key: str, remove: bool) -> None:
        """Every device of a chain goes (a track's own chain: with its track if not `remove`)."""
        for device_id, processor_id in list(self._devices.get(key, [])):
            if self._where.get(device_id) == key:
                self._forget_processor(device_id, processor_id, remove)
        self._devices.pop(key, None)

    def engine_device_id(self, track_id: str, device_id: str) -> int | None:
        """A device's processor, if it is on that track (in a rack too) and loaded."""
        key = self._where.get(device_id)
        if key is None or self._chain_owner.get(key) != track_id:
            return None
        return self._pids.get(device_id)

    def engine_chain_id(self, chain_id: str) -> int | None:
        """A rack chain's engine chain."""
        return self._chains.get(chain_id) if chain_id in self._rack_of_chain else None

    def device_param_info(self, track_id: str, device_id: str, param_id: str):
        """A device's parameter as the engine describes it (ParamInfo; None: not loaded, or no such one)."""
        processor_id = self.engine_device_id(track_id, device_id)
        if processor_id is None:
            return None
        return next((p for p in self.param_infos(processor_id) if p.id == param_id), None)

    def _on_device_param_changed(self, track_id: str, device_id: str, param_id: str) -> None:
        self.override_automation(track_id, automation.device_key(device_id, param_id))
        self._push_device_param(track_id, device_id, param_id)

    def _push_device_param(self, track_id: str, device_id: str, param_id: str) -> None:
        engine_id = self.engine_device_id(track_id, device_id)
        value = self.project.device(track_id, device_id).params.get(param_id)
        if engine_id is None or value is None:
            return
        if engine_id in self._plugin_ids:
            index = self.engine.processor_param_index(engine_id, param_id)
            if index < 0 or self.engine.processor_param(engine_id, index) == value:
                return  # an edit made in the plug-in's own editor: it has the value already
            self.engine.set_processor_param(engine_id, index, value)
            return
        self._set_param(engine_id, param_id, value)

    def _set_param(self, processor_id: int, param_id: str, value: float) -> None:
        index = self.engine.processor_param_index(processor_id, param_id)
        if index >= 0:
            self.engine.set_processor_param(processor_id, index, value)

    def _push_device_state(self, track_id: str, device_id: str) -> None:
        engine_id = self.engine_device_id(track_id, device_id)
        device = self.project.device(track_id, device_id)
        if engine_id is None:
            return
        if not device.is_plugin:
            self._set_builtin_state(engine_id, device)
        elif device.state:
            self._set_plugin_state(engine_id, device.plugin.name, base64.b64decode(device.state))
            self._refresh_plugin_own_values(track_id, device_id, engine_id)

    def _set_builtin_state(self, processor_id: int, device: Device) -> None:
        """A built-in device's state is the model's (none: its defaults); it is
        restored in the background, as it may load files."""
        try:
            state = base64.b64decode(device.state) if device.state else b""
        except ValueError:
            state = b""
        self._state_pool.start(_StateTask(self.engine, processor_id, device_name(device), state, self._state_signals))

    def wait_for_device_states(self) -> None:
        """Until every built-in device's state is restored (before rendering offline)."""
        self._state_pool.waitForDone()
