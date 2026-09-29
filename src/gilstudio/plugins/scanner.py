"""Finds VST3 and CLAP plugins in the standard Windows locations.

Hosting is not implemented yet: the engine exposes a `Processor` interface and a
`PluginFormat` extension point (engine/src/plugins/PluginFormat.h) for it. This
module only lists what is installed so the browser can show it. A real scan
(reading plugin metadata) should run in a separate process so a misbehaving
plugin cannot crash the DAW.
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class PluginInfo:
    name: str
    format: str  # "VST3" or "CLAP"
    path: str


def search_paths() -> dict[str, list[Path]]:
    common = Path(os.environ.get("CommonProgramFiles", r"C:\Program Files\Common Files"))
    local = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local")) / "Programs" / "Common"
    return {
        "VST3": [common / "VST3", local / "VST3"],
        "CLAP": [common / "CLAP", local / "CLAP"],
    }


def scan_plugins() -> list[PluginInfo]:
    found: dict[str, PluginInfo] = {}
    for fmt, roots in search_paths().items():
        extension = "." + fmt.lower()
        for root in roots:
            if not root.is_dir():
                continue
            stack = [root]
            while stack:
                folder = stack.pop()
                try:
                    entries = list(os.scandir(folder))
                except OSError:
                    continue
                for entry in entries:
                    if entry.name.lower().endswith(extension):
                        # VST3 plugins are usually bundles (folders); don't descend into them.
                        found.setdefault(entry.path.lower(), PluginInfo(Path(entry.name).stem, fmt, entry.path))
                    elif entry.is_dir(follow_symlinks=False):
                        stack.append(Path(entry.path))
    return sorted(found.values(), key=lambda p: (p.name.lower(), p.format))
