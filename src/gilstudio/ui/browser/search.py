"""Filtering and ordering the browser's lists.

`find` is the one place a list is narrowed down and sorted, so it is where later
filters (items hidden from search) and orders (similar sounds) go.

Sort orders:
  "rank"  what you use most (and most recently) first, then the best name matches,
          then the list's own order. With nothing used yet, a search lists the
          names that start with what you typed first.
  "name"  alphabetical."""

from __future__ import annotations

import re

from .browser_models import BrowserItem
from .library import Library

SORTS = {"rank": "Rank", "name": "Name"}
_WORD_START = re.compile(r"(?:^|[\s_\-.()\[\]])(\w)")


def match_quality(item: BrowserItem, terms: list[str]) -> int:
    """How well the terms match the item: in its name beats in its folder or vendor,
    and at the start of the name or of a word beats the middle of one."""
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
        items = [i for i in items if i.matches(terms)]
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
