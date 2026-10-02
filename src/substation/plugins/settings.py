"""Persistent plug-in preferences (QSettings): the user's own VST3 folders."""

from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import QSettings

from .scanner import search_paths

KEY = "plugins/vst3_folders"


def custom_folders() -> list[str]:
    """The folders the user added, searched after the standard ones."""
    stored = QSettings().value(KEY)
    if isinstance(stored, str):  # QSettings gives a one-item list back as a string
        stored = [stored]
    return [str(p) for p in stored] if isinstance(stored, list) else []


def set_custom_folders(folders: list[str]) -> None:
    QSettings().setValue(KEY, [str(Path(f)) for f in folders])


def plugin_folders() -> list[Path]:
    """Every folder a scan looks in."""
    return search_paths(custom_folders())
