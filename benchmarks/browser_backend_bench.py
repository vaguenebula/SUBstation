"""The browser's backend alone, before (the Python index and search, kept in
tests/browser_reference.py) and after (the native substation._browser), on a
large library. Every query's results are compared item by item as well.

    python -m benchmarks.browser_backend_bench [--size 200000] [--audio] [--json out.json]

Index     building it (the Python walk; the native scan with a new index file),
          and starting again with the saved one (native only)
Query     the time a search takes: for Python that is time the UI thread was
          blocked (including working out item keys the first time after indexing);
          for the native backend, on its thread, and from asking until the
          results can be taken on the caller's thread
Indexing  native queries while a full rescan runs
With --audio the engine plays (silently) meanwhile; see browser_ui_bench."""

from __future__ import annotations

import argparse
import json
import os
import random
import statistics
import sys
import tempfile
import threading
import time
from pathlib import Path

from benchmarks.library_gen import make_library
from substation import _browser
from substation.audio.engine_bridge import AUDIO_EXTENSIONS
from substation.ui.browser.browser_models import BrowserItem
from substation.ui.browser.file_index import (
    MAX_DEPTH,
    MAX_FILES,
    SearchResult,
    place_spec,
    usage_records,
)
from substation.ui.browser.library import HALF_LIFE_DAYS, Library
from substation.ui.browser.search import place_prefix
from tests import browser_reference as ref

QUERIES = ["", "kick", "e", "808 bass", "kick deep", "zzqx"]
NOW = 1_000_000_000.0


def timed(fn, *args):
    start = time.perf_counter()
    result = fn(*args)
    return result, (time.perf_counter() - start) * 1000


def fresh(items: list[BrowserItem]) -> list[BrowserItem]:
    """The same items without their cached keys, as right after indexing."""
    return [BrowserItem(i.name, i.path, i.kind, i.detail) for i in items]


def native_query(backend, text, sort, prefix="") -> tuple[object, float]:
    start = time.perf_counter()
    backend.search(text, sort, NOW, [_browser.AUDIO], "", prefix)
    while (result := backend.take()[3]) is None:
        time.sleep(0.0002)
    return result, (time.perf_counter() - start) * 1000


