"""Editing racks: grouping devices into one and ungrouping it, its chains
(adding, removing, duplicating, moving, renaming, their mixers), and its
macros (turning them, mapping them to parameters)."""

from __future__ import annotations

import copy
from collections.abc import Callable

from .. import automation
from ..automation import MAX_VOLUME_DB, MIN_VOLUME_DB
from ..commands import (
    SetDeviceParamsCommand,
    SetDevicesCommand,
    SetMacrosCommand,
    UpdateChainCommand,
)
from ..devices import (
    builtin_param_info,
    device_ids_of,
    device_is_instrument,
    device_name,
    new_chain,
    new_rack,
)
from ..project import (
    MACRO_COUNT,
    MAX_RACK_DEPTH,
    Chain,
    Device,
    MacroMapping,
    chain_devices,
    container_of,
    find_device,
    iter_chains,
    macro_param,
    rack_depth,
    rack_height,
    refresh_ids,
)


class RackEdits:
    """Racks, their chains and macros. Part of ProjectEditor (editor/__init__.py)."""

    def group_devices(self, track_id: str, device_ids) -> Device | None:
        """Ctrl+G in the device view: these devices (in one chain, in its order)
        go into a new rack, in one chain, where the first of them was. One undo
        step; the rack (None if they aren't all in one chain, or it would nest
        too deep)."""
        track = self.project.track(track_id)
        wanted = {i for i in device_ids if find_device(track.devices, i) is not None}
        containers = {container_of(track.devices, i) for i in wanted}
        if len(containers) != 1:
            return None
        chain = containers.pop()
        before = copy.deepcopy(track.devices)
        after = copy.deepcopy(before)
        devices = chain_devices(after, chain)
        grouped = [d for d in devices if d.id in wanted]
        if rack_depth(after, chain) + 1 + max(rack_height(d) for d in grouped) > MAX_RACK_DEPTH:
            return None
        rack = new_rack([new_chain(device_name(grouped[0]), grouped)])
        at = devices.index(grouped[0])
        devices[:] = [d for d in devices if d.id not in wanted]
        devices.insert(at, rack)
        self._push(SetDevicesCommand(self.project, track_id, before, after, "Group Devices"))
        return self.project.device(track_id, rack.id)

    def ungroup_rack(self, track_id: str, rack_id: str) -> bool:
        """Ctrl+Shift+G: a rack goes, and its chains' devices take its place, one
        chain after another (an instrument coming out goes first). The automation
        of its chains' faders and its macros go too. One undo step; False if it
        isn't a rack, or several instruments would come out of it (layered
        instruments: a chain has just one)."""
        track = self.project.track(track_id)
        rack = find_device(track.devices, rack_id)
        if rack is None or not rack.is_rack:
            return False
        before = copy.deepcopy(track.devices)
        after = copy.deepcopy(before)
        devices = chain_devices(after, container_of(after, rack_id))
        at = next(i for i, d in enumerate(devices) if d.id == rack_id)
        devices[at:at + 1] = [d for chain in devices[at].chains for d in chain.devices]
        if sum(1 for d in devices if device_is_instrument(d)) > 1:
            return False
        instrument = next((d for d in devices if device_is_instrument(d)), None)
        if instrument is not None and devices[0] is not instrument:
            devices.remove(instrument)
            devices.insert(0, instrument)
        self._set_devices(track_id, before, after, "Ungroup Rack")
        return True

    def add_rack_chain(self, track_id: str, rack_id: str, index: int | None = None,
                       name: str | None = None) -> Chain:
        """A new, empty chain of a rack (last, or at `index`)."""
        before = copy.deepcopy(self.project.track(track_id).devices)
        after = copy.deepcopy(before)
        rack = find_device(after, rack_id)
        if rack is None or not rack.is_rack:
            raise ValueError("chains belong to racks")
        chain = new_chain(name or f"Chain {len(rack.chains) + 1}")
        rack.chains.insert(len(rack.chains) if index is None else max(0, min(index, len(rack.chains))), chain)
        self._push(SetDevicesCommand(self.project, track_id, before, after, "Add Chain"))
        return self.project.chain(track_id, chain.id)

    def remove_rack_chains(self, track_id: str, chain_ids) -> None:
        """Delete chains of racks, with their devices (and their automation, and
        their faders'). One undo step."""
        ids = set(chain_ids)
        before = copy.deepcopy(self.project.track(track_id).devices)
        after = copy.deepcopy(before)
        for rack, _chain in list(iter_chains(after)):
            rack.chains = [c for c in rack.chains if c.id not in ids]
        if after != before:
            self._set_devices(track_id, before, after, "Delete Chain" if len(ids) == 1 else "Delete Chains")

    def duplicate_rack_chain(self, track_id: str, chain_id: str) -> Chain:
        """A copy of a chain right after it: new devices with the same settings
        (plug-ins in the state they were last saved in, as presets are)."""
        before = copy.deepcopy(self.project.track(track_id).devices)
        after = copy.deepcopy(before)
        rack = next(r for r, c in iter_chains(after) if c.id == chain_id)
        index = next(i for i, c in enumerate(rack.chains) if c.id == chain_id)
        holder = new_rack([copy.deepcopy(rack.chains[index])])
        refresh_ids(holder)
        rack.chains.insert(index + 1, holder.chains[0])
        self._push(SetDevicesCommand(self.project, track_id, before, after, "Duplicate Chain"))
        return self.project.chain(track_id, holder.chains[0].id)

    def move_rack_chain(self, track_id: str, chain_id: str, index: int) -> None:
        """Reorder a rack's chains: this one to `index` (among the others)."""
        before = copy.deepcopy(self.project.track(track_id).devices)
        after = copy.deepcopy(before)
        rack = next(r for r, c in iter_chains(after) if c.id == chain_id)
        chain = next(c for c in rack.chains if c.id == chain_id)
        rack.chains.remove(chain)
        rack.chains.insert(max(0, min(index, len(rack.chains))), chain)
        if after != before:
            self._push(SetDevicesCommand(self.project, track_id, before, after, "Move Chain"))

    def rename_chain(self, track_id: str, chain_id: str, name: str) -> None:
        old = self.project.chain(track_id, chain_id).name
        if name and name != old:
            self._push(UpdateChainCommand(self.project, track_id, chain_id, "name", old, name, "Rename Chain"))

    def set_chain_param(self, track_id: str, chain_id: str, attr: str, value, merge_key: object | None = None) -> None:
        """A rack chain's mixer: volume_db, pan, mute, solo (saved, but not undone, as a track's)."""
        if attr == "solo":
            if self.project.chain(track_id, chain_id).solo != value:
                self.project.update_chain(track_id, chain_id, solo=value)
            return
        labels = {"volume_db": "Change Chain Volume", "pan": "Change Chain Pan", "mute": "Toggle Chain Activator"}
        if attr == "volume_db":
            value = max(MIN_VOLUME_DB, min(MAX_VOLUME_DB, value))
        elif attr == "pan":
            value = max(-1.0, min(1.0, value))
        old = getattr(self.project.chain(track_id, chain_id), attr)
        if value != old:
            self._push(UpdateChainCommand(self.project, track_id, chain_id, attr, old, value, labels[attr], merge_key))
        touched = {"volume_db": automation.CHAIN_VOLUME, "pan": automation.CHAIN_PAN}.get(attr)
        if touched:
            rack = self.project.chain_rack(track_id, chain_id)
            self.parameter_touched.emit(track_id, automation.chain_key(rack.id, chain_id, touched))

    # Macros: a rack's parameters, each moving the parameters mapped to it. Their
    # values are normalized. param_info() describes a device's parameter (its
    # normalized mapping: a ParamInfo or ParamSpec); the UI hands it plug-ins'
    # (set_param_info); built-in devices' are known without it.

    def set_param_info(self, describe: Callable[[str, str, str], object | None]) -> None:
        """Where param_info() learns about parameters it doesn't know (plug-ins'):
        describe(track id, device id, param id)."""
        self._describe = describe

    def set_own_value(self, read: Callable[[str, str], float | None]) -> None:
        """Where set_macro() learns what a plug-in's parameter is now, as set in
        the plug-in's own editor (the model doesn't have it): read(owner, key)."""
        self._read_own = read

    def param_info(self, track_id: str, device_id: str, param_id: str):
        device = self.project.device(track_id, device_id)
        if not device.is_plugin and not device.is_rack:
            return builtin_param_info(device.kind, param_id)
        describe = getattr(self, "_describe", None)
        return describe(track_id, device_id, param_id) if describe is not None else None

    def macro_targets(self, track_id: str, rack_id: str, index: int, value: float) -> dict[tuple[str, str], float]:
        """What a rack's macro at `value` sets: itself, and each parameter mapped to it (plain values)."""
        rack = self.project.device(track_id, rack_id)
        value = max(0.0, min(1.0, value))
        values = {(rack_id, macro_param(index)): value}
        for mapping in rack.macros:
            if mapping.macro != index or not self.project.has_device(track_id, mapping.device_id):
                continue
            info = self.param_info(track_id, mapping.device_id, mapping.param_id)
            if info is not None:
                values[(mapping.device_id, mapping.param_id)] = info.from_normalized(mapping.target(value))
        return values

    def set_macro(self, track_id: str, rack_id: str, index: int, value: float, merge_key: object | None = None) -> None:
        """Turn a rack's macro: it and every parameter mapped to it, one undo step
        (one per gesture, with a `merge_key`)."""
        new = self.macro_targets(track_id, rack_id, index, value)
        old = {}
        for device_id, param_id in new:
            own = self.project.device(track_id, device_id).params.get(param_id)
            read = getattr(self, "_read_own", None)
            if own is None and read is not None:  # set in a plug-in's own editor
                own = read(track_id, automation.device_key(device_id, param_id))
            if own is None:  # a default value: as it was
                info = self.param_info(track_id, device_id, param_id)
                own = new[(device_id, param_id)] if info is None else getattr(info, "default_value",
                                                                              getattr(info, "default", 0.0))
            old[(device_id, param_id)] = own
        if old != new:
            self._push(SetDeviceParamsCommand(self.project, track_id, old, new, "Change Macro", merge_key))

    def map_macro(self, track_id: str, rack_id: str, index: int, device_id: str, param_id: str,
                  low: float = 0.0, high: float = 1.0) -> None:
        """Map a rack's macro to a parameter of a device in it (a parameter is
        mapped to one macro of the rack at a time)."""
        rack = self.project.device(track_id, rack_id)
        if not rack.is_rack or not 0 <= index < MACRO_COUNT or device_id == rack_id \
                or device_id not in device_ids_of(rack):
            raise ValueError("a macro moves parameters of devices in its rack")
        kept = tuple(m for m in rack.macros if (m.device_id, m.param_id) != (device_id, param_id))
        new = (*kept, MacroMapping(index, device_id, param_id, low, high))
        self._push(SetMacrosCommand(self.project, track_id, rack_id, rack.macros, new, "Map Macro"))

    def unmap_macro(self, track_id: str, rack_id: str, device_id: str, param_id: str) -> None:
        rack = self.project.device(track_id, rack_id)
        kept = tuple(m for m in rack.macros if (m.device_id, m.param_id) != (device_id, param_id))
        if kept != rack.macros:
            self._push(SetMacrosCommand(self.project, track_id, rack_id, rack.macros, kept, "Remove Macro Mapping"))

    def macro_of(self, track_id: str, device_id: str, param_id: str) -> tuple[str, int] | None:
        """The rack and macro a parameter is mapped to (the nearest rack's), if any."""
        devices = self.project.track(track_id).devices
        chain = container_of(devices, device_id)
        while chain is not None:
            rack = self.project.chain_rack(track_id, chain)
            for mapping in rack.macros:
                if (mapping.device_id, mapping.param_id) == (device_id, param_id):
                    return rack.id, mapping.macro
            chain = container_of(devices, rack.id)
        return None
