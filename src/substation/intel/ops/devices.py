"""Operations on devices: what can be added, adding and removing devices, their
parameters (built-in and plug-in alike), their switch and their sidechain."""

from __future__ import annotations

from typing import Annotated, Literal

from ...model import automation
from ...model.devices import BUILTIN_CATEGORIES, BUILTIN_DEVICES, BUILTIN_INSTRUMENTS, device_name
from ...model.project import (
    MACRO_COUNT,
    PLUGIN_KIND,
    POST_FADER,
    PRE_FADER,
    PRE_FX,
    RACK_KIND,
    Device,
    Sidechain,
    macro_param,
)
from .context import DeviceId, OpContext, TrackId
from .errors import invalid, not_found
from .params import describe, device_specs, is_automated, override, own_value
from .registry import Doc, Ge, Le, MaxLen, MinLen, Risk, operation
from .tracks import find_plugin

Units = Annotated[Literal["plain", "normalized"],
                  Doc("'plain': in the parameter's own units (dB, Hz, %, a list's index; a plug-in's 0..1); "
                      "'normalized': 0..1 as automation stores it.")]

CATEGORY_INSTRUMENTS = "Instruments"
CATEGORY_EFFECTS = "Audio Effects"


def device_ref(ctx: OpContext, track_id: str, device: Device) -> dict:
    ref = {"id": device.id, "track_id": track_id, "kind": device.kind, "name": device_name(device)}
    if device.plugin is not None:
        ref["plugin"] = {"name": device.plugin.name, "vendor": device.plugin.vendor, "uid": device.plugin.uid}
    if not device.enabled:
        ref["enabled"] = False
    return ref


def _resolve(ctx: OpContext, device: str):
    """(kind, plugin) for a device named as add_device takes it."""
    if device == RACK_KIND:
        return RACK_KIND, None
    if device in BUILTIN_DEVICES:
        return device, None
    plugin = find_plugin(ctx, device)
    if plugin is None:
        raise not_found(f"There is no device {device!r} (see list_available_devices)", device=device)
    return PLUGIN_KIND, plugin


@operation(risk=Risk.READ, summary="What devices can be added: the built-in ones, and the plug-ins the scan "
                                   "found, by category ('Instruments', 'Audio Effects') and a name query.")
def list_available_devices(ctx: OpContext,
                           category: Annotated[Literal["Instruments", "Audio Effects"] | None,
                                               Doc("Only these; null: all.")] = None,
                           query: Annotated[str | None, MaxLen(64),
                                            Doc("Words its name, vendor or VST3 category must contain "
                                                "(e.g. 'reverb').")] = None) -> dict:
    words = (query or "").lower().split()
    found = []
    for group, kinds in BUILTIN_CATEGORIES.items():
        for kind in kinds:
            name = BUILTIN_DEVICES[kind][0]
            found.append({"id": kind, "name": name, "builtin": True, "category": group,
                          "instrument": kind in BUILTIN_INSTRUMENTS, "text": f"{name} {kind} {group}"})
    found.append({"id": RACK_KIND, "name": "Audio Effect Rack", "builtin": True, "category": CATEGORY_EFFECTS,
                  "instrument": False, "text": "rack audio effect rack group chains"})
    for plugin in (ctx.ui.available_plugins() if ctx.ui is not None else []):
        group = CATEGORY_INSTRUMENTS if plugin.instrument else CATEGORY_EFFECTS
        found.append({"id": plugin.uid, "name": plugin.name, "vendor": plugin.vendor, "builtin": False,
                      "category": group, "vst3_category": plugin.category, "instrument": plugin.instrument,
                      "text": f"{plugin.name} {plugin.vendor} {plugin.category.replace('|', ' ')}"})
    devices = [{k: v for k, v in d.items() if k != "text"} for d in found
               if (category is None or d["category"] == category) and all(w in d["text"].lower() for w in words)]
    return {"devices": devices}


@operation(risk=Risk.EDIT, summary="Add a device to a track's (or the master's, a return's) chain, or to a rack's "
                                   "chain on it: a built-in device by its id, a plug-in by its uid "
                                   "(list_available_devices), or 'rack'. An instrument goes only on a MIDI track, "
                                   "first, replacing the one there.")
def add_device(ctx: OpContext, track_id: TrackId,
               device: Annotated[str, MinLen(1), MaxLen(64), Doc("A built-in device's id, a plug-in's uid, or "
                                                                 "'rack'.")],
               index: Annotated[int | None, Ge(0), Doc("Where in the chain (0: first); null: last.")] = None,
               chain_id: Annotated[str | None, Doc("A rack chain's id to add it into; null: the track's own "
                                                   "chain.")] = None) -> dict:
    ctx.owner(track_id)
    ctx.check_unfrozen(track_id)
    kind, plugin = _resolve(ctx, device)
    added = ctx.editor.add_device(track_id, kind, index, plugin, chain_id)
    if added is None:
        raise invalid("It can't go there (an instrument on a track that isn't MIDI, a chain that isn't there, "
                      "or racks too deep)")
    return {"device": device_ref(ctx, track_id, ctx.project.device(track_id, added.id))}


@operation(risk=Risk.DESTRUCTIVE, summary="Delete devices from a track (a rack with what is in it), and their "
                                          "automation.")
