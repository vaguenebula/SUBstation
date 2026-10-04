"""Parameters as operations see them: any automation target (a mixer control,
a send, a device parameter, a rack's macro or chain fader) described by a
ParamSpec, with its value now. Built-in devices are described without the
engine; plug-ins need it (EngineFacts.param_specs)."""

from __future__ import annotations

from ...model import automation
from ...model.automation import MASTER, MIXER_KEYS, MIXER_PAN, MIXER_VOLUME
from ...model.devices import BUILTIN_DEVICES, builtin_param_info, device_name
from ...model.params import ParamSpec, chain_specs, mixer_specs, send_spec
from ...model.project import MACRO_COUNT, Device, Send, iter_chains, macro_param
from .context import OpContext
from .errors import invalid, not_found


def macro_specs(device: Device) -> list[ParamSpec]:
    """A rack's macros: normalized 0..1, as the model keeps them."""
    return [ParamSpec(automation.device_key(device.id, macro_param(i)), f"Macro {i + 1}", device_name(device))
            for i in range(MACRO_COUNT)]


def device_specs(ctx: OpContext, owner: str, device: Device) -> list[ParamSpec] | None:
    """A device's parameters that can be set and automated; None if they can't be
    known (a plug-in with no engine to ask, or one that isn't loaded)."""
    if device.is_rack:
        return macro_specs(device) + chain_specs(device.id, [(c.id, c.name) for c in device.chains],
                                                 device_name(device))
    if ctx.engine is not None:
        specs = ctx.engine.param_specs(owner, device.id)
        if specs is not None:
            return specs
    if device.is_plugin or device.kind not in BUILTIN_DEVICES:
        return None
    specs = []
    for param_id in BUILTIN_DEVICES[device.kind][1]:
        info = builtin_param_info(device.kind, param_id)
        if info is None or not info.automatable or info.hidden or info.read_only:
            continue
        specs.append(ParamSpec.from_info(info, automation.device_key(device.id, param_id), device_name(device)))
    return specs


def spec_for(ctx: OpContext, owner: str, key: str) -> ParamSpec:
    """The description of an automation target; OpError if there is none."""
    track = ctx.owner(owner)
    if not automation.is_key(key):
        raise invalid(f"{key!r} isn't an automation target (see get_automation_targets)", target=key)
    if key in MIXER_KEYS:
        return mixer_specs(master=owner == MASTER)[MIXER_KEYS.index(key)]
    if (return_id := automation.key_send(key)) is not None:
        if not ctx.project.has_return(return_id) or track.is_master:
            raise not_found(f"{track.name} has no send {return_id!r}", target=key)
        return send_spec(return_id, ctx.project.return_letter(return_id))
    device = ctx.device(owner, automation.key_device(key))
    specs = device_specs(ctx, owner, device)
    if specs is None:
        raise invalid(f"{device_name(device)}'s parameters can't be read (it isn't loaded)", device_id=device.id)
    spec = next((s for s in specs if s.key == key), None)
    if spec is None:
        raise not_found(f"{device_name(device)} has no parameter {automation.parse_key(key)[2]!r}", target=key)
    return spec


def own_value(ctx: OpContext, owner: str, key: str, spec: ParamSpec) -> float:
    """A target's value as set by hand (plain), whether or not automation plays."""
    track = ctx.project.track(owner)
    if key == MIXER_VOLUME:
        return track.volume_db
    if key == MIXER_PAN:
        return track.pan
    if (return_id := automation.key_send(key)) is not None:
        return track.sends.get(return_id, Send()).level_db
    if (control := automation.key_chain_control(key)) is not None:
        chain = next((c for _, c in iter_chains(track.devices) if c.id == control[0]), None)
        if chain is not None:
            return chain.volume_db if control[1] == automation.CHAIN_VOLUME else chain.pan
        return spec.default
    device = ctx.device(owner, automation.key_device(key))
    param_id = automation.parse_key(key)[2]
    if device.is_plugin and ctx.engine is not None:  # its values live in the plug-in
        value = ctx.engine.own_value(owner, key)
        if value is not None:
            return value
    value = device.params.get(param_id)
    return spec.default if value is None else value


def value_at(ctx: OpContext, owner: str, key: str, spec: ParamSpec, beat: float) -> float:
    """A target's normalized value at a beat: its envelope's there, else its own."""
    points = ctx.project.envelope(owner, key)
    value = automation.value_at(points, beat) if points else None
    return value if value is not None else spec.to_normalized(own_value(ctx, owner, key, spec))


def is_automated(ctx: OpContext, owner: str, key: str) -> bool:
    """Whether an envelope plays this target now (it has one, not overridden)."""
    if ctx.engine is not None:
        return ctx.engine.is_automated(owner, key)
    return bool(ctx.project.envelope(owner, key))


def override(ctx: OpContext, owner: str, key: str) -> None:
    """A target set by hand while its automation plays: the automation stops until
    re-enabled, as turning its knob does (also when the value was already that)."""
    if ctx.engine is not None:
        ctx.engine.override_automation(owner, key)


def describe(ctx: OpContext, owner: str, spec: ParamSpec, value: float | None = None) -> dict:
    """A parameter as reads return it."""
    plain = own_value(ctx, owner, spec.key, spec) if value is None else value
    entry = {
        "id": automation.parse_key(spec.key)[-1] if automation.key_device(spec.key) else spec.key,
        "key": spec.key, "name": spec.name, "value": plain, "text": spec.format(plain),
        "normalized": round(spec.to_normalized(plain), 6), "min": spec.minimum, "max": spec.maximum,
        "default": spec.default, "unit": spec.unit,
    }
    if spec.steps:
        entry["steps"] = spec.steps
    if spec.labels:
        entry["labels"] = list(spec.labels)
    if spec.scale != "linear":
        entry["scale"] = spec.scale
    if ctx.project.envelope(owner, spec.key):
        entry["automated"] = True
        entry["overridden"] = bool(ctx.engine is not None and ctx.engine.is_overridden(owner, spec.key))
    return entry


def to_normalized(spec: ParamSpec, value: float, units: str) -> float:
    """A value given in plain units (dB, Hz, %, a step's index) or normalized, as the envelope stores it."""
    if units == "normalized":
        if not 0.0 <= value <= 1.0:
            raise invalid("A normalized value is 0..1")
        return spec.quantize(value)
    if spec.scale != "fader" and not spec.minimum - 1e-9 <= value <= spec.maximum + 1e-9:
        raise invalid(f"{spec.name} goes from {spec.format(spec.minimum)} to {spec.format(spec.maximum)}",
                      min=spec.minimum, max=spec.maximum)
    return spec.to_normalized(value)
