"""The browser's index and search as they were in Python, before the native
backend (browser/src) replaced them. Kept unchanged as the reference the native
one has to agree with, item for item and in the same order, and as the
baseline of the benchmarks."""

from __future__ import annotations

import os
import re
from pathlib import Path

from substation.ui.browser.browser_models import BrowserItem
from substation.ui.browser.library import Library

AUDIO_EXTENSIONS = (".wav", ".wave", ".flac", ".mp3")
MAX_FILES = 300_000
MAX_DEPTH = 16
_WORD_START = re.compile(r"(?:^|[\s_\-.()\[\]])(\w)")


def walk_audio(root: str, max_files: int = MAX_FILES, max_depth: int = MAX_DEPTH) -> list[BrowserItem]:
    items: list[BrowserItem] = []
    stack = [(root, 0)]
    while stack and len(items) < max_files:
        folder, depth = stack.pop()
        try:
            entries = list(os.scandir(folder))
        except OSError:
            continue
        for entry in entries:
            name = entry.name
            if name.startswith((".", "$")):
                continue
            try:
                if entry.is_dir(follow_symlinks=False):
                    if depth < max_depth:
                        stack.append((entry.path, depth + 1))
                elif name.lower().endswith(AUDIO_EXTENSIONS):
                    items.append(BrowserItem(name, entry.path, "audio", os.path.basename(folder)))
            except OSError:
                continue
    return items


def index_places(places: list[str], max_files: int = MAX_FILES, max_depth: int = MAX_DEPTH) -> list[BrowserItem]:
    """What the index thread made of the places: their files, once each, by name."""
    audio: dict[str, BrowserItem] = {}
    for place in [p for p in places if Path(p).is_dir()]:
        for item in walk_audio(place, max_files, max_depth):
            audio.setdefault(os.path.normcase(item.path), item)
    return sorted(audio.values(), key=lambda i: i.name.lower())


def matches(item: BrowserItem, terms: list[str]) -> bool:
    haystack = f"{item.name} {item.detail}".lower()
    return all(term in haystack for term in terms)


def match_quality(item: BrowserItem, terms: list[str]) -> int:
    name = item.name.lower()
    stem = name.rsplit(".", 1)[0] if item.kind == "audio" else name
    quality = 8 if stem == " ".join(terms) else 0
    word_starts = [m.start(1) for m in _WORD_START.finditer(name)]
    for term in terms:
        if name.startswith(term):
            quality += 3
        elif any(name.startswith(term, at) for at in word_starts):
            quality += 2
        elif term in name:
            quality += 1
    return quality


def find(items: list[BrowserItem], query: str, library: Library, sort: str = "rank") -> list[BrowserItem]:
    terms = query.lower().split()
    if terms:
        items = [i for i in items if matches(i, terms)]
    if sort == "name":
        return sorted(items, key=lambda i: i.name.casefold())
    now = library.clock()
    if not terms:  # only the used items move; the rest keep their order
        used = [i for i in items if i.key in library.records]
        if not used:
            return items
        ranks = {i.key: library.rank(i.key, now) for i in used}
        return sorted(items, key=lambda i: -ranks.get(i.key, 0.0))
    return sorted(items, key=lambda i: (-library.rank(i.key, now), -match_quality(i, terms)))


def place_items(items: list[BrowserItem], place: str) -> list[BrowserItem]:
    """The Places filter of the panel."""
    prefix = str(Path(place)).lower().rstrip("\\/") + "\\"
    return [i for i in items if i.path.lower().startswith(prefix)]
