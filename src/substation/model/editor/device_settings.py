"""Editing a device's settings: its parameters and state, loading a preset into
it, a rack's name, its on/off switch and its sidechain."""

from __future__ import annotations

import copy

from .. import automation
from ..commands import (
    SetDeviceEnabledCommand,
    SetDeviceNameCommand,
    SetDeviceParamCommand,
    SetDeviceParamsCommand,
    SetDeviceSidechainCommand,
    SetDeviceStateCommand,
)
from ..devices import builtin_param_info, device_name, loads_into
from ..project import (
    MAX_RACK_DEPTH,
    Device,
    Sidechain,
    container_of,
    find_device,
    rack_depth,
    rack_height,
)


class DeviceSettingsEdits:
    """A device's parameters, state, presets, switch and sidechain. Part of ProjectEditor (editor/__init__.py)."""

    def set_device_param(self, track_id: str, device_id: str, param_id: str, value: float,
                         merge_key: object | None = None, old: float | None = None) -> None:
        """Change a parameter. `old` is its value before, if the model doesn't know
        it (a plug-in's parameters live in the plug-in)."""
        if old is None:
            old = self.project.device(track_id, device_id).params.get(param_id)
        if old is None or old != value:
            self._push(SetDeviceParamCommand(self.project, track_id, device_id, param_id,
                                             value if old is None else old, value, merge_key))
        self.parameter_touched.emit(track_id, automation.device_key(device_id, param_id))

    def set_device_params(self, track_id: str, device_id: str, values: dict[str, float],
                          merge_key: object | None = None, text: str = "Change Device Parameters") -> None:
        """Change several of a built-in device's parameters ({param id: value}) as one undo step
        (one per gesture, with a `merge_key`, while the same parameters change)."""
        device = self.project.device(track_id, device_id)
        new = {(device_id, param_id): float(value) for param_id, value in values.items()}
        old = {}
        for (_, param_id), value in new.items():
            own = device.params.get(param_id)
            if own is None:  # a default value: as it was
                info = self.param_info(track_id, device_id, param_id)
                own = value if info is None else info.default_value
            old[(device_id, param_id)] = own
        if old != new:
            self._push(SetDeviceParamsCommand(self.project, track_id, old, new, text, merge_key))
        if values:
            self.parameter_touched.emit(track_id, automation.device_key(device_id, next(iter(values))))

    def touch_parameter(self, owner: str, key: str) -> None:
        """A parameter taken hold of (clicked) without changing it: as Ableton
        does, the arrangement shows its automation."""
        self.parameter_touched.emit(owner, key)

    def set_device_state(self, track_id: str, device_id: str, old: str | None, new: str,
                         text: str = "Load Preset") -> None:
        """Replace a device's state (base64): a plug-in's, e.g. with a preset, or
        a built-in device's besides its parameters. `old` is its state before,
        to go back to on undo."""
        self._push(SetDeviceStateCommand(self.project, track_id, device_id, old, new, text))

    def load_preset_into(self, track_id: str, device_id: str, preset: Device, text: str = "Load Preset") -> bool:
        """Load a preset (a device: serialization.load_preset) into a device of the
        same kind (loads_into), which stays where it is, with its id, on/off
        switch and sidechain: a plug-in takes the preset's state, a built-in
        device its parameters and state, a rack its chains (new devices) and
        macros. One undo step; False if it can't (another kind of device, or a
        rack that would nest too deep there). A plug-in's state before is the
        model's: store it first (EngineBridge.store_plugin_states)."""
        p = self.project
        device = p.device(track_id, device_id)
        if not loads_into(preset, device):
            return False
        if device.is_rack:
            before = copy.deepcopy(p.track(track_id).devices)
            if rack_depth(before, container_of(before, device_id)) + rack_height(preset) > MAX_RACK_DEPTH:
                return False
            after = copy.deepcopy(before)
            rack = find_device(after, device_id)
            rack.chains, rack.macros, rack.params = copy.deepcopy(preset.chains), preset.macros, dict(preset.params)
            rack.name = preset.name
            self._set_devices(track_id, before, after, text)
            return True
        commands = []
        if not device.is_plugin:  # (a plug-in's parameters are in its state)
            new = {(device_id, k): v for k, v in preset.params.items()}
            old = {}
            for key, value in new.items():
                own = device.params.get(key[1])
                if own is None:  # (a parameter the device has no value for: at its default)
                    info = builtin_param_info(device.kind, key[1])
                    own = value if info is None else info.default_value
                old[key] = own
            if old != new:
                commands.append(SetDeviceParamsCommand(p, track_id, old, new, text))
        if preset.state != device.state and (preset.state is not None or not device.is_plugin):
            commands.append(SetDeviceStateCommand(p, track_id, device_id, device.state, preset.state, text))
        if len(commands) == 1:
            self._push(commands[0])
        elif commands:
            self.undo_stack.beginMacro(text)
            for command in commands:
                self._push(command)
            self.undo_stack.endMacro()
        return True

    def rename_rack(self, track_id: str, device_id: str, name: str | None, text: str = "Rename Rack") -> None:
        """A rack's name (None or "": named by its kind again)."""
        device = self.project.device(track_id, device_id)
        name = name or None
        if device.is_rack and device.name != name:
            self._push(SetDeviceNameCommand(self.project, track_id, device_id, device.name, name, text))

    def set_device_enabled(self, track_id: str, device_id: str, enabled: bool) -> None:
        if self.project.device(track_id, device_id).enabled != enabled:
            self._push(SetDeviceEnabledCommand(self.project, track_id, device_id, enabled))

    def set_device_sidechain(self, track_id: str, device_id: str, sidechain: Sidechain | None) -> None:
        """What a device's sidechain (aux) input hears: a track's (a group's, a
        return's) signal, after its fader, before it, or after one of its devices
        (Sidechain); None: nothing. Raises ValueError for a source it can't take
        (the master, its own track, or one its track feeds: a cycle) or a tap
        after a device that isn't on the source."""
        p = self.project
        device = p.device(track_id, device_id)
        if sidechain is not None:
            source = sidechain.track_id
            if not (p.has_track(source) or p.has_return(source)) or p.sidechain_would_cycle(track_id, source):
                raise ValueError(f"{device_name(device)} can't take its sidechain from that track")
            if sidechain.tap_device is not None and not any(d.id == sidechain.tap for d in p.track(source).devices):
                raise ValueError("a sidechain can only be taken after one of its source's devices")
        if sidechain != device.sidechain:
            text = "Remove Sidechain" if sidechain is None else "Change Sidechain"
            self._push(SetDeviceSidechainCommand(p, track_id, device_id, device.sidechain, sidechain, text))
