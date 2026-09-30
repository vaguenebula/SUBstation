"""The browser's native backend (browser/src, gilstudio._browser): that it
indexes, matches and orders exactly as the Python code before it did (kept in
browser_reference.py), keeps its index up to date incrementally, and runs its
searches off the caller's thread, latest first."""

import os
import random
import re
import subprocess
import sys
import threading
import time
import unicodedata
from pathlib import Path

import pytest

from gilstudio import _browser
from gilstudio.audio.engine_bridge import AUDIO_EXTENSIONS
from gilstudio.model.project import PluginRef
from gilstudio.ui.browser.browser_models import BrowserItem, ItemListModel
from gilstudio.ui.browser.file_index import (
    KINDS,
    SearchResult,
    place_spec,
    usage_records,
)
from gilstudio.ui.browser.library import HALF_LIFE_DAYS, Library
from gilstudio.ui.browser.search import place_prefix

from . import browser_reference as ref

AUDIO = _browser.AUDIO
DAY = 86400.0
_WORD_START = re.compile(r"(?:^|[\s_\-.()\[\]])(\w)")
windows_only = pytest.mark.skipif(sys.platform != "win32", reason="the backend's file system side is Win32")


class Backend:
    """The native backend over some places, driven synchronously."""

    def __init__(self, places, store="", max_files=ref.MAX_FILES, max_depth=ref.MAX_DEPTH):
        self.native = _browser.Browser(str(store), list(AUDIO_EXTENSIONS), max_files, max_depth)
        if places is not None:
            self.set_places(places)

    def set_places(self, places):
        self.native.set_places([place_spec(str(p)) for p in places])

    def wait(self):
        assert self.native.wait_idle(60)

    def search(self, text="", sort="rank", now=0.0, groups=(AUDIO,), tag="", prefix=""):
        generation = self.native.search(text, sort, now, list(groups), tag, prefix)
        self.wait()
        result = self.native.take()[3]
        assert result is not None and result.generation == generation
        return result

    def items(self, **query) -> list[BrowserItem]:
        result = self.search(**query)
        return SearchResult(result, {}).items(0, result.total)

    def close(self):
        self.native.close()


@pytest.fixture
def backends():
    made = []

    def make(*args, **kwargs):
        backend = Backend(*args, **kwargs)
        made.append(backend)
        return backend

    yield make
    for backend in made:
        backend.close()


def touch(path: Path) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"")
    return path


# --- Text: Python's own rules ----------------------------------------------------------

same_unicode = pytest.mark.skipif(_browser.UNICODE_VERSION != unicodedata.unidata_version,
                                  reason="the tables were made with another Unicode version")


@same_unicode
def test_lower_and_casefold_of_every_character():
    for cp in range(0x110000):
        c = chr(cp)
        if c.lower() != _browser.lower(c) or c.casefold() != _browser.casefold(c):
            pytest.fail(f"U+{cp:04X}: {_browser.lower(c)!r} {_browser.casefold(c)!r}")


@same_unicode
def test_lower_follows_final_sigma():
    rng = random.Random(1)
    pool = ["Σ", "σ", "ς", "Α", "a", "'", ".", "\u0301", "\u00ad", " ", "1", "İ", "ǅ", "_", "Ω", "\u0345", "ʰ"]
    for _ in range(20000):
        s = "".join(rng.choice(pool) for _ in range(rng.randint(1, 7)))
        assert _browser.lower(s) == s.lower(), repr(s)


@same_unicode
def test_split_and_word_starts():
    rng = random.Random(2)
    pool = ["a", "B", "7", "_", "-", ".", "(", ")", "[", "]", " ", "\t", "\u00a0", "\u2003", "\x1c", "é", "ß",
            "\u0301", "٣", "·", "!", "漢", "\U0001f600"]
    for _ in range(20000):
        s = "".join(rng.choice(pool) for _ in range(rng.randint(0, 9)))
        assert _browser.split(s) == s.split(), repr(s)
        assert _browser.word_starts(s) == [m.start(1) for m in _WORD_START.finditer(s)], repr(s)