def remove_devices(ctx: OpContext, track_id: TrackId,
                   device_ids: Annotated[list[DeviceId], MinLen(1)]) -> dict:
    names = [device_name(ctx.device(track_id, d)) for d in device_ids]
    ctx.check_unfrozen(track_id)
    ctx.editor.remove_devices(track_id, device_ids)
    return {"deleted": names}


@operation(risk=Risk.EDIT, summary="Switch a device on or off.", label="Toggle Device", idempotent=True)
def set_device_enabled(ctx: OpContext, track_id: TrackId, device_id: DeviceId, enabled: bool) -> dict:
    device = ctx.device(track_id, device_id)
    ctx.check_unfrozen(track_id)
    ctx.editor.set_device_enabled(track_id, device_id, enabled)
    return {"device": device_ref(ctx, track_id, device)}


@operation(risk=Risk.READ, summary="A device's parameters with their values now (plain units, the device's own "
                                   "text, normalized), ranges, steps, and whether automation plays them. Paged; "
                                   "`query` filters by name.")
def get_device_params(ctx: OpContext, track_id: TrackId, device_id: DeviceId,
                      query: Annotated[str | None, MaxLen(64), Doc("Words the name must contain.")] = None,
                      offset: Annotated[int, Ge(0)] = 0, limit: Annotated[int, Ge(1), Le(200)] = 50) -> dict:
    device = ctx.device(track_id, device_id)
    specs = device_specs(ctx, track_id, device)
    if specs is None:
        raise invalid(f"{device_name(device)}'s parameters can't be read (it isn't loaded)")
    words = (query or "").lower().split()
    matching = [s for s in specs if all(w in s.name.lower() for w in words)]
    return {"device": device_ref(ctx, track_id, device), "total": len(matching), "offset": offset,
            "params": [describe(ctx, track_id, s) for s in matching[offset:offset + limit]]}


@operation(risk=Risk.EDIT, summary="Set a device's parameter (a built-in's, a plug-in's, a rack's macro). The "
                                   "result echoes the device's own text for the new value. Setting a parameter "
                                   "that automation plays overrides its automation, as turning its knob does "
                                   "(re_enable_automation undoes that).", label="Change Device Parameter")
def set_device_param(ctx: OpContext, track_id: TrackId, device_id: DeviceId,
                     param_id: Annotated[str, MinLen(1), MaxLen(128), Doc("The parameter's id "
                                                                          "(get_device_params).")],
                     value: float, units: Units = "plain") -> dict:
    device = ctx.device(track_id, device_id)
    ctx.check_unfrozen(track_id)
    key = automation.device_key(device_id, param_id)
    specs = device_specs(ctx, track_id, device)
    if specs is None:
        raise invalid(f"{device_name(device)}'s parameters can't be set (it isn't loaded)")
    spec = next((s for s in specs if s.key == key), None)
    if spec is None:
        raise not_found(f"{device_name(device)} has no parameter {param_id!r}", param_id=param_id)
    if automation.key_chain(key) is not None:
        raise invalid("A rack chain's fader isn't a parameter to set here")
    if units == "normalized":
        if not 0.0 <= value <= 1.0:
            raise invalid("A normalized value is 0..1")
        plain = spec.from_normalized(value)
    else:
        if not spec.minimum - 1e-9 <= value <= spec.maximum + 1e-9:
            raise invalid(f"{spec.name} goes from {spec.format(spec.minimum)} to {spec.format(spec.maximum)}",
                          min=spec.minimum, max=spec.maximum)
        plain = spec.from_normalized(spec.to_normalized(value)) if spec.steps else value
    overrides = is_automated(ctx, track_id, key)
    if device.is_rack:
        index = next(i for i in range(MACRO_COUNT) if macro_param(i) == param_id)
        ctx.editor.set_macro(track_id, device_id, index, plain)
    else:
        old = own_value(ctx, track_id, key, spec) if device.is_plugin else None
        ctx.editor.set_device_param(track_id, device_id, param_id, plain, old=old)
    result = {"device": device_ref(ctx, track_id, device), "param": param_id, "name": spec.name, "value": plain,
              "text": spec.format(plain)}
    if overrides:
        override(ctx, track_id, key)
        result["overrides_automation"] = True
    return result


@operation(risk=Risk.EDIT, summary="What a device's sidechain input hears: a track's (group's, return's) signal "
                                   "after its fader ('post'), before it ('pre'), before its devices ('pre-fx'), or "
                                   "after one of its devices (that device's id); null source: nothing.",
           label="Change Sidechain")
def set_device_sidechain(ctx: OpContext, track_id: TrackId, device_id: DeviceId,
                         source_track_id: Annotated[str | None, Doc("The source track's id; null: none.")],
                         tap: Annotated[str, MinLen(1), MaxLen(64),
                                        Doc("'post', 'pre', 'pre-fx' or a device id on the source.")] = POST_FADER
                         ) -> dict:
    device = ctx.device(track_id, device_id)
    ctx.check_unfrozen(track_id)
    sidechain = None
    if source_track_id is not None:
        ctx.owner(source_track_id)
        if tap not in (POST_FADER, PRE_FADER, PRE_FX) and ctx.project.device_owner(tap) != source_track_id:
            raise invalid("A sidechain taps 'post', 'pre', 'pre-fx' or after a device on its source")
        sidechain = Sidechain(source_track_id, tap)
    ctx.editor.set_device_sidechain(track_id, device_id, sidechain)
    return {"device": device_ref(ctx, track_id, device),
            "sidechain": None if sidechain is None else {"track_id": source_track_id, "tap": tap}}
