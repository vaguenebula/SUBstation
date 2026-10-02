# Browser benchmarks

Not part of the test suite. They make a synthetic sample library (`library_gen.py`:
packs, categories and sub-folders of 20-150 files, names like real sample names,
mostly WAV with some FLAC/MP3, Ableton `.asd` files and artwork beside them; the
files are empty, the browser only reads names) once per size, in
`%TEMP%\sub-browser-bench`, and measure on it. Nothing touches your settings, use
counts, index or plug-ins.

```powershell
python -m benchmarks.browser_backend_bench --size 200000 [--audio] [--json out.json]
python -m benchmarks.browser_ui_bench --size 200000 [--audio] [--json out.json] [--code DIR]
```

- `browser_backend_bench` compares the backend alone: the Python index and search
  from before (kept as `tests/browser_reference.py`) against the native one. It also
  checks that every query returns the same items in the same order, and fails if not.
- `browser_ui_bench` measures the real `BrowserPanel`: time until results are laid
  out and painted, and the longest time the UI thread couldn't run (a 1 ms timer's
  gaps). It only uses what the panel had before and after the native backend, so
  `--code DIR` runs it against another copy of the `substation` package, for example
  the one before (`git archive 63776d3 src/substation`, with the built
  `_engine*.pyd` copied into it; the engine didn't change).
- `--audio` plays an arrangement (8 tracks, half of them time-stretched) through the
  default output the whole time, silently (master gain 0). It reports the engine's
  CPU load, and how far its playhead fell behind the wall clock at worst: a late
  audio callback is a dropout and leaves the playhead behind for good.

## Results

200 000 audio files (plus ~60 000 other files) in 2 364 folders; Windows 11, Python
3.12, Focusrite USB (WASAPI, 10 ms buffer). Warm file-system cache. Before is commit
63776d3; after is this branch. Raw reports are in `results/`.

**What the user waits for** (`browser_ui_bench --audio`; the runs without audio are
the same within noise):

| | before | after |
|---|---|---|
| First results with a new index | 1678 ms (after the whole walk) | 50 ms (while scanning; done at 668 ms) |
| First results on the next start | 1781 ms (walks again) | 135 ms (the saved index; checked at 262 ms) |
| Search results, median / slowest of 30 | 286 / 749 ms | 11.6 / 23.7 ms |
| After the last key when typing | 249 ms | 9.9 ms |
| Search results during a full rescan | up to 1245 ms | up to 23 ms |
| Longest UI stall: searching | 602 ms | 10 ms |
| Longest UI stall: typing | 94 ms | 4.4 ms |
| Longest UI stall: during a rescan | 1245 ms | 3.2 ms |
| Longest UI stall: start / next start | 407 / 330 ms | 19 / 4.5 ms |
| Engine CPU load, max | 5.7 % | 5.9 % |
| Playhead's worst lag (10 ms buffer) | 10.9 ms | 10.2 ms |

Times include the 150 ms the old search waited for typing to pause; the new one
doesn't wait. Neither version made the audio drop out: the playhead never fell more
than a buffer behind.

**The backend alone** (`browser_backend_bench`; Python's time is time the UI thread
was blocked, the native search runs on its own thread):

| Samples, 200 000 files | results | Python | native search | native, asked to taken |
|---|---|---|---|---|
| (everything), Rank | 200 000 | 22 ms (240 ms the first time) | 2.5 ms | 2.7 ms |
| "kick", Rank | 13 268 | 97 ms | 7.5 ms | 8.0 ms |
| "e", Rank | 132 674 | 298 ms (440 ms the first time) | 17.7 ms | 18.2 ms |
| "808 bass", Rank | 425 | 72 ms | 7.0 ms | 7.4 ms |
| (everything), Name | 200 000 | 18 ms | 1.2 ms | 1.6 ms |
| "kick", Name | 13 268 | 69 ms | 6.8 ms | 6.9 ms |
| a place, any query | ≤ 447 | 23-27 ms | 0.3-0.4 ms | 0.5-1.0 ms |

"The first time" is the first Rank search after indexing, which made every item's
key. Searches during a full rescan: 1.6-20 ms. Building the index: Python 877 ms,
native 548 ms (first files after 34 ms); starting with the saved index: 238 ms until
checked, first files after 114 ms (reading it 68 ms, the first snapshot 50 ms). The
"asked to taken" column polls every 0.2 ms, so it includes up to that much waiting.

**Variance.** Reading a saved index that was written a moment before sometimes
took 0.5-1 s instead of ~70 ms, once per newly written file, in both benchmarks
(`after-ui-200k.json`: 852 ms to first results on the restart). Reading the same
file again, sooner or later, is fast; it looks like the file being scanned by
antivirus after it was written. It only delays the first results; the UI stays
responsive (4 ms longest stall in that run). A real restart reads a file written
long before.

## How the time was spent before

`results/before-profile-200k.txt` is the profile of the Python browser that led to
the design (same library, warm cache):

- Walking: 810 ms, of which the file system calls were ~200 ms; the rest was Python
  per file (working out the folder's name again for every file: 460 ms). It ran on a
  thread, but held the GIL, so the UI stalled for up to 100 ms meanwhile.
- Every search ran on the UI thread: ~73 ms to filter 200 000 names, up to 230 ms to
  rank the matches, and 480 ms more for the first ranked search after indexing (the
  item keys).
- Showing the results: Qt's list view lays out every row it has, about 1 µs a row,
  so 200 ms for 200 000 rows on each change, whatever language made the list.
- Nothing was kept: every start and every change of places walked everything again.

So the native backend keeps a saved index and updates it by folder, searches on its
own thread with no Python in between, and the list is filled a page (256 rows) at a
time as it scrolls.

# Parallel track processing

```powershell
python -m benchmarks.parallel_render_bench [--tracks 32] [--device synth|ott|plugin] [--live 64,256] [--json out.json]
```

Every track plays eight-voice chords on the built-in Synth (`--device ott`: through
an OTT; `--device plugin --plugin PATH --name NAME`: a noise clip and the notes
through a VST3 plug-in). It renders offline (1024-frame chunks, best of three) on
1, 2, 4 and the default number of threads, and fails if any render differs from
the one on one thread. `--live` plays the same through the fake ASIO driver
(tests/asio_driver) in manual mode, buffers back to back on one thread: the time
per buffer against its length is the audio thread's load.

## Results

Intel Core Ultra 7 270K Plus (24 logical cores, so 23 threads by default), Windows 11,
Python 3.12. Raw reports are in `results/parallel-*.json`.

32 Synth tracks, 10 s:

| threads | offline | speed-up | live, 64 frames (mean / p99) | live, 256 frames (mean / p99) |
|---|---|---|---|---|
| 1 | 0.657 s (15× realtime) | 1.00× | 94 / 192 µs | 359 / 613 µs |
| 2 | 0.335 s | 1.96× | 55 / 99 µs | 195 / 332 µs |
| 4 | 0.176 s | 3.74× | 33 / 64 µs | 108 / 229 µs |
| 23 | 0.072 s (139× realtime) | 9.11× | 22 / 101 µs | 55 / 158 µs |

8 Synth → OTT tracks, 10 s: 1.96× on 2 threads, 3.69× on 4, 5.19× on 23 (there are
only 8 tracks to share). Live at 64 frames: 46 µs per buffer on one thread, 15 µs on 4.

A project with two tracks renders at 32 frames as it did on one thread (below the
threshold, chunks render serially) and a little faster at 128 (13 → 9 µs).

### Starting the heaviest tracks first

`--heavy N` puts a chain of OTTs on the last N tracks (the last in routing order,
so without cost ordering they start last); `--compare-ordering` renders each
thread count both ways. 16 Synth tracks, 2 of them with 8 OTTs, 10 s
(`results/parallel-16-heavy-ordering.json`):

| threads | heaviest first | routing order | live, 128 frames (mean): heaviest first / routing order |
|---|---|---|---|
| 1 | 0.665 s | (same) | 182 µs |
| 2 | 0.334 s | 0.336 s | 99 / 98 µs |
| 4 | 0.200 s | 0.267 s | 59 / 79 µs |
| 8 | 0.193 s | 0.244 s | 66 / 78 µs |

With 4 threads or more the render is down to about as long as the heaviest track
takes alone. With 2 threads the order hardly matters: every track starts within
the first few slots anyway. With 32 equal tracks both orders are the same within
noise, and timing every track costs nothing measurable on one thread.
