"""Parallel track processing: tracks rendered on one thread and on several.

    python -m benchmarks.parallel_render_bench [--tracks 32] [--device synth|ott|plugin]
        [--plugin PATH --name NAME] [--heavy N [--heavy-otts 8]] [--compare-ordering]
        [--seconds 20] [--threads 1,2,4,8] [--live 64,256] [--json out.json]

Every track plays dense chords on the built-in Synth (with --device ott, through
an OTT after it; with --device plugin, a noise clip and the notes through the
named VST3 plug-in). With --heavy, the last N tracks (the last ones in routing
order, so the last to start without cost ordering) go on through a chain of
OTTs. --compare-ordering runs each thread count twice: the tracks with the most
work first (the engine's default), and in routing order. Offline: the arrangement rendered as an export renders it
(1024-frame chunks), best of three. Live (--live; needs the engine built with
the ASIO SDK): the fake ASIO driver (tests/asio_driver) in manual mode, its
buffers processed back to back on this thread at the given sizes; the time a
buffer takes, against how long it plays, is the audio thread's load (over 100%
would be a dropout). Renders are checked to be the same on any number of threads.
"""

from __future__ import annotations

import argparse
import ctypes
import json
import os
import statistics
import sys
import tempfile
import time
import wave
from pathlib import Path

import numpy as np

from gilstudio import _engine as ge

RATE = 48000
SPB = RATE // 2  # samples per beat at 120 BPM
TEST_ASIO = Path(ge.__file__).parent / "_testdrivers" / "GILTestAsio.dll"
TEST_ASIO_NAME = "GIL Test ASIO"


def noise_wav(seconds: float) -> str:
    path = Path(tempfile.mkdtemp(prefix="gil-parallel-bench-")) / "noise.wav"
    rng = np.random.default_rng(0)
    pcm = np.round(rng.uniform(-0.25, 0.25, (int(seconds * RATE), 2)) * 32767).astype("<i2")
    with wave.open(str(path), "wb") as f:
        f.setnchannels(2)
        f.setsampwidth(2)
        f.setframerate(RATE)
        f.writeframes(pcm.tobytes())
    return str(path)