@windows_only
def test_keys_are_normcase():
    for s in ["C:\\Samples\\Kick.WAV", "D:\\ÀΣ\\İ\\ǅ.wav", "E:\\Straße\\ΑΣ.flac", "C:\\x\\\U0001f600.mp3"]:
        assert _browser.nt_lower(s) == os.path.normcase(s)


def random_name(rng: random.Random) -> str:
    words = ["Kick", "kick", "KICK", "Snare", "808", "Bass", "Big", "Deep", "Hat", "Open", "Loop", "e", "Σ",
             "ΑΣ", "İ", "Straße", "Café", "Ω", "a", "x"]
    seps = [" ", "_", "-", ".", "(", ")", "[", "]", "", "  "]
    parts = [rng.choice(words)]
    for _ in range(rng.randint(0, 3)):
        parts += [rng.choice(seps), rng.choice(words)]
    return "".join(parts)


def random_query(rng: random.Random) -> str:
    words = ["kick", "Kick", "k", "e", "808", "bass", "big kick", "σ", "ας", "i̇", "straße", "café", "ω", "a",
             "deep_", "(", "", " ", "hat open", "zz", ".wav", "kick kick", "ss"]
    return rng.choice(words) if rng.random() < 0.7 else " ".join(rng.choice(words) for _ in range(2))


def test_match_quality_as_python():
    rng = random.Random(3)
    for _ in range(5000):
        name = random_name(rng) + rng.choice([".wav", ".WAV", "", ".flac"])
        query = random_query(rng)
        terms = query.lower().split()
        if not terms:
            continue
        for kind in ("audio", "plugin"):
            item = BrowserItem(name, "C:/x/" + name, kind, "Folder")
            assert _browser.match_quality(name, kind == "audio", query) == ref.match_quality(item, terms), \
                (name, query, kind)


# --- Searching: as the Python find() did ------------------------------------------------

def random_items(rng: random.Random, n: int) -> list[BrowserItem]:
    items = []
    for i in range(n):
        name = random_name(rng)
        kind = rng.choice(["audio", "plugin", "device"])
        detail = rng.choice(["Drums", "Vendor", "Audio Effects", "Instruments", "Kicks", "ΣΑ", ""])
        if kind == "audio":
            name += rng.choice([".wav", ".mp3", ".WAV"])
            items.append(BrowserItem(name, f"C:\\Lib\\{detail}\\{i}\\{name}", "audio", detail))
        elif kind == "plugin":
            ref_ = PluginRef(format="VST3", uid=f"uid{i}", name=name, vendor=detail, path=f"C:\\P\\{i}.vst3",
                             instrument=rng.random() < 0.5)
            items.append(BrowserItem(name, ref_.path, "plugin", detail, ref_))
        else:
            items.append(BrowserItem(name, f"dev{i}", "device", detail))
    return items


def tag_of(item: BrowserItem) -> str:
    return item.detail[:3]


@pytest.mark.parametrize("seed", range(6))
def test_search_orders_as_python(seed, tmp_path, backends):
    rng = random.Random(seed)
    clock_now = [1_000_000_000.0]
    library = Library(tmp_path / "library.json", lambda: clock_now[0])
    items = random_items(rng, 400)
    backend = backends(None)
    backend.native.set_external(5, [(KINDS.index(i.kind), i.name, i.path, i.detail, i.key, tag_of(i))
                                    for i in items])
    for round_ in range(60):
        if rng.random() < 0.3:  # some uses, at various times; some with odd records
            clock_now[0] += rng.choice([0.0, 3600.0, DAY * 40])
            library.record_use([rng.choice(items).key for _ in range(rng.randint(1, 4))])
            if rng.random() < 0.2:
                library.records[rng.choice(items).key] = {"hidden": True}  # a record without uses
        backend.native.set_usage(usage_records(library), HALF_LIFE_DAYS)
        query = random_query(rng)
        sort = rng.choice(["rank", "name"])
        tag = rng.choice(["", "", "Dru", "Ven", "Aud"])
        expected = ref.find([i for i in items if not tag or tag_of(i) == tag], query, library, sort)
        result = backend.search(query, sort, library.clock(), groups=[5], tag=tag)
        got = [row[4] for row in result.rows(0, result.total)]
        assert got == [i.key for i in expected], (round_, query, sort, tag)


