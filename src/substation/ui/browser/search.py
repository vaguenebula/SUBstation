"""Filtering and ordering the browser's lists: what to ask the search for.

The native backend does the searching (browser/src/Search.cpp), on its own
thread; `scope_query` turns a sidebar entry into what it searches. An item
matches when every word of the query is in its name or detail (folder, vendor,
category), ignoring case. That is also where later filters (items hidden from
search) and orders (similar sounds) go.

Sort orders:
  "rank"  what you use most (and most recently) first, then the best name matches,
          then the list's own order. With nothing used yet, a search lists the
          names that start with what you typed first.
  "name"  alphabetical.

The list's own order: built-in devices, plug-ins (as the scan found them),
presets (by the device they are for, then by name), then samples by name."""

from __future__ import annotations

from pathlib import Path

from ... import _browser
from .browser_models import BrowserItem

SORTS = {"rank": "Rank", "name": "Name"}

# The groups of items searched, as the backend numbers them.
AUDIO = _browser.AUDIO  # the index's files
BUILTIN = 1
PLUGINS = 2
PRESETS = 3


def place_prefix(place: str) -> str:
    """Files under a place have paths starting with this (in lower case)."""
    return str(Path(place)).lower().rstrip("\\/") + "\\"


def scope_query(scope: tuple) -> tuple[list[int], str, str]:
    """What a sidebar entry lists: (groups in order, tag, place prefix)."""
    kind, sub = scope[0], (scope[1] if len(scope) > 1 else "")
    if kind == "all":
        return [BUILTIN, PLUGINS, PRESETS, AUDIO], "", ""
    if kind == "builtin":
        return [BUILTIN], sub, ""
    if kind == "plugins":
        return [PLUGINS], sub, ""
    if kind == "presets":
        return [PRESETS], sub, ""
    if kind == "place":
        return [AUDIO], "", place_prefix(sub)
    return [AUDIO], "", ""


def plugin_tag(item: BrowserItem) -> str:
    """The Plug-ins category an item is listed under."""
    return "Instruments" if item.plugin is not None and item.plugin.instrument else "Audio Effects"
