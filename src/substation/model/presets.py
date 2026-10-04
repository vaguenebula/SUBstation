"""The user's preset library: devices saved by name (a device's save button),
listed in the browser's Presets section.

A preset is a .gilpreset file (serialization.save_preset: the device as the
project file stores it, a rack with everything in it). The library is a folder
(Documents\\SUBstation\\Presets, or SUBSTATION_PRESETS) with a folder per kind
of device, named as the kind is (a plug-in's name, a built-in device's, "Audio
Effect Rack" or "Instrument Rack"): the browser groups presets by it. A rack
takes the name of the preset it is saved as or loaded from (Device.name). Presets
straight in the library folder (put there by hand) are listed too, ungrouped.

A preset loads as a new device (serialization.load_preset), or into a device
of the same kind (editor.loads_into, ProjectEditor.load_preset_into).

Default presets: a device saved as the default for its kind (a built-in device
by its kind, a plug-in by its class id; not racks) is what new devices of that
kind start as (`default_device`, which ProjectEditor asks through
set_device_defaults). They are in the library's Defaults folder, which the
browser doesn't list: "<device name>.gilpreset" for a built-in device,
"<plug-in name> (<class id>).gilpreset" for a plug-in."""

from __future__ import annotations

import os
import re
from dataclasses import dataclass
from pathlib import Path

from .editor import BUILTIN_DEVICES, kind_name
from .project import PLUGIN_KIND, RACK_KIND, Device, PluginRef
from .serialization import PRESET_EXTENSION, ProjectFileError, load_preset, save_preset

FORBIDDEN = re.compile(r'[<>:"/\\|?*\x00-\x1f]')  # not in Windows file names
RESERVED = {"CON", "PRN", "AUX", "NUL", *(f"COM{i}" for i in range(1, 10)), *(f"LPT{i}" for i in range(1, 10))}
DEFAULTS = "Defaults"  # the library's folder of default presets (not listed)


def library_dir() -> Path:
    override = os.environ.get("SUBSTATION_PRESETS")
    if override:
        return Path(override)
    return Path.home() / "Documents" / "SUBstation" / "Presets"


@dataclass(frozen=True)
class PresetFile:
    path: Path
    name: str  # the file's name, without its extension
    group: str  # the device it is for (its folder in the library); "" straight in the library


def file_name(name: str) -> str:
    """A name as a file name: characters Windows refuses become "_". Raises
    ValueError for one that is empty (or only dots and spaces) or reserved."""
    cleaned = FORBIDDEN.sub("_", name).strip().rstrip(". ")
    if not cleaned or cleaned.split(".")[0].upper() in RESERVED:
        raise ValueError(f"{name!r} can't be a preset's name")
    return cleaned


def group_of(device: Device) -> str:
    """The library folder for presets of a device: its name (not the defaults' folder)."""
    name = file_name(kind_name(device))
    return f"{name} (Device)" if name.casefold() == DEFAULTS.casefold() else name


def preset_path(device: Device, name: str, root: Path | None = None) -> Path:
    """Where a preset of `device` called `name` goes in the library."""
    return (library_dir() if root is None else Path(root)) / group_of(device) / f"{file_name(name)}{PRESET_EXTENSION}"


def save_to_library(device: Device, name: str, root: Path | None = None) -> Path:
    """Save a device as a preset in the library (replacing one of that name); its path."""
    path = preset_path(device, name, root)
    path.parent.mkdir(parents=True, exist_ok=True)
    save_preset(device, path)
    return path


def list_presets(root: Path | None = None) -> list[PresetFile]:
    """The presets in the library: by group, then by name (ignoring case)."""
    root = library_dir() if root is None else Path(root)
    found = []
    try:
        entries = list(os.scandir(root))
    except OSError:  # no library yet
        return []
    for entry in entries:
        if entry.is_file() and entry.name.lower().endswith(PRESET_EXTENSION):
            found.append(PresetFile(Path(entry.path), Path(entry.name).stem, ""))
        elif entry.is_dir() and not entry.name.startswith(".") and entry.name.casefold() != DEFAULTS.casefold():
            try:
                found += [PresetFile(Path(f.path), Path(f.name).stem, entry.name) for f in os.scandir(entry.path)
                          if f.is_file() and f.name.lower().endswith(PRESET_EXTENSION)]
            except OSError:
                continue
    return sorted(found, key=lambda p: (p.group.casefold(), p.name.casefold()))


def rename_preset(path: Path, name: str) -> Path:
    """Give a preset another name (in its folder); its new path. Raises
    FileExistsError if another preset there has that name, ValueError for a
    name that can't be a file's."""
    path = Path(path)
    target = path.with_name(f"{file_name(name)}{PRESET_EXTENSION}")
    if str(target) == str(path):  # (paths compare ignoring case on Windows)
        return path
    if target.exists() and os.path.normcase(target) != os.path.normcase(path):  # (a change of case is no clash)
        raise FileExistsError(f"There is a preset called {target.stem} already")
    path.rename(target)
    return target



# --- Default presets -----------------------------------------------------------------


def default_path(kind: str, plugin: PluginRef | None = None, root: Path | None = None) -> Path | None:
    """Where the default preset of a kind of device is (None: racks, which have none)."""
    if kind == RACK_KIND or (kind == PLUGIN_KIND and plugin is None):
        return None
    name = file_name(kind_name(Device(id="", kind=kind, plugin=plugin)))
    if plugin is not None:
        name = f"{name} ({file_name(plugin.uid)})"
    return (library_dir() if root is None else Path(root)) / DEFAULTS / f"{name}{PRESET_EXTENSION}"


def has_default(kind: str, plugin: PluginRef | None = None, root: Path | None = None) -> bool:
    path = default_path(kind, plugin, root)
    return path is not None and path.is_file()


def save_default(device: Device, root: Path | None = None) -> Path:
    """Save a device (not a rack) as the default preset of its kind (replacing
    the one there was); its path."""
    path = default_path(device.kind, device.plugin, root)
    if path is None:
        raise ValueError("Racks have no default preset")
    path.parent.mkdir(parents=True, exist_ok=True)
    save_preset(device, path)
    return path


def clear_default(kind: str, plugin: PluginRef | None = None, root: Path | None = None) -> bool:
    """New devices of this kind start as they come again. Whether there was a default."""
    path = default_path(kind, plugin, root)
    if path is None or not path.is_file():
        return False
    path.unlink()
    return True


def default_device(kind: str, plugin: PluginRef | None = None, root: Path | None = None) -> Device | None:
    """A new device of this kind (and plug-in) as its default preset has it (new
    ids), or None: it has none (or one that can't be read, or is for another
    device: it is ignored). A plug-in keeps the PluginRef asked for (where it
    is now) with the preset's state; a built-in device takes the preset's
    parameters over its defaults (those it was saved without stay at theirs)."""
    path = default_path(kind, plugin, root)
    if path is None or not path.is_file():
        return None
    try:
        device = load_preset(path)
    except ProjectFileError:
        return None
    if device.kind != kind:
        return None
    if plugin is not None:
        if device.plugin is None or device.plugin.uid != plugin.uid:
            return None
        device.plugin = plugin
    elif kind in BUILTIN_DEVICES:
        device.params = {**BUILTIN_DEVICES[kind][1], **device.params}
    return device