def run(size: int, audio: bool, used: int) -> dict:
    root = make_library(size)
    tmp = Path(tempfile.mkdtemp(prefix="sub-backend-bench-"))
    report: dict = {"size": size, "library": str(root), "used_records": used, "cpu_count": os.cpu_count()}
    playback = Playback() if audio else None

    # --- Index -------------------------------------------------------------------------
    items, wall = timed(ref.index_places, [str(root)])
    report["index"] = {"python": {"wall_ms": round(wall), "files": len(items)}}
    store = tmp / "index.bin"

    def open_native():
        backend = _browser.Browser(str(store), list(AUDIO_EXTENSIONS), MAX_FILES, MAX_DEPTH)
        backend.set_places([place_spec(str(root))])
        return backend

    def settle(backend):
        start = time.perf_counter()
        first = None
        while backend.indexing or not backend.version:
            if first is None and backend.file_count:
                first = (time.perf_counter() - start) * 1000
            time.sleep(0.0005)
        return first, (time.perf_counter() - start) * 1000

    backend = open_native()
    first, wall = settle(backend)
    report["index"]["native_new"] = {"first_files_ms": round(first or wall), "wall_ms": round(wall),
                                     **backend.stats}
    backend.close()
    backend = open_native()
    first, wall = settle(backend)
    report["index"]["native_restart"] = {"first_files_ms": round(first or wall), "wall_ms": round(wall),
                                         **backend.stats}

    # --- Query -------------------------------------------------------------------------
    library = Library(tmp / "library.json", lambda: NOW)
    rng = random.Random(1)
    library.record_use([i.key for i in rng.sample(items, used)])
    backend.set_usage(usage_records(library), HALF_LIFE_DAYS)
    place = str(Path(items[len(items) // 2].path).parents[2])  # a pack's folder
    rows = []

    def python_find(listed, scope, text, sort):  # as the panel did it, on the UI thread
        return ref.find(ref.place_items(listed, place) if scope == "place" else listed, text, library, sort)

    for scope in ("samples", "place"):
        prefix = place_prefix(place) if scope == "place" else ""
        for sort in ("rank", "name"):
            for text in QUERIES:
                first = fresh(items)
                _, python_first = timed(python_find, first, scope, text, sort)
                expected, python = timed(python_find, first, scope, text, sort)
                result, end_to_end = native_query(backend, text, sort, prefix)
                page_start = time.perf_counter()
                page = SearchResult(result, {}).items(0, 256)
                page_ms = (time.perf_counter() - page_start) * 1000
                same = [r[2] for r in result.rows(0, result.total)] == [i.path for i in expected]
                rows.append({"scope": scope, "sort": sort, "query": text, "results": result.total,
                             "python_ms": round(python, 1), "python_first_ms": round(python_first, 1),
                             "native_search_ms": round(result.search_ms, 2),
                             "native_end_to_end_ms": round(end_to_end, 2), "first_page_ms": round(page_ms, 2),
                             "same_results": same and page == expected[:256]})
    report["query"] = rows
    report["all_results_same"] = all(r["same_results"] for r in rows)

    # --- While indexing -------------------------------------------------------------------
    during = []
    backend.rescan()
    for text in QUERIES * 3:
        if not backend.indexing:
            break
        result, end_to_end = native_query(backend, text, "rank")
        during.append({"query": text, "native_search_ms": round(result.search_ms, 2),
                       "native_end_to_end_ms": round(end_to_end, 2), "indexing": backend.indexing})
    backend.wait_idle(60)
    report["indexing"] = during
    backend.close()
    if playback is not None:
        report["playback"] = playback.stop()
    return report


class Playback:
    """The engine playing silently; its playhead against the wall clock, sampled
    on a Python thread (see browser_ui_bench.Playback)."""

    def __init__(self):
        from benchmarks.browser_ui_bench import _start_playback
        from substation import _engine as ge

        self.engine = ge.Engine()
        _start_playback(self.engine)
        self.samples = []
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _run(self):
        while not self._stop.wait(0.02):
            seconds = self.engine.position_beats * 60.0 / self.engine.tempo
            self.samples.append((time.perf_counter(), seconds, self.engine.cpu_load))

    def stop(self) -> dict:
        self._stop.set()
        self._thread.join()
        lag, ahead = 0.0, None
        for wall, played, _ in self.samples:
            offset = played - wall
            ahead = offset if ahead is None else max(ahead, offset)
            lag = max(lag, ahead - offset)
        loads = [load for *_, load in self.samples]
        status = self.engine.device_status
        self.engine.stop()
        self.engine.close_device()
        return {"cpu_load_max": round(max(loads), 3), "cpu_load_median": round(statistics.median(loads), 3),
                "playhead_max_lag_ms": round(lag * 1000, 1), "device": status.name,
                "buffer_ms": round(1000 * status.buffer_frames / self.engine.sample_rate, 1)}


def summary(report: dict) -> str:
    lines = [f"{report['size']} files"]
    index = report["index"]
    lines.append(f"index: python {index['python']['wall_ms']} ms; native new {index['native_new']['wall_ms']} ms "
                 f"(first files {index['native_new']['first_files_ms']} ms), restart "
                 f"{index['native_restart']['wall_ms']} ms (first files {index['native_restart']['first_files_ms']} ms)")
    for r in report["query"]:
        lines.append(f"  {r['scope']:7} {r['sort']:4} {r['query']!r:11} {r['results']:7}  python {r['python_ms']:7.1f} "
                     f"(first {r['python_first_ms']:7.1f})  native {r['native_search_ms']:6.2f} / "
                     f"{r['native_end_to_end_ms']:6.2f} ms  page {r['first_page_ms']:5.2f}  same {r['same_results']}")
    lines.append("while indexing: " + ", ".join(f"{r['native_end_to_end_ms']}" for r in report["indexing"]))
    if "playback" in report:
        lines.append(f"playback: {report['playback']}")
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--size", type=int, default=200_000)
    parser.add_argument("--used", type=int, default=500, help="items with use counts")
    parser.add_argument("--audio", action="store_true")
    parser.add_argument("--json")
    args = parser.parse_args()
    report = run(args.size, args.audio, args.used)
    print(summary(report))
    if args.json:
        Path(args.json).write_text(json.dumps(report, indent=1), encoding="utf-8")
    if not report["all_results_same"]:
        sys.exit("native results differ from the Python ones")


if __name__ == "__main__":
    main()
