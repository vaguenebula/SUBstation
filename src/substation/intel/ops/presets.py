"""Operations on presets: what the library has (for a device), loading a preset
into a device, and adding one as a new device.

A preset is a SUBstation .gilpreset from the preset library (Documents\\
SUBstation\\Presets, or SUBSTATION_PRESETS), or a plug-in's own .vstpreset
from a browser place."""

from __future__ import annotations

import base64
from pathlib import Path
from typing import Annotated

from ...model.devices import device_name, kind_name
from ...model.presets import group_of, library_dir
from ...model.presets import list_presets as library_presets
from ...model.serialization import PRESET_EXTENSION, ProjectFileError, load_preset
from .context import DeviceId, OpContext, TrackId
from .devices import device_ref
from .errors import invalid
from .registry import Doc, Ge, MaxLen, MinLen, Risk, operation

PresetPath = Annotated[str, MinLen(1), MaxLen(1024), Doc("The preset file's full path (list_presets).")]
VST3_PRESET = ".vstpreset"


def vstpreset_class_id(data: bytes) -> str | None:
    """The VST3 class id a .vstpreset is for, as plug-in uids are written (None:
    not a .vstpreset). The file stores it in COM byte order: the first three
    fields' bytes are swapped from the plain form."""
    if len(data) < 48 or data[:4] != b"VST3":
        return None
    try:
        com = data[8:40].decode("ascii")
    except UnicodeDecodeError:
        return None

    def swap(hex_text: str) -> str:
        return "".join(reversed([hex_text[i:i + 2] for i in range(0, len(hex_text), 2)]))

    return (swap(com[0:8]) + swap(com[8:12]) + swap(com[12:16]) + com[16:]).upper()


def _load(ctx: OpContext, path: str):
    file = ctx.check_path(path, extra_roots=[library_dir()])
    if file.suffix.lower() != PRESET_EXTENSION:
        raise invalid(f"{file.name} isn't a {PRESET_EXTENSION} preset")
    try:
        return file, load_preset(file)
    except ProjectFileError as exc:
        raise invalid(f"{file.name} can't be read: {exc}") from exc


@operation(risk=Risk.READ, summary="The presets in the library: all of them, or those for a device (by track and "
                                   "device id, or by its kind's name).")
def list_presets(ctx: OpContext, track_id: TrackId | None = None, device_id: DeviceId | None = None,
                 device_kind: Annotated[str | None, MaxLen(128),
                                        Doc("A device's kind as named in the library ('Synth', a plug-in's "
                                            "name).")] = None) -> dict:
    group = None
    if device_id is not None:
        if track_id is None:
            raise invalid("Give the device's track_id too")
        group = group_of(ctx.device(track_id, device_id))
    elif device_kind is not None:
        group = device_kind
    presets = [{"name": p.name, "device": p.group, "path": str(p.path)} for p in library_presets()
               if group is None or p.group.casefold() == group.casefold()]
    return {"presets": presets}


@operation(risk=Risk.EDIT, summary="Load a preset into a device of its kind, which stays where it is: a .gilpreset "
                                   "from the library (a built-in device's, a plug-in's, a rack's), or a plug-in's "
                                   ".vstpreset from a browser place.", label="Load Preset")
def load_device_preset(ctx: OpContext, track_id: TrackId, device_id: DeviceId, path: PresetPath) -> dict:
    device = ctx.device(track_id, device_id)
    ctx.check_unfrozen(track_id)
    if Path(path).suffix.lower() == VST3_PRESET:
        if not device.is_plugin:
            raise invalid(f"{device_name(device)} isn't a plug-in: a .vstpreset is a plug-in's own preset")
        file = ctx.check_path(path)
        data = file.read_bytes()
        class_id = vstpreset_class_id(data)
        if class_id is None:
            raise invalid(f"{file.name} isn't a VST3 preset")
        if class_id != device.plugin.uid.upper():
            raise invalid(f"{file.name} is another plug-in's preset, not {device.plugin.name}'s")
        old = ctx.engine.plugin_state(track_id, device_id) if ctx.engine is not None else None
        old_text = base64.b64encode(old).decode("ascii") if old is not None else device.state
        ctx.editor.set_device_state(track_id, device_id, old_text, base64.b64encode(data).decode("ascii"),
                                    f"Load Preset {file.stem}")
        return {"device": device_ref(ctx, track_id, device), "preset": file.stem}
    file, preset = _load(ctx, path)
    if ctx.engine is not None and device.is_plugin:
        ctx.engine.store_plugin_states({device_id})  # (undo goes back to its state as it is now)
    if not ctx.editor.load_preset_into(track_id, device_id, preset, f"Load Preset {file.stem}"):
        raise invalid(f"{file.stem} is a preset for {kind_name(preset)}, not {device_name(device)}")
    return {"device": device_ref(ctx, track_id, ctx.project.device(track_id, device_id)), "preset": file.stem}


@operation(risk=Risk.EDIT, summary="Add a library preset (.gilpreset) to a track's chain as a new device, as "
                                   "dropping it from the browser does (an instrument only on a MIDI track).",
           label="Insert Preset")
def insert_preset(ctx: OpContext, track_id: TrackId, path: PresetPath,
                  index: Annotated[int | None, Ge(0), Doc("Where in the chain; null: last.")] = None) -> dict:
    ctx.owner(track_id)
    ctx.check_unfrozen(track_id)
    file, preset = _load(ctx, path)
    if not ctx.editor.insert_device(track_id, preset, index, None, f"Add {file.stem}"):
        raise invalid(f"{file.stem} can't go on {ctx.project.track(track_id).name}")
    return {"device": device_ref(ctx, track_id, ctx.project.device(track_id, preset.id)), "preset": file.stem}
