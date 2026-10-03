"""Editing device chains: adding, inserting, copying, pasting, moving and
removing devices, on a track or in a rack's chain."""

from __future__ import annotations

import copy
from collections.abc import Callable

from .. import automation
from ..commands import (
    SetChainsCommand,
    SetDevicesCommand,
    SetEnvelopeCommand,
    SetEnvelopesCommand,
)
from ..devices import (
    device_ids_of,
    device_ids_of_list,
    device_is_instrument,
    device_name,
    new_device,
)
from ..project import (
    MAX_RACK_DEPTH,
    Device,
    PluginRef,
    chain_devices,
    container_of,
    iter_chains,
    iter_devices,
    rack_depth,
    rack_height,
    refresh_ids,
)


class DeviceChainEdits:
    """Devices in chains. Part of ProjectEditor (editor/__init__.py)."""

    def set_device_defaults(self, default: Callable[[str, PluginRef | None], Device | None]) -> None:
        """Where new devices come from: default(kind, plugin) is a new device as
        the user's default preset for that kind has it, or None (as it comes;
        presets.default_device). Without it, devices start as they come."""
        self._default_device = default

    def _new_device(self, kind: str, plugin: PluginRef | None = None) -> Device:
        """A new device of a kind: as its default preset has it, if there is one."""
        default = getattr(self, "_default_device", None)
        device = default(kind, plugin) if default is not None else None
        return device if device is not None else new_device(kind, plugin)

    def add_device(self, track_id: str, kind: str, index: int | None = None,
                   plugin: PluginRef | None = None, chain: str | None = None) -> Device | None:
        """Add a device (a built-in `kind`, kind 'plugin' and a `plugin`, or an
        empty rack) to a track's (or the master's) chain, or to a rack's chain on
        it (`chain`: its id), as its default preset has it if there is one. An
        instrument only goes on a MIDI track (None otherwise), where it comes
        first in its chain and replaces any other instrument there."""
        device = self._new_device(kind, plugin)
        return device if self.insert_device(track_id, device, index, chain, f"Add {device_name(device)}") else None

    def insert_device(self, track_id: str, device: Device, index: int | None = None, chain: str | None = None,
                      text: str | None = None, show_editors: bool = True) -> bool:
        """Put a new device (a whole rack too: a preset) into a chain, as add_device
        does. One undo step; False if it can't go there."""
        return bool(self.insert_devices(track_id, [device], index, chain, text or f"Add {device_name(device)}",
                                        show_editors))

    def insert_devices(self, track_id: str, devices: list[Device], index: int | None = None,
                       chain: str | None = None, text: str = "Add Devices", show_editors: bool = True) -> list[Device]:
        """Put new devices (racks too) into a chain, in their order, before the
        device at `index` (None: last), as add_device does: an instrument only on
        a MIDI track, first, replacing the one there. One undo step; the devices
        that went in (none: nothing changed). Their plug-ins' editors show if
        `show_editors`."""
        track = self.project.track(track_id)
        before = copy.deepcopy(track.devices)
        after = copy.deepcopy(before)
        target = chain_devices(after, chain)
        if target is None:
            return []
        depth = rack_depth(after, chain)
        first = 1 if target and device_is_instrument(target[0]) else 0  # effects go after the instrument
        at = len(target) if index is None else max(first, min(index, len(target)))
        added = []
        for device in devices:
            if depth + rack_height(device) > MAX_RACK_DEPTH:
                continue
            if device_is_instrument(device):
                if not track.is_midi:
                    continue
                kept = [d for d in target if not device_is_instrument(d)]
                at += len(kept) - len(target) + 1  # (one may have gone from before it; this one goes first)
                target[:] = [device, *kept]
            else:
                first = 1 if target and device_is_instrument(target[0]) else 0
                at = max(first, at)
                target.insert(at, device)
                at += 1
            added.append(device)
        if not added:
            return []
        self._set_devices(track_id, before, after, text)
        if show_editors:
            for added_device in iter_devices(added):
                if added_device.is_plugin:
                    self.plugin_added.emit(track_id, added_device.id)
        return added

    def copy_devices(self, track_id: str, device_ids) -> list[Device]:
        """Copies of devices (racks with everything in them; those in a selected
        rack go with it), in their order on the track, for pasting: plug-ins in the
        state last stored in the model (store their states first)."""
        wanted = set(device_ids)
        found = [d for d in iter_devices(self.project.track(track_id).devices) if d.id in wanted]
        inside = {i for d in found for i in device_ids_of(d) if i != d.id}
        return [copy.deepcopy(d) for d in found if d.id not in inside]

    def paste_devices(self, track_id: str, copied: list[Device], index: int | None = None,
                      chain: str | None = None, folded=frozenset(), text: str | None = None) -> list[Device]:
        """New devices like the copied ones (copy_devices) into a chain of a track,
        before the device at `index` (None: last), as insert_devices puts them.
        Their sidechains stay, unless the source is gone or would close a cycle
        here. Those whose originals are `folded` are folded too. One undo step;
        the devices pasted."""
        devices = []
        for original in copied:
            device = copy.deepcopy(original)
            old_ids = [d.id for d in iter_devices([device])]
            refresh_ids(device)
            for old_id, inner in zip(old_ids, iter_devices([device]), strict=True):
                if old_id in folded:
                    self.project.folded_devices.add(inner.id)
                if inner.sidechain is not None and (not self.project.has_owner(inner.sidechain.track_id)
                                                    or self.project.sidechain_would_cycle(
                                                        track_id, inner.sidechain.track_id)):
                    inner.sidechain = None
            devices.append(device)
        if text is None:
            text = f"Paste {device_name(devices[0])}" if len(devices) == 1 else "Paste Devices"
        return self.insert_devices(track_id, devices, index, chain, text, show_editors=False)

    def set_devices_folded(self, track_id: str, device_ids, folded: bool) -> None:
        """Fold or unfold devices in the device view. View state: saved, not undone."""
        self.project.set_devices_folded(track_id, device_ids, folded)

    def move_device(self, track_id: str, device_id: str, index: int) -> None:
        """Move a device to position `index` in its chain (an instrument stays first)."""
        devices = self.project.track(track_id).devices
        chain = container_of(devices, device_id)
        ids = [d.id for d in chain_devices(devices, chain)]
        # move_devices counts positions in the chain before the move: moving right skips the device itself.
        self.move_devices(track_id, [device_id], index + 1 if index > ids.index(device_id) else index, chain)

    @staticmethod
    def _prune_macros(devices: list[Device]) -> None:
        """Drop the macro mappings whose device isn't (any longer) inside its rack:
        a macro moves parameters of devices in its rack."""
        for rack in iter_devices(devices):
            if not rack.macros:
                continue
            inside = device_ids_of(rack) - {rack.id}
            kept = tuple(m for m in rack.macros if m.device_id in inside)
            if kept != rack.macros:
                rack.macros = kept

    @staticmethod
    def _outermost(devices: list[Device], device_ids) -> list[Device]:
        """The devices of these ids (not instruments), in their order on the track,
        but those in racks among them (they go along with their rack)."""
        wanted = set(device_ids)
        found = [d for d in iter_devices(devices) if d.id in wanted and not device_is_instrument(d)]
        inside = {i for d in found for i in device_ids_of(d) if i != d.id}
        return [d for d in found if d.id not in inside]

    def move_devices(self, track_id: str, device_ids, index: int, chain: str | None = None) -> bool:
        """Move devices together (in their order on the track) to before the
        device at `index` in a chain of the track as it is now (the end if past
        it): its own (`chain` None) or a rack's. One undo step; an instrument
        doesn't move, and nothing goes before one; a rack doesn't go into itself,
        nor nest too deep. False if nothing moved."""
        before = copy.deepcopy(self.project.track(track_id).devices)
        after = copy.deepcopy(before)
        target = chain_devices(after, chain)
        moving = self._outermost(after, device_ids)
        if not moving or target is None:
            return False
        inside = set().union(*(device_ids_of(m) for m in moving))
        if chain is not None and self.project.chain_rack(track_id, chain).id in inside:
            return False  # into itself
        depth = rack_depth(after, chain)
        if any(depth + rack_height(d) > MAX_RACK_DEPTH for d in moving):
            return False
        at = sum(1 for d in target[:max(0, index)] if d.id not in inside)
        for device in moving:  # out of wherever they are
            chain_devices(after, container_of(after, device.id)).remove(device)
        first = 1 if target and device_is_instrument(target[0]) else 0
        at = max(first, min(at, len(target)))
        target[at:at] = moving
        self._prune_macros(after)  # (a device out of its rack leaves its macros)
        if after == before:
            return False
        self._push(SetDevicesCommand(self.project, track_id, before, after,
                                     "Move Device" if len(moving) == 1 else "Move Devices"))
        return True

    def move_devices_to_track(self, track_id: str, device_ids, to_track_id: str, index: int | None = None,
                              chain: str | None = None) -> bool:
        """Move effects (racks too, with everything in them; in their order on the
        track) to another track's (or the master's) chain, or a rack's chain there
        (`chain`), before the device at `index` (None: last; never before its
        instrument). They stay the same devices, so plug-ins keep their state, and
        their automation goes with them, and their sidechains (unless one would
        close a cycle there). One undo step; False if nothing moved."""
        if to_track_id == track_id:
            if index is None and chain is None:
                return False
            target = chain_devices(self.project.track(track_id).devices, chain) or []
            return self.move_devices(track_id, device_ids, len(target) if index is None else index, chain)
        source = copy.deepcopy(self.project.track(track_id).devices)
        target_devices = copy.deepcopy(self.project.track(to_track_id).devices)
        moving = self._outermost(source, device_ids)
        target = chain_devices(target_devices, chain)
        if not moving or target is None:
            return False
        if any(rack_depth(target_devices, chain) + rack_height(d) > MAX_RACK_DEPTH for d in moving):
            return False
        for device in moving:
            chain_devices(source, container_of(source, device.id)).remove(device)
            for inner in iter_devices([device]):  # a sidechain from where they go (or what that feeds) would close a cycle
                if inner.sidechain is not None and self.project.sidechain_would_cycle(to_track_id,
                                                                                     inner.sidechain.track_id):
                    inner.sidechain = None
        first = 1 if target and device_is_instrument(target[0]) else 0
        at = len(target) if index is None else max(first, min(index, len(target)))
        target[at:at] = moving
        self._prune_macros(source)
        self._prune_macros(target_devices)
        before = {track_id: copy.deepcopy(self.project.track(track_id).devices),
                  to_track_id: copy.deepcopy(self.project.track(to_track_id).devices)}
        text = "Move Device" if len(moving) == 1 else "Move Devices"
        moved = set().union(*(device_ids_of(d) for d in moving))
        envelopes = {key: points for key, points in self.project.automation(track_id).items()
                     if automation.key_device(key) in moved}
        if not envelopes:
            self._push(SetChainsCommand(self.project, before, {track_id: source, to_track_id: target_devices}, text))
            return True
        self.undo_stack.beginMacro(text)
        self._push(SetChainsCommand(self.project, before, {track_id: source, to_track_id: target_devices}, text))
        old = {(owner, key): self.project.envelope(owner, key) for key in envelopes for owner in (track_id, to_track_id)}
        new = {(track_id, key): () for key in envelopes} | {(to_track_id, key): points for key, points in envelopes.items()}
        self._push(SetEnvelopesCommand(self.project, old, new, text))
        self.undo_stack.endMacro()
        return True

    def remove_device(self, track_id: str, device_id: str) -> None:
        self.remove_devices(track_id, [device_id])

    def remove_devices(self, track_id: str, device_ids) -> None:
        """Delete devices from a track (in racks too; a rack with what is in it), in one undo step."""
        ids = set(device_ids)
        before = copy.deepcopy(self.project.track(track_id).devices)
        after = copy.deepcopy(before)

        def prune(devices: list[Device]) -> None:
            devices[:] = [d for d in devices if d.id not in ids]
            for device in devices:
                for chain in device.chains:
                    prune(chain.devices)

        prune(after)
        if after != before:
            removed = len(device_ids_of_list(before)) - len(device_ids_of_list(after))
            self._set_devices(track_id, before, after, "Delete Device" if removed == 1 else "Delete Devices")

    def _set_devices(self, track_id: str, before: list[Device], after: list[Device], text: str) -> None:
        """Change a track's devices; the automation of devices that leave it goes
        with them (their parameters', and a rack's chains' faders'), and so do the
        mappings of macros to them (in the same undo step)."""
        gone = device_ids_of_list(before) - device_ids_of_list(after)
        chains_gone = {c.id for _, c in iter_chains(before)} - {c.id for _, c in iter_chains(after)}
        self._prune_macros(after)
        orphans = [key for key in self.project.track(track_id).automation
                   if automation.key_device(key) in gone or automation.key_chain(key) in chains_gone]
        if not orphans:
            self._push(SetDevicesCommand(self.project, track_id, before, after, text))
            return
        self.undo_stack.beginMacro(text)
        self._push(SetDevicesCommand(self.project, track_id, before, after, text))
        for key in orphans:
            self._push(SetEnvelopeCommand(self.project, track_id, key, self.project.envelope(track_id, key), (), text))
        self.undo_stack.endMacro()