# --- Indexing: as the Python walk did -----------------------------------------------------

def reference_items(places, **limits) -> list[BrowserItem]:
    return ref.index_places([str(p) for p in places], **limits)


def build_tree(root: Path, rng: random.Random, folders: int = 25, files: int = 30) -> None:
    """Folders nested a few deep, many names shared between folders (and in
    different case), files that are not audio, and names the browser skips."""
    dirs = [root]
    for i in range(folders):
        parent = rng.choice(dirs)
        name = rng.choice(["Kicks", "Loops", "Pack", "ΣΑ", "İ", "Straße", "Café", "a b", "x.y"]) + f" {i}"
        dirs.append(parent / name)
        dirs[-1].mkdir(parents=True)
    for folder in dirs:
        used = set()
        for _ in range(rng.randint(0, files)):
            name = random_name(rng) + rng.choice([".wav", ".WAV", ".mp3", ".flac", ".wave", ".txt", ".asd",
                                                   ".wav.asd", ""])
            if name.lower() in used or name.strip(". ") != name:
                continue
            used.add(name.lower())
            touch(folder / name)
    touch(root / ".hidden.wav")
    touch(root / "$recycled.wav")
    touch(root / ".git" / "inside.wav")
    touch(root / "$RECYCLE.BIN" / "gone.wav")


def test_index_lists_as_python(tmp_path, backends):
    rng = random.Random(7)
    build_tree(tmp_path / "lib", rng)
    backend = backends([tmp_path / "lib"])
    backend.wait()
    expected = reference_items([tmp_path / "lib"])
    assert len(expected) > 100
    assert backend.items() == expected  # same files, details and paths, in the same order
    assert backend.native.file_count == len(expected)


def test_index_limits_as_python(tmp_path, backends):
    rng = random.Random(8)
    lib = tmp_path / "lib"
    build_tree(lib, rng, folders=40, files=12)
    deep = lib
    for i in range(8):  # deeper than the limit below
        deep = deep / f"level{i}"
        touch(deep / f"deep{i}.wav")
    for max_files, max_depth in [(50, 16), (10**6, 3), (7, 2), (1, 16)]:
        backend = backends([lib], max_files=max_files, max_depth=max_depth)
        backend.wait()
        assert backend.items() == reference_items([lib], max_files=max_files, max_depth=max_depth), \
            (max_files, max_depth)


@windows_only
def test_index_walks_junctions_not_symlinks(tmp_path, backends):
    lib = tmp_path / "lib"
    target = tmp_path / "elsewhere"
    touch(target / "Linked.wav")
    touch(lib / "Real.wav")
    subprocess.run(["cmd", "/c", "mklink", "/J", str(lib / "junction"), str(target)], check=True, capture_output=True)
    made = subprocess.run(["cmd", "/c", "mklink", "/D", str(lib / "dirlink"), str(target)], check=False,
                          capture_output=True)  # needs developer mode
    subprocess.run(["cmd", "/c", "mklink", str(lib / "filelink.wav"), str(lib / "Real.wav")], check=False,
                   capture_output=True)
    backend = backends([lib])
    backend.wait()
    names = [i.name for i in backend.items()]
    assert "Linked.wav" in names  # through the junction
    if made.returncode == 0:
        assert names.count("Linked.wav") == 1  # not through the symbolic link
    assert backend.items() == reference_items([lib])


def test_places_overlapping_or_missing(tmp_path, backends):
    rng = random.Random(9)
    lib = tmp_path / "lib"
    build_tree(lib, rng)
    inner = next(p for p in sorted(lib.iterdir()) if p.is_dir() and not p.name.startswith((".", "$")))
    for places in ([lib, inner], [inner, lib], [tmp_path / "missing", lib], [inner]):
        backend = backends(places)
        backend.wait()
        assert backend.items() == reference_items(places), places


