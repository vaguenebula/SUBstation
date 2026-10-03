"""Kinds of devices: the built-in ones (as the engine has them), what devices are
called, which are instruments, and new devices, racks and chains."""

from __future__ import annotations

from .. import _engine as ge
from .project import (
    MACRO_COUNT,
    PLUGIN_KIND,
    RACK_KIND,
    Chain,
    Device,
    PluginRef,
    iter_devices,
    macro_param,
    new_id,
)

# The built-in devices come from the engine (engine/src/builtin/devices/), so a new one
# needs nothing here. kind: (display name, {param id: default}).
BUILTIN_DEVICES = {
    d.id: (d.name, {p.id: p.default_value for p in d.params}) for d in ge.builtin_devices()
}
# How the browser's Built-in category groups the devices above.
BUILTIN_CATEGORIES: dict[str, list[str]] = {}
for _device in ge.builtin_devices():
    BUILTIN_CATEGORIES.setdefault(_device.category, []).append(_device.id)
BUILTIN_INSTRUMENTS = frozenset(d.id for d in ge.builtin_devices() if d.is_instrument)
DEFAULT_INSTRUMENT = "synth"  # new MIDI tracks come with it, ready to play


def is_instrument(kind: str, plugin: PluginRef | None = None) -> bool:
    """Whether a device of this kind (and plug-in) is an instrument."""
    if kind == PLUGIN_KIND:
        return plugin is not None and plugin.instrument
    return kind in BUILTIN_INSTRUMENTS


def device_is_instrument(device: Device) -> bool:
    """An instrument, or a rack with one in it (an instrument rack: it plays the track's notes)."""
    if device.is_rack:
        return any(device_is_instrument(d) for chain in device.chains for d in chain.devices)
    return is_instrument(device.kind, device.plugin)


def loads_into(preset: Device, device: Device) -> bool:
    """Whether a preset can be loaded into a device in place: a device of the
    same kind (the same plug-in; a rack into a rack, both instrument racks or
    neither)."""
    if preset.kind != device.kind or device_is_instrument(preset) != device_is_instrument(device):
        return False
    if preset.is_plugin:
        return preset.plugin is not None and device.plugin is not None and preset.plugin.uid == device.plugin.uid
    return True


def device_name(device: Device) -> str:
    """What a device is called: a rack by its own name if it has one (its
    preset's), else every device by its kind (kind_name)."""
    if device.is_rack and device.name:
        return device.name
    return kind_name(device)


def kind_name(device: Device) -> str:
    """The name of a device's kind: a plug-in's, a built-in device's, Audio Effect Rack or Instrument Rack."""
    if device.is_rack:
        return "Instrument Rack" if device_is_instrument(device) else "Audio Effect Rack"
    if device.plugin is not None:
        return device.plugin.name
    return BUILTIN_DEVICES.get(device.kind, (device.kind,))[0]


def new_device(kind: str, plugin: PluginRef | None = None) -> Device:
    if kind == PLUGIN_KIND:
        if plugin is None:
            raise ValueError("a plug-in device needs a plug-in")
        return Device(id=new_id(), kind=kind, plugin=plugin)
    if kind == RACK_KIND:
        return new_rack([])
    return Device(id=new_id(), kind=kind, params=dict(BUILTIN_DEVICES[kind][1]))


def new_rack(chains: list[Chain]) -> Device:
    """A rack with these chains, its macros at 0."""
    return Device(id=new_id(), kind=RACK_KIND, params={macro_param(i): 0.0 for i in range(MACRO_COUNT)},
                  chains=chains)


def new_chain(name: str, devices: list[Device] | None = None) -> Chain:
    return Chain(id=new_id(), name=name, devices=devices or [])


def builtin_param_info(kind: str, param_id: str):
    """A built-in device's parameter as the engine describes it (None: no such one)."""
    return next((p for d in ge.builtin_devices() if d.id == kind for p in d.params if p.id == param_id), None)


def device_ids_of_list(devices: list[Device]) -> set[str]:
    """The ids of these devices and of everything in them."""
    return {d.id for d in iter_devices(devices)}


def device_ids_of(device: Device) -> set[str]:
    """A device's id, and those of everything in it (a rack)."""
    return device_ids_of_list([device])
