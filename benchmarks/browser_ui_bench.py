"""The browser as the user sees it: how long until results appear, and how long
the UI thread stalls, on a large library. Runs against the real BrowserPanel.

    python -m benchmarks.browser_ui_bench [--size 200000] [--audio] [--json out.json] [--code DIR]

Scenarios
  start      a new panel with an empty index: time until the first results show in
             Samples, until indexing is done, and the longest UI stall meanwhile
  restart    a second panel (the first one saved its index): time to first results
  query      search text set at once (as when pasting): time until the results are
             laid out and painted, per query, scope and sort
  typing     "kick deep" typed at 110 ms a key: time from the last key to the final
             results, and the longest stall while typing
  indexing   the queries while a full rescan runs
With --audio the engine plays (silently: master gain 0) through the default output
the whole time. Its CPU load is sampled, and how far its playhead fell behind the
wall clock at worst (a late audio callback is a dropout, and leaves it behind).

Only surfaces that the browser had before and after its native backend are used,
so the same script measures both: --code runs it with another copy of the
gilstudio package (say, from before; see benchmarks/README.md). Nothing touches the
user's settings, library or plug-ins: settings, use counts, the index and the
plug-in paths are all temporary."""

from __future__ import annotations

import argparse
import json
import os
import statistics
import sys
import tempfile
import time
from pathlib import Path


def _parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--size", type=int, default=200_000)
    parser.add_argument("--audio", action="store_true", help="play (silently) through the default output")
    parser.add_argument("--json", help="write the report here")
    parser.add_argument("--code", help="a folder holding the gilstudio package to measure instead")
    return parser.parse_args()


def _use_code(folder: str) -> None:
    """Import gilstudio from `folder` instead of the (editable) installation."""
    sys.meta_path[:] = [f for f in sys.meta_path if not type(f).__module__.startswith("_editable_")]
    sys.path.insert(0, folder)


if __name__ == "__main__":
    ARGS = _parse_args()
    if ARGS.code:
        _use_code(ARGS.code)

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
_tmp = Path(tempfile.mkdtemp(prefix="gil-ui-bench-"))
os.environ["GILSTUDIO_LIBRARY"] = str(_tmp / "library.json")
os.environ["GILSTUDIO_BROWSER_INDEX"] = str(_tmp / "browser-index.bin")
os.environ["GILSTUDIO_PLUGIN_CACHE"] = str(_tmp / "vst3-cache.json")
os.environ["GILSTUDIO_VST3_PATH"] = str(_tmp / "no-plugins")
(_tmp / "no-plugins").mkdir()

from PySide6.QtCore import (  # noqa: E402
    QCoreApplication,
    QElapsedTimer,
    QSettings,
    QTimer,
    qInstallMessageHandler,
)
from PySide6.QtWidgets import QApplication  # noqa: E402

# The offscreen platform (no windows) says so about every window; nothing to see.
_OFFSCREEN_NOISE = ("does not support propagateSizeHints", "Cannot find font directory", "Qt no longer ships fonts")
_previous_handler = None


def _quiet(mode, context, message):
    if any(noise in message for noise in _OFFSCREEN_NOISE):
        return
    if _previous_handler is not None:
        _previous_handler(mode, context, message)
    else:
        print(message, file=sys.stderr)


_previous_handler = qInstallMessageHandler(_quiet)

from benchmarks.library_gen import make_library  # noqa: E402

QUERIES = ["kick", "e", "808 bass", "kick deep", "zzqx"]


class StallMeter:
    """Gaps between ticks of a 1 ms timer on the UI thread: how long it could not run."""

    def __init__(self):
        self.clock = QElapsedTimer()
        self.timer = QTimer(interval=1)
        self.timer.timeout.connect(self._tick)
        self.gaps: list[float] = []
        self._last = 0.0

    def _tick(self):
        now = self.clock.nsecsElapsed() / 1e6
        self.gaps.append(now - self._last)
        self._last = now

    def start(self):
        self.gaps = []
        self.clock.start()
        self._last = 0.0
        self.timer.start()

    def stop(self) -> dict:
        self.timer.stop()
        self._tick()  # the gap still open (a stall that just ended)
        gaps = sorted(self.gaps)
        return {"max_stall_ms": round(gaps[-1], 1), "p99_stall_ms": round(gaps[int(len(gaps) * 0.99)], 1)}