def test_place_and_all_scopes(tmp_path, backends):
    rng = random.Random(10)
    lib = tmp_path / "lib"
    build_tree(lib, rng)
    backend = backends([lib])
    backend.wait()
    items = reference_items([lib])
    library = Library(tmp_path / "library.json", lambda: 1e9)
    library.record_use([i.key for i in rng.sample(items, 10)])
    backend.native.set_usage(usage_records(library), HALF_LIFE_DAYS)
    folders = sorted({str(Path(i.path).parent) for i in items})
    for query in ["", "kick", "e", "808 bass", "σ", "straße", "zz"]:
        for sort in ("rank", "name"):
            got = backend.items(text=query, sort=sort, now=1e9)
            assert got == ref.find(items, query, library, sort), (query, sort)
            place = rng.choice(folders)
            got = backend.items(text=query, sort=sort, now=1e9, prefix=place_prefix(place))
            assert got == ref.find(ref.place_items(items, place), query, library, sort), (query, sort, place)


# --- Keeping up to date -----------------------------------------------------------------

@windows_only
def test_saved_index_is_checked_not_listed_again(tmp_path, backends):
    rng = random.Random(11)
    lib = tmp_path / "lib"
    build_tree(lib, rng)
    store = tmp_path / "state" / "index.bin"
    first = backends([lib], store=store)
    first.wait()
    before = first.items()
    first.close()
    assert store.exists()

    # Nothing changed: what was saved is shown, and nothing is published again.
    again = backends([lib], store=store)
    again.wait()
    assert again.items() == before and again.native.version == 1
    again.close()

    # Changes while closed: files added and removed, a folder renamed, one made.
    folders = sorted(p for p in lib.rglob("*") if p.is_dir() and not p.name.startswith((".", "$")))
    touch(folders[0] / "Added Kick.wav")
    removed = next(p for p in lib.rglob("*.wav") if not any(s.startswith((".", "$")) for s in p.parts))
    removed.unlink()
    folders[-1].rename(folders[-1].with_name("Renamed Folder"))
    touch(lib / "New Pack" / "Deep" / "New Snare.wav")
    changed = backends([lib], store=store)
    changed.wait()
    assert changed.items() == reference_items([lib])


@windows_only
def test_rescan_lists_every_folder_again(tmp_path, backends):
    lib = tmp_path / "lib"
    touch(lib / "Drums" / "Kick.wav")
    store = tmp_path / "index.bin"
    first = backends([lib], store=store)
    first.wait()
    first.close()
    # A file the folder's time doesn't tell about (the time is put back).
    stat = (lib / "Drums").stat()
    touch(lib / "Drums" / "Snare.wav")
    os.utime(lib / "Drums", ns=(stat.st_atime_ns, stat.st_mtime_ns))
    backend = backends([lib], store=store)
    backend.wait()
    assert [i.name for i in backend.items()] == ["Kick.wav"]  # checked by time only
    backend.native.rescan()
    backend.wait()
    assert [i.name for i in backend.items()] == ["Kick.wav", "Snare.wav"]


def wait_for(predicate, timeout=10.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.02)
    return False


@windows_only
def test_changes_in_places_are_seen(tmp_path, backends):
    lib = tmp_path / "lib"
    touch(lib / "Drums" / "Kick.wav")
    backend = backends([lib])
    backend.wait()

    def names():
        return [i.name for i in backend.items()]

    touch(lib / "Drums" / "Snare.wav")
    assert wait_for(lambda: names() == ["Kick.wav", "Snare.wav"])
    touch(lib / "Pack" / "Sub" / "Deeper" / "Hat.wav")  # new folders, several deep
    assert wait_for(lambda: "Hat.wav" in names())
    (lib / "Drums" / "Kick.wav").rename(lib / "Drums" / "Big Kick.wav")
    assert wait_for(lambda: names() == ["Big Kick.wav", "Hat.wav", "Snare.wav"])
    (lib / "Pack").rename(lib / "Moved")
    assert wait_for(lambda: backend.items() == reference_items([lib]))
    touch(lib / ".git" / "objects" / "x.wav")  # never listed
    assert backend.items() == reference_items([lib])