def build(engine: ge.Engine, args, beats: float) -> None:
    rng = np.random.default_rng(1)
    noise = None
    plugin_uid = None
    if args.device == "plugin":
        if not args.plugin or not args.name:
            sys.exit("--device plugin needs --plugin PATH and --name NAME")
        plugin_uid = next((d.uid for d in ge.scan_vst3(args.plugin) if d.name == args.name), None)
        if plugin_uid is None:
            sys.exit(f"No plug-in named {args.name!r} in {args.plugin}")
        noise = noise_wav(beats / 2 + 1)
        engine.load_source(noise)
    for t in range(args.tracks):
        track = engine.add_track()
        chain = engine.track_chain(track)
        synth = engine.add_builtin_processor(chain, "synth")
        engine.set_processor_param(synth, 0, float(t % 4))  # the waveforms in turn
        notes = []
        for bar in range(int(beats // 4)):
            root = int(rng.integers(36, 60))
            for interval in (0, 4, 7, 11, 14, 17, 19, 24):  # eight voices at a time
                notes.append(ge.NoteDesc(bar * 4.0, 4.0, root + interval, 100))
        engine.set_track_notes(track, notes)
        if args.device == "ott":
            engine.add_builtin_processor(chain, "ott")
        elif args.device == "plugin":
            engine.set_track_clips(track, [ge.ClipDesc(noise, 0.0, beats / 2)])
            engine.add_plugin_processor(chain, "VST3", args.plugin, plugin_uid)
        if t >= args.tracks - args.heavy:  # heavy tracks go on through their OTTs, after their device
            for _ in range(args.heavy_otts):
                engine.add_builtin_processor(chain, "ott")
        engine.set_track_gain(track, 1.0 / args.tracks)
    engine.idle()


def runs(threads: list[int], orderings: list[str]) -> list[tuple[int, str]]:
    """(threads, ordering) to measure: one thread renders in order whatever the ordering."""
    return [(count, ordering) for count in threads for ordering in (orderings if count > 1 else orderings[:1])]


def bench_offline(engine: ge.Engine, threads: list[int], orderings: list[str], frames: int) -> list[dict]:
    results, reference = [], None
    for count, ordering in runs(threads, orderings):
        engine.audio_threads = count
        engine.cost_ordering = ordering == "cost"
        times = []
        for _ in range(3):
            start = time.perf_counter()
            out = engine.render_offline(0.0, frames)
            times.append(time.perf_counter() - start)
        if reference is None:
            reference = out
        elif not np.array_equal(out, reference):
            sys.exit(f"The render on {count} threads ({ordering} order) differs from the one on {threads[0]}")
        best = min(times)
        results.append({"threads": count, "ordering": ordering, "seconds": best, "realtime": frames / RATE / best})
    engine.cost_ordering = True
    return results


class Driver:
    """The fake ASIO driver's hooks (see tests/test_asio.py)."""

    def __init__(self):
        dll = ctypes.CDLL(str(TEST_ASIO))
        for name, restype, argtypes in [("Reset", None, []), ("SetManual", None, [ctypes.c_int]),
                                        ("Process", ctypes.c_int, [ctypes.c_int]), ("ClearOutput", None, [])]:
            function = getattr(dll, "GilTestAsio_" + name)
            function.restype, function.argtypes = restype, argtypes
            setattr(self, name, function)
        self.Reset()
        self.SetManual(1)


def bench_live(engine: ge.Engine, threads: list[int], orderings: list[str], buffer: int,
               seconds: float) -> list[dict]:
    driver = Driver()
    engine.open_device(TEST_ASIO_NAME, RATE, buffer, driver="ASIO")
    results = []
    try:
        count = int(seconds * RATE / buffer)
        for threads_, ordering in runs(threads, orderings):
            engine.audio_threads = threads_
            engine.cost_ordering = ordering == "cost"
            engine.position_beats = 0.0
            engine.play()
            driver.Process(8)  # (the workers wake up)
            per_buffer = []
            for i in range(count):
                start = time.perf_counter()
                driver.Process(1)
                per_buffer.append(time.perf_counter() - start)
                if i % 256 == 0:
                    driver.ClearOutput()
            engine.stop()
            driver.Process(2)
            budget = buffer / RATE
            per_buffer.sort()
            results.append({
                "buffer": buffer, "threads": threads_, "ordering": ordering,
                "mean_us": statistics.fmean(per_buffer) * 1e6,
                "p99_us": per_buffer[int(len(per_buffer) * 0.99)] * 1e6,
                "max_us": per_buffer[-1] * 1e6,
                "load": statistics.fmean(per_buffer) / budget,
            })
    finally:
        engine.cost_ordering = True
        engine.close_device()
    return results


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tracks", type=int, default=32)
    parser.add_argument("--device", choices=["synth", "ott", "plugin"], default="synth")
    parser.add_argument("--plugin", help="a .vst3 file or bundle (--device plugin)")
    parser.add_argument("--name", help="the plug-in's name in it (--device plugin)")
    parser.add_argument("--heavy", type=int, default=0, help="this many tracks (the last) are heavy")
    parser.add_argument("--heavy-otts", type=int, default=8, help="the OTTs on a heavy track")
    parser.add_argument("--compare-ordering", action="store_true",
                        help="also render with the tracks started in routing order")
    parser.add_argument("--seconds", type=float, default=20.0, help="offline: the length rendered")
    default_threads = sorted({1, 2, 4, ge.Engine.default_audio_threads()})
    parser.add_argument("--threads", default=",".join(map(str, default_threads)))
    parser.add_argument("--live", default="", help="buffer sizes to play live through the fake ASIO driver")
    parser.add_argument("--live-seconds", type=float, default=5.0)
    parser.add_argument("--json", help="write the results here too")
    args = parser.parse_args()
    threads = [int(t) for t in args.threads.split(",") if t]
    buffers = [int(b) for b in args.live.split(",") if b]
    orderings = ["cost", "routing"] if args.compare_ordering else ["cost"]

    engine = ge.Engine()
    beats = args.seconds * RATE / SPB
    build(engine, args, max(beats, 8.0))
    report = {"tracks": args.tracks, "device": args.device, "heavy": args.heavy, "heavy_otts": args.heavy_otts,
              "cores": os.cpu_count(), "offline": [], "live": []}

    heavy = f", {args.heavy} of them with {args.heavy_otts} OTTs" if args.heavy else ""
    print(f"{args.tracks} tracks ({args.device}{heavy}), {os.cpu_count()} logical cores; offline, {args.seconds:g} s:")
    print(f"{'threads':>8} {'order':>8} {'time':>9} {'x realtime':>11} {'speed-up':>9}")
    report["offline"] = bench_offline(engine, threads, orderings, int(args.seconds * RATE))
    serial = report["offline"][0]["seconds"]
    for r in report["offline"]:
        print(f"{r['threads']:>8} {r['ordering']:>8} {r['seconds']:>8.3f}s {r['realtime']:>10.1f}x "
              f"{serial / r['seconds']:>8.2f}x")

    if buffers:
        if "ASIO" not in ge.driver_types() or not TEST_ASIO.exists():
            print("\nLive: skipped (the engine was built without the ASIO SDK)")
        else:
            os.environ["GILSTUDIO_ASIO_DRIVERS"] = (
                f"{TEST_ASIO_NAME}|{{5B2E8C1A-7F3D-4E6B-9C0A-1D2F3E4A5B6C}}|{TEST_ASIO}")
            print(f"\nLive, {args.live_seconds:g} s per run (time per buffer; load = time / buffer length):")
            print(f"{'buffer':>7} {'threads':>8} {'order':>8} {'mean':>9} {'p99':>9} {'max':>9} {'load':>7}")
            for buffer in buffers:
                for r in bench_live(engine, threads, orderings, buffer, args.live_seconds):
                    report["live"].append(r)
                    print(f"{r['buffer']:>7} {r['threads']:>8} {r['ordering']:>8} {r['mean_us']:>7.0f}us "
                          f"{r['p99_us']:>7.0f}us {r['max_us']:>7.0f}us {r['load']:>6.0%}")
    print(f"\n(tracks rendered on workers: {engine.nodes_on_workers})")
    if args.json:
        Path(args.json).write_text(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
