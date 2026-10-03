"""What it costs to refresh the oscilloscope and the built-in editors' displays at 60 fps
instead of ~30 fps (the oscilloscope's 33 ms timer; the editors follow the 33 ms meter timer).

    python -m benchmarks.display_fps_bench [--seconds 3] [--json out.json]

For each display the real widget is built offscreen and one refresh is timed the way a
timer tick runs it: the poll/update step plus a synchronous paint (`render()` into a reused
image; `repaint()` does nothing on the offscreen platform), which is the UI-thread work the
tick causes. Median and 95th percentile per refresh, then the
share of one core that 30 and 60 refreshes a second would take (`cost x rate`) and the
difference. It does not include the window system's compositing or the engine's audio
thread, which are the same at either rate except for the number of frames presented.

Also runs all displays together under a real 1 ms-timer stall probe at 30 and at 60 Hz
(QTimer-driven, like the app) to read the longest stretch the UI thread couldn't run."""

from __future__ import annotations

import argparse
import json
import os
import statistics
import sys
import time
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

import numpy as np  # noqa: E402
from PySide6.QtCore import QCoreApplication, QElapsedTimer, QTimer  # noqa: E402
from PySide6.QtGui import QImage  # noqa: E402
from PySide6.QtWidgets import QApplication  # noqa: E402

from substation.ui.device_editors import compressor, sampler  # noqa: E402
from substation.ui.widgets.oscilloscope import Oscilloscope  # noqa: E402


class FakeEngine:
    """Stands in for the engine's master scope: a new 8192-sample chord on every poll."""

    def __init__(self):
        self.master_scope_written = 0
        t = np.arange(8192, dtype=np.float32) / 48000
        self._wave = (0.4 * np.sin(2 * np.pi * 220 * t) + 0.3 * np.sin(2 * np.pi * 330 * t)
                      + 0.1 * np.random.default_rng(1).standard_normal(8192)).astype(np.float32)

    def tick(self):
        self.master_scope_written += 512

    def master_scope(self, n):
        return np.roll(self._wave, -self.master_scope_written)[-n:].copy()


class Case:
    """One display: `step()` is what a timer tick does, `widget` what it repaints."""

    def __init__(self, name, widget, step):
        self.name, self.widget, self.step = name, widget, step
        self.image = QImage(widget.size(), QImage.Format.Format_ARGB32_Premultiplied)

    def refresh(self):
        self.step()
        self.widget.render(self.image)


def _cases():
    engine = FakeEngine()
    scope = Oscilloscope(engine)
    scope._timer.stop()
    scope.show()

    def scope_step():
        engine.tick()
        scope._poll()

    rng = np.random.default_rng(2)
    graph = compressor.ReductionGraph(lambda: -18.0)
    graph.resize(graph.width(), 130)
    graph.show()

    def graph_step():
        graph.add(np.abs(rng.standard_normal(3)).astype(np.float32) * 6,
                  rng.uniform(-40, -3, 3).astype(np.float32), rng.uniform(-40, -3, 3).astype(np.float32))

    class Source:
        path, frames = "bench.wav", 480_000
        peaks = None

    class Stub:  # what SampleView asks of its device widget
        values = {"start": 10.0, "end": 90.0, "loop": 0.0}

        def sample_path(self): return "bench.wav"
        def sample_source(self): return Source
        def load_error(self): return None
        def value(self, pid): return self.values[pid]

    # Waveform columns are computed once per width/length and cached by the view; give it
    # them directly so the repaint measures drawing, not the one-time peak reduction.
    view = sampler.SampleView(Stub())
    width = max(1, int(view._plot().width()))
    lo = -np.abs(rng.standard_normal(width)).clip(0, 1).astype(np.float32)
    view._columns = (("bench.wav", Source.frames, width), (lo, -lo))
    view.resize(sampler.VIEW_WIDTH, 100)
    view.show()
    head = [0.0]

    def view_step():
        head[0] = (head[0] + 0.004) % 1.0
        view.set_playhead(head[0])

    return [Case("oscilloscope", scope, scope_step), Case("compressor graph", graph, graph_step),
            Case("sampler view", view, view_step)]


def _time_case(case: Case, seconds: float) -> dict:
    for _ in range(50):
        case.refresh()
    samples = []
    end = time.perf_counter() + seconds
    while time.perf_counter() < end:
        t0 = time.perf_counter()
        case.refresh()
        samples.append(time.perf_counter() - t0)
    samples.sort()
    median, p95 = statistics.median(samples), samples[int(len(samples) * 0.95)]
    return {"median_ms": median * 1e3, "p95_ms": p95 * 1e3, "n": len(samples),
            "cpu_pct_30": median * 30 * 100, "cpu_pct_60": median * 60 * 100,
            "extra_cpu_pct": median * 30 * 100}


def _stall_probe(cases, hz: int, seconds: float) -> dict:
    """All displays refreshed from one `hz` timer, while a 1 ms timer logs how late it fires."""
    busy = 0.0
    gaps = []
    last = [time.perf_counter()]

    def probe():
        now = time.perf_counter()
        gaps.append(now - last[0])
        last[0] = now

    def tick():
        nonlocal busy
        t0 = time.perf_counter()
        for case in cases:
            case.refresh()
        busy += time.perf_counter() - t0

    probe_timer, tick_timer = QTimer(), QTimer()
    probe_timer.setInterval(1)
    probe_timer.timeout.connect(probe)
    tick_timer.setInterval(round(1000 / hz))
    tick_timer.timeout.connect(tick)
    clock = QElapsedTimer()
    clock.start()
    probe_timer.start()
    tick_timer.start()
    while clock.elapsed() < seconds * 1000:
        QCoreApplication.processEvents()
        time.sleep(0.0005)
    probe_timer.stop()
    tick_timer.stop()
    gaps = sorted(gaps[5:])
    return {"hz": hz, "ui_busy_pct": busy / seconds * 100, "longest_gap_ms": gaps[-1] * 1e3,
            "p99_gap_ms": gaps[int(len(gaps) * 0.99)] * 1e3}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--seconds", type=float, default=3.0)
    parser.add_argument("--json")
    args = parser.parse_args()
    app = QApplication.instance() or QApplication([])
    cases = _cases()
    report = {"cases": {}, "stall": []}
    print(f"{'display':<18}{'median':>10}{'p95':>9}{'@30 fps':>10}{'@60 fps':>10}{'extra':>9}")
    for case in cases:
        r = _time_case(case, args.seconds)
        report["cases"][case.name] = r
        print(f"{case.name:<18}{r['median_ms']:>8.3f}ms{r['p95_ms']:>7.3f}ms"
              f"{r['cpu_pct_30']:>9.2f}%{r['cpu_pct_60']:>9.2f}%{r['extra_cpu_pct']:>8.2f}%")
    total = sum(c["median_ms"] for c in report["cases"].values())
    print(f"{'all three':<18}{total:>8.3f}ms{'':>9}{total * 3:>9.2f}%{total * 6:>9.2f}%{total * 3:>8.2f}%")
    for hz in (30, 60, 30, 60):  # twice, to show run-to-run noise
        report["stall"].append(_stall_probe(cases, hz, args.seconds))
        s = report["stall"][-1]
        print(f"timer @ {hz} Hz: UI busy {s['ui_busy_pct']:.2f}%, p99 gap {s['p99_gap_ms']:.2f} ms, "
              f"longest gap {s['longest_gap_ms']:.2f} ms")
    if args.json:
        Path(args.json).write_text(json.dumps(report, indent=2))
    del app


if __name__ == "__main__":
    main()
