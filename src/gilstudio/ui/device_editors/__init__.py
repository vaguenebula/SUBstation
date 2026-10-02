"""Built-in devices' own editors, in the device view.

A built-in device shows a knob per parameter (device_panel.DeviceWidget) unless
a module here gives it an editor of its own: a DeviceWidget subclass registered
for the device's kind (its engine id),

    @device_editor("compressor")
    class CompressorWidget(DeviceWidget): ...

Every module in this package is loaded the first time an editor is looked up,
so a new one needs nothing else. An editor sets parameters as the knobs do
(ProjectEditor.set_device_param), so undo, automation and saving work alike,
and draws the device's displays (DeviceWidget.read_display) in
refresh_displays(), which the device view calls as the meters update.
"""

from __future__ import annotations

import importlib
import pkgutil

_editors: dict[str, type] = {}
_loaded = False


def device_editor(kind: str):
    """Registers the decorated class as the editor of built-in devices of `kind`."""

    def register(cls: type) -> type:
        if kind in _editors:
            raise ValueError(f"Two editors for the built-in device {kind!r}")
        _editors[kind] = cls
        return cls

    return register


def editor_for(kind: str) -> type | None:
    """The editor registered for built-in devices of `kind`, if any."""
    global _loaded
    if not _loaded:  # now, not at import: the editors import the device view, which imports this
        _loaded = True
        for module in pkgutil.iter_modules(__path__):
            importlib.import_module(f"{__name__}.{module.name}")
    return _editors.get(kind)