class Playback:
    """The engine playing, and how it keeps up: CPU load, and the playhead
    against the wall clock. Sampled on the UI thread, but each sample reads both
    at once, so a stalled UI thread takes fewer samples, not wrong ones."""

    def __init__(self, engine):
        self.engine = engine
        self.samples: list[tuple[float, float, float]] = []  # wall s, playhead s, CPU load
        self.timer = QTimer(interval=20)
        self.timer.timeout.connect(self._sample)

    def _sample(self):
        seconds = self.engine.position_beats * 60.0 / self.engine.tempo
        self.samples.append((time.perf_counter(), seconds, self.engine.cpu_load))

    def start(self):
        self.timer.start()

    def stop(self) -> dict:
        self.timer.stop()
        lag = 0.0
        ahead = None
        for wall, played, _ in self.samples:
            offset = played - wall
            ahead = offset if ahead is None else max(ahead, offset)
            lag = max(lag, ahead - offset)
        loads = [load for _, _, load in self.samples]
        return {"cpu_load_max": round(max(loads), 3), "cpu_load_median": round(statistics.median(loads), 3),
                "playhead_max_lag_ms": round(lag * 1000, 1), "samples": len(self.samples),
                "device": self.engine.device_status.name,
                "buffer_ms": round(1000 * self.engine.device_status.buffer_frames / self.engine.sample_rate, 1)}


def pump(app, seconds: float = 0.0):
    deadline = time.perf_counter() + seconds
    while True:
        app.processEvents()
        if time.perf_counter() >= deadline:
            return
        time.sleep(0.0005)


def wait_until(app, predicate, timeout: float = 120.0) -> bool:
    deadline = time.perf_counter() + timeout
    while time.perf_counter() < deadline:
        if predicate():
            return True
        app.processEvents()
        time.sleep(0.0005)
    return False


def indexed_files(browser) -> int:
    index = browser.index
    return index.file_count if hasattr(index, "file_count") else len(index.audio)


def select(browser, scope: tuple) -> None:
    from gilstudio.ui.browser.browser_panel import ROLE_SCOPE

    sidebar = browser.sidebar
    items = [sidebar.topLevelItem(i) for i in range(sidebar.topLevelItemCount())]
    items += [item.child(j) for item in list(items) for j in range(item.childCount())]
    for item in items:
        if tuple(item.data(0, ROLE_SCOPE) or ()) == scope:
            sidebar.setCurrentItem(item)
            return
    raise KeyError(scope)


def _searching(browser) -> bool:
    return bool(getattr(browser, "searching", False)) or browser._search_timer.isActive()


def _total(browser) -> int:
    model = browser.list_model
    return model.total if hasattr(model, "total") else model.rowCount()


class ResultWatch:
    """When the list changed, and when its first rows were painted after."""

    def __init__(self, browser):
        self.browser = browser
        self.resets = 0
        browser.list_model.modelReset.connect(self._reset)

    def _reset(self):
        self.resets += 1

    def settle_ms(self, app, since: float, resets_before: int, timeout: float = 60.0) -> float | None:
        """Time from `since` until the list was reset (with the new results) and painted."""
        if not wait_until(app, lambda: self.resets > resets_before and not _searching(self.browser), timeout):
            return None
        self.browser.list_view.viewport().repaint()  # the layout and paint the user waits for
        return (time.perf_counter() - since) * 1000.0


def query_once(app, browser, watch: ResultWatch, text: str) -> dict:
    browser.search.setText("")
    wait_until(app, lambda: not _searching(browser))
    pump(app, 0.05)
    before = watch.resets
    start = time.perf_counter()
    browser.search.setText(text)
    ms = watch.settle_ms(app, start, before)
    return {"query": text, "ms": None if ms is None else round(ms, 1), "rows_total": _total(browser)}


def new_panel(bridge):
    from gilstudio.ui.browser.browser_panel import BrowserPanel

    browser = BrowserPanel(bridge)
    browser.resize(320, 800)
    browser.show()
    return browser, ResultWatch(browser)