def test_places_change_incrementally(tmp_path, backends):
    touch(tmp_path / "a" / "One.wav")
    touch(tmp_path / "b" / "Two.wav")
    backend = backends([tmp_path / "a"])
    backend.wait()
    backend.set_places([tmp_path / "a", tmp_path / "b"])
    backend.wait()
    assert backend.items() == reference_items([tmp_path / "a", tmp_path / "b"])
    backend.set_places([tmp_path / "b"])
    backend.wait()
    assert [i.name for i in backend.items()] == ["Two.wav"]


@pytest.mark.parametrize("damage", ["truncate", "garbage", "flip"])
def test_damaged_index_is_ignored(tmp_path, backends, damage):
    touch(tmp_path / "lib" / "Kick.wav")
    store = tmp_path / "index.bin"
    first = backends([tmp_path / "lib"], store=store)
    first.wait()
    first.close()
    data = bytearray(store.read_bytes())
    if damage == "truncate":
        data = data[: len(data) // 2]
    elif damage == "garbage":
        data = bytearray(os.urandom(len(data)))
    else:
        data[len(data) // 2] ^= 0xFF
    store.write_bytes(bytes(data))
    backend = backends([tmp_path / "lib"], store=store)
    backend.wait()
    assert [i.name for i in backend.items()] == ["Kick.wav"]


# --- Searching off the caller's thread ---------------------------------------------------

@pytest.fixture
def big_library(tmp_path):
    rng = random.Random(12)
    for f in range(60):
        for i in range(60):
            touch(tmp_path / "lib" / f"Folder {f}" / (random_name(rng) + f" {i}.wav"))
    return tmp_path / "lib"


def test_only_the_latest_search_is_handed_out(big_library, backends):
    backend = backends([big_library])
    backend.wait()
    generations = [backend.native.search(q, "rank", 0.0, [AUDIO]) for q in ["k", "ki", "kic", "kick", "e", "x"]]
    assert generations == sorted(generations)
    backend.wait()
    result = backend.native.take()[3]
    assert result.generation == generations[-1]
    assert backend.native.take()[3] is None  # handed out once
    # Results of a search replaced before they were taken are dropped.
    backend.native.search("kick", "rank", 0.0, [AUDIO])
    backend.wait()
    latest = backend.native.search("e", "name", 0.0, [AUDIO])
    taken = backend.native.take()[3]
    assert taken is None or taken.generation == latest


def test_results_come_in_pages(big_library, backends):
    backend = backends([big_library])
    backend.wait()
    result = backend.search("e", "name")
    rows = result.rows(0, result.total)
    assert result.total == len(rows) > 1000
    assert result.rows(100, 50) == rows[100:150]
    assert result.rows(result.total - 3, 50) == rows[-3:] and result.rows(result.total + 5, 5) == []
    source = SearchResult(result, {})
    model = ItemListModel()
    model.set_source(source)
    assert model.total == result.total and model.rowCount() == ItemListModel.PAGE
    assert model.canFetchMore()
    model.fetchMore()
    assert model.rowCount() == 2 * ItemListModel.PAGE
    model.ensure_rows(result.total + 10)
    assert model.rowCount() == result.total and not model.canFetchMore()
    item = model.item(model.index(700))
    assert source.find(item) == 700


def test_waiting_releases_the_gil(big_library, backends):
    backend = backends([big_library])
    backend.wait()
    count = [0]
    stop = threading.Event()

    def spin():
        while not stop.is_set():
            count[0] += 1

    thread = threading.Thread(target=spin)
    thread.start()
    try:
        assert wait_for(lambda: count[0] > 0)  # spinning
        during_wait = 0
        for _ in range(3):
            backend.native.rescan()
            start = count[0]
            backend.wait()  # the Python thread runs meanwhile
            during_wait += count[0] - start
    finally:
        stop.set()
        thread.join()
    assert during_wait > 1000