def run(size: int, audio: bool) -> dict:
    root = make_library(size)
    QCoreApplication.setOrganizationName("GIL Studio Bench")
    QCoreApplication.setApplicationName("GIL Studio Bench")
    app = QApplication.instance() or QApplication([])
    settings = QSettings()
    settings.clear()
    settings.setValue("browser/places", [str(root)])
    settings.setValue("browser/sort", "rank")

    import gilstudio
    from gilstudio import _engine as ge
    from gilstudio.audio.engine_bridge import EngineBridge
    from gilstudio.model.project import Project
    from gilstudio.ui.browser import (
        browser_panel,  # noqa: F401  (imported before anything is timed)
    )

    engine = ge.Engine()
    bridge = EngineBridge(engine, Project())
    report: dict = {"size": size, "library": str(root), "audio": audio, "python": sys.version.split()[0],
                    "code": str(Path(gilstudio.__file__).parent)}
    playback = Playback(engine)
    if audio:
        _start_playback(engine)
        playback.start()

    stall = StallMeter()

    # --- start: empty index ----------------------------------------------------------
    stall.start()
    t0 = time.perf_counter()
    browser, watch = new_panel(bridge)
    select(browser, ("samples",))
    wait_until(app, lambda: browser.list_model.rowCount() > 0, 300)
    first = time.perf_counter() - t0
    wait_until(app, lambda: not browser.index.indexing and indexed_files(browser) >= size, 600)
    done = time.perf_counter() - t0
    report["start"] = {"first_results_ms": round(first * 1000), "indexed_ms": round(done * 1000),
                       "files": indexed_files(browser), **stall.stop()}
    pump(app, 0.5)

    # --- restart: a second panel, reusing what the first one saved --------------------
    browser.shutdown()
    browser.deleteLater()
    pump(app, 0.2)
    stall.start()
    t0 = time.perf_counter()
    browser, watch = new_panel(bridge)
    select(browser, ("samples",))
    wait_until(app, lambda: browser.list_model.rowCount() > 0, 300)
    first = time.perf_counter() - t0
    wait_until(app, lambda: not browser.index.indexing and indexed_files(browser) >= size, 600)
    report["restart"] = {"first_results_ms": round(first * 1000),
                         "indexed_ms": round((time.perf_counter() - t0) * 1000), **stall.stop()}
    if hasattr(browser.index, "native"):  # the native index's own account
        report["restart"]["index"] = {k: round(v, 1) for k, v in browser.index.native.stats.items()}
    pump(app, 0.5)

    # --- query --------------------------------------------------------------------------
    queries = []
    stall.start()
    for scope in (("samples",), ("all",), ("place", str(root))):
        select(browser, scope)
        pump(app, 0.1)
        for sort in ("rank", "name"):
            browser.sort.setCurrentIndex(browser.sort.findData(sort))
            for text in QUERIES:
                queries.append({"scope": scope[0], "sort": sort, **query_once(app, browser, watch, text)})
    report["query"] = {"results": queries, **stall.stop()}
    times = [q["ms"] for q in queries if q["ms"] is not None]
    report["query"]["median_ms"] = round(statistics.median(times), 1)
    report["query"]["max_ms"] = round(max(times), 1)

    # --- typing ---------------------------------------------------------------------------
    select(browser, ("samples",))
    browser.sort.setCurrentIndex(browser.sort.findData("rank"))
    browser.search.setText("")
    wait_until(app, lambda: not _searching(browser))
    pump(app, 0.2)
    stall.start()
    typed = "kick deep"
    for i in range(1, len(typed)):
        browser.search.setText(typed[:i])
        pump(app, 0.110)
    before = watch.resets
    last = time.perf_counter()
    browser.search.setText(typed)
    final = watch.settle_ms(app, last, before)
    report["typing"] = {"after_last_key_ms": None if final is None else round(final, 1), **stall.stop()}

    # --- during indexing --------------------------------------------------------------------
    during = []
    stall.start()
    browser.index.rebuild(browser.places)
    pump(app, 0.05)
    for text in QUERIES:
        if not browser.index.indexing:
            break
        during.append({**query_once(app, browser, watch, text), "indexing_at_end": browser.index.indexing})
    wait_until(app, lambda: not browser.index.indexing, 600)
    report["indexing"] = {"results": during, **stall.stop()}

    if audio:
        report["playback"] = playback.stop()
        engine.stop()
        engine.close_device()
    browser.shutdown()
    bridge.shutdown()
    return report


def _start_playback(engine) -> None:
    """Play an arrangement of a few tones, silently (master gain 0), from the
    start and without looping, so the playhead follows the wall clock."""
    import wave

    import numpy as np

    from gilstudio import _engine as ge

    engine.open_device()
    engine.set_master_gain(0.0)
    for i in range(8):
        t = np.arange(48000 * 4) / 48000
        pcm = np.round(0.3 * np.sin(2 * np.pi * 110 * (i + 1) * t) * 32767).astype("<i2")
        path = _tmp / f"tone{i}.wav"
        with wave.open(str(path), "wb") as f:
            f.setnchannels(1)
            f.setsampwidth(2)
            f.setframerate(48000)
            f.writeframes(pcm.tobytes())
        engine.load_source(str(path))
        track = engine.add_track()
        warp = i % 2 == 1  # half of them time-stretched, the heavier path
        clips = [ge.ClipDesc(str(path), beat, 4.0, 0.0, 1.0, warp=warp, segment_bpm=100.0 if warp else 0.0)
                 for beat in range(0, 4000, 8)]
        engine.set_track_clips(track, clips)
    engine.play()


def main() -> None:
    report = run(ARGS.size, ARGS.audio)
    text = json.dumps(report, indent=1)
    print(text)
    if ARGS.json:
        Path(ARGS.json).write_text(text, encoding="utf-8")


if __name__ == "__main__":
    main()
