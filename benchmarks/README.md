# Benchmarks

Programs to run by hand, not part of the test suite (ctest doesn't run them).
Nothing they do touches your settings, use counts, browser index or plug-ins.

| | |
|---|---|
| [parallel_render_bench.cpp](parallel_render_bench.cpp) | Tracks rendered on one thread and on several: offline (best of three, checked bit-identical on any number of threads) and live through the fake ASIO driver. The engine alone, no Qt. |
| [browser_backend_bench.cpp](browser_backend_bench.cpp) | The browser's backend on a large synthetic library: indexing, starting from the saved index, searches. Checks every query against the reference the tests use, and fails if any differs. |
| [builtin_devices_bench.cpp](builtin_devices_bench.cpp) | Each built-in audio effect alone, as the audio thread runs it: what it costs a core, at its defaults or at settings given. The engine alone, no Qt. |
| [sound_similarity_bench.cpp](sound_similarity_bench.cpp) | Sound similarity's fingerprints (Essentia's descriptors) on a real sample library: how fast they are made and searched, how fast the index analyses a library, and how often a sound's nearest sounds are of its kind; `--split` holds half the queries out, `--tune` searches the aspects' weights. The intelligence module alone, no Qt. |
| [LibraryGen.h](LibraryGen.h) | Makes the synthetic sample libraries. |
| [PyRandom.h](PyRandom.h), [Json.h](Json.h) | Python's random numbers (so a library is the one the Python generator made), and the reports as JSON. |

Two more benchmarks measured the Python UI (`browser_ui_bench.py`: the
`BrowserPanel`; `display_fps_bench.py`: the device editors' displays). They went
with that UI; their recorded results stay below and in `results/`, and the
scripts are in the history (before the Qt Quick UI: `git show
bd845a9:benchmarks/browser_ui_bench.py`). The Qt Quick UI's own drawing is
timed by its tests (`test_ui_sg`: the scene-graph painter with tens of
thousands of shapes).

## Building and running

The benchmarks are built with `-DSUBSTATION_BUILD_BENCHMARKS=ON` (off by
default), into the build's `bin` folder with everything else. Always in Release.
`browser_backend_bench` needs the application layer (Qt Core, as the tests do);
`parallel_render_bench` and `builtin_devices_bench` need only the engine, and `sound_similarity_bench` only the
intelligence module (no Qt either; it wants a sample library of your own).

Linux:

```sh
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release -DSUBSTATION_BUILD_BENCHMARKS=ON
ninja -C build parallel_render_bench browser_backend_bench builtin_devices_bench sound_similarity_bench
build/bin/parallel_render_bench --tracks 32 --threads 1,2,4,8 --json parallel.json
build/bin/builtin_devices_bench --seconds 5 --json devices.json
build/bin/browser_backend_bench --size 200000 --json backend.json
build/bin/sound_similarity_bench --folder ~/Samples --cache /tmp/fingerprints.bin --json similarity.json
```

Windows, in a *Developer PowerShell for VS 2022* (MSVC and Ninja on the path), with
Qt's MSVC build and, for `--live`, the ASIO SDK (see [docs/building.md](../docs/building.md)):

```powershell
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release -DSUBSTATION_BUILD_BENCHMARKS=ON `
      -DCMAKE_PREFIX_PATH=C:\Qt\6.8.0\msvc2022_64 -DSUBSTATION_ASIO_SDK=C:\src\asiosdk_2.3.3_2019-06-14
ninja -C build parallel_render_bench browser_backend_bench sound_similarity_bench
build\bin\parallel_render_bench.exe --tracks 32 --live 64,256 --json parallel.json
$env:PATH = "C:\Qt\6.8.0\msvc2022_64\bin;$env:PATH"   # browser_backend_bench runs with Qt's DLLs
build\bin\browser_backend_bench.exe --size 200000 --audio --json backend.json
build\bin\sound_similarity_bench.exe --folder D:\Samples --cache $env:TEMP\fingerprints.bin --json similarity.json
```

`--help` lists each one's options. The results below were measured with the
Python-era builds (Python 3.12 driving the engine and the browser backend through
the nanobind bindings, `python -m benchmarks.<name>`), before the benchmarks were
ported to C++; the C++ programs measure the same things the same way, and print
and write reports in the same shape.

# Browser benchmarks

```
browser_backend_bench [--size 200000] [--used 500] [--audio] [--json out.json]
```

It makes a synthetic sample library ([LibraryGen.cpp](LibraryGen.cpp): packs,
categories and sub-folders of 20-150 files, names like real sample names, mostly
WAV with some FLAC/MP3, Ableton `.asd` files and artwork beside them; the files are
empty, the browser only reads names) once per size, in
`%TEMP%\sub-browser-bench\lib-<size>-1` (`$TMPDIR` or `/tmp` elsewhere), and
measures on it. The names come from Python's random numbers, so the library is the
same, file for file, as the Python generator made it, and one made by either is
reused by the other. `--used` items (picked as the Python benchmark picked them)
get a use count, so ranked searches have something to rank.

It compares the backend alone with the reference: the Python index and search from
before (`tests/browser_reference.py`), ported to C++ for the tests
([tests/app/support/BrowserReference.h](../tests/app/support/BrowserReference.h)).
Every query's results must be the same items in the same order; it reports which
differ and fails (exit code 1) if any does.

- Index: building it (the reference's walk; the native scan with a new index
  file, and when its first files showed), and starting again with the saved one.
- Query: the reference's search on the calling thread; the native search on its
  own thread, and from asking until the results can be taken on the caller's
  thread (polled every 0.2 ms); and making the first page (256 rows) of them. For
  the whole library and for a pack's folder (a place), ranked and by name, for
  `""`, `kick`, `e`, `808 bass`, `kick deep` and `zzqx`.
- Indexing: native searches while a full rescan runs.
- `--audio` plays an arrangement (8 tracks, half of them time-stretched) through the
  default output the whole time, silently (master gain 0). It reports the engine's
  CPU load, and how far its playhead fell behind the wall clock at worst: a late
  audio callback is a dropout and leaves the playhead behind for good.

The reference's times are those of the C++ port now, not of the Python code the
results below compare with, so its column (`reference_ms`; the Python version's
reports had `python_ms` and `python_first_ms`) is no longer the "before". The
native columns and the index's are the same as before.

`browser_ui_bench.py` (removed with the Python UI) measured the real (Python)
`BrowserPanel`: time until results were laid out and painted, and the longest time
the UI thread couldn't run (a 1 ms timer's gaps).

## Results

Measured with the Python-era builds: the Python benchmarks, `browser_ui_bench.py`
and the Python `browser_backend_bench.py`.

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

```
parallel_render_bench [--tracks 32] [--device synth|ott|plugin] [--plugin PATH --name NAME]
    [--heavy N [--heavy-otts 8]] [--compare-ordering] [--seconds 20] [--threads 1,2,4,N]
    [--live 64,256] [--live-seconds 5] [--json out.json]
```

Every track plays eight-voice chords on the built-in Synth (`--device ott`: through
an OTT; `--device plugin --plugin PATH --name NAME`: a noise clip and the notes
through a VST3 plug-in, for example
`--plugin build/testplugins/SUBTestPlugins.vst3 --name "SUB Test Effect"`). It
renders offline (1024-frame chunks, best of three) on 1, 2, 4 and the default number
of threads (or `--threads`), and fails if any render differs from the one on one
thread. `--live` plays the same through the fake ASIO driver (tests/asio_driver) in
manual mode, buffers back to back on one thread: the time per buffer against its
length is the audio thread's load. The driver is built with the tests where the
engine has ASIO (Windows, with the ASIO SDK, and `SUBSTATION_BUILD_TESTS` and
`SUBSTATION_TEST_PLUGINS` on, as by default); elsewhere `--live` says it was
skipped. It is Qt-free, and uses the engine tests' helpers (`tests/engine/harness`:
WAV files, seeded random numbers, the fake driver), so its chords' roots and its
noise are not numpy's: the same amount of work, not the same notes.

On 4 cores, Linux (8 tracks, 5 s, `--threads 1,2,4`):

```
8 tracks (synth), 4 logical cores; offline, 5 s:
 threads    order      time  x realtime  speed-up
       1     cost    0.174s       28.7x     1.00x
       2     cost    0.096s       52.0x     1.81x
       4     cost    0.056s       89.4x     3.12x

Live: skipped (the engine was built without the ASIO SDK)

(tracks rendered on workers: 4209)
```

## Results

Measured with the Python-era build (`python -m benchmarks.parallel_render_bench`).
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

# Sound similarity

```
sound_similarity_bench --folder <sample library> [--threads N] [--limit N] [--cache file]
                       [--weights t,m,s,e,p,r,ts,c,ss,to,x] [--tune] [--split] [--show N]
                       [--index] [--json out.json]
```

Fingerprints every audio file under the folder with the index's extractor
(`EssentiaExtractor`; on `--threads` threads, normal priority; `--cache` keeps
them between runs, made again when the extractor's schema changes), then measures
one search over all of them, measuring the library's statistics, and how well the
nearest sounds match what a sound is. A file's kind comes from its name and its
folder's: kick, snare, clap, closed and open hi-hat, tom, cymbal, rim, shaker,
snap, 808, and drum machines' abbreviations (BD, SD, CP, CH, OH...); TidalCycles'
Dirt-Samples folders (bd, sn, cp, hh, oh, lt/mt/ht, cr, rm, stab, pluck, bass,
jungbass, arpy...) as kick, snare... synth stab, pluck, synth bass, sub bass, arp;
a name with "loop", "fill", "bpm", "breaks"... is a loop. Each labelled one-shot
is a query over the whole library, and precision@k is the share of its k nearest
sounds (itself left out) that are one-shots of its kind; each loop is a query too,
its hits other loops. It prints that per kind and each aspect alone. `--weights`
gives the aspects' weights in `Aspect`'s order (Timbre, TimbreMotion, Spectrum,
Envelope, Pitch, Rhythm, TimbreSpread, Contrast, SpectralShape, Tonality,
Embedding); those left off the end keep the extractor's. `--split` halves the queries by their folder and reports each half;
`--tune` searches the weights for the best mean precision@10 over the kinds of
the first half (so the second shows what the tuning is worth on sounds it didn't
see). `--index` also runs the `SoundIndex` itself over the library at background
priority: from nothing, then from its saved fingerprints. `--show N` prints the N
nearest of one query per kind. How the fingerprint works is in
[docs/intelligence.md](../docs/intelligence.md).

## Results

[Dirt-Samples](https://github.com/tidalcycles/Dirt-Samples) (not in the
repository: `git clone --depth 1 https://github.com/tidalcycles/Dirt-Samples`):
2 068 WAV files in 221 folders, at 44.1, 22.05, 48 and 16 kHz and a few odd rates,
plus 30 drum loops made from its kicks, snares and hats (90–174 BPM, 4 s). A
release build on a 4-core cloud machine (Linux, GCC 13):

| | |
|---|---|
| analysing 2 068 files, 4 threads | 8.6 s (240 files/s); a file: median 10 ms, 95% 58 ms (a thread); one thread 59 files/s |
| the index, 4 analysers at background priority | 8.2 s; the next start reads the 0.8 MB store in 3 ms and has checked every file's stamp 8 ms later; a search 0.7 ms |
| one search over 2 068 fingerprints | 0.2 ms (the comparison only); measuring the library's statistics 2 ms |
| queries | 583 labelled one-shots, 40 loops |

Precision@10, Essentia's descriptors against the module's own (the extractor it
replaced, built from the commit before; the same files, labels and queries):

| | one-shots, mean over kinds | one-shots, all | P@1, mean | loops | kick | snare | synth stab | synth bass |
|---|---|---|---|---|---|---|---|---|
| the module's own descriptors | 0.467 | 0.580 | 0.671 | 0.462 | 0.455 | 0.692 | 0.804 | 0.414 |
| Essentia's (default weights) | **0.492** | **0.602** | **0.683** | **0.528** | **0.504** | **0.692** | **0.870** | **0.504** |

Each aspect alone (mean precision@10 over kinds): timbre 0.34, timbre motion 0.37,
spectrum 0.34, envelope 0.18, pitch 0.13, rhythm 0.11, timbre spread 0.22,
contrast 0.39, spectral shape 0.32, tonality 0.22. With `--split`, the six aspects
the module had: 0.389 / 0.469 (the two halves); with the four added: 0.407 /
0.514. Weights `--tune`d on the first half did worse on the second (0.498), so the
defaults stay round. The full table per kind is in
[docs/intelligence.md](../docs/intelligence.md#how-well-it-works).

The module's own descriptors, on a Splice library of 5 091 files (1 403 packs) on
an Intel Core Ultra 7 270K Plus (24 threads), Windows 11, MinGW GCC 13: precision@10
0.694 over all queries (kick 0.84, snare 0.79, 808 0.73, closed hat 0.53, open hat
0.59, clap 0.56); 1 250 files/s on 12 threads, 1.4–9.5 ms a file. That library's
labels are cleaner than Dirt-Samples' file names: compare extractors on one set,
not the sets with each other.

# Built-in devices

```
builtin_devices_bench [--device ID[,ID...]] [--seconds 10] [--block 256] [--rate 48000]
    [--set PARAM=VALUE ...] [--json out.json]
```

Each built-in audio effect (or those named), made from the registry and prepared
at the rate, plays stereo noise and two tones at about -12 dBFS in blocks of
`--block` frames, as the audio thread gives them; the time its `process()` takes,
against how long the audio plays, is its load on one core (best of three).
`--set` changes a parameter, by id and in its own units, on every device that has
one: the heavy settings below. No engine, no graph: the device alone.

## Results

Linux, Intel Xeon at 2.1 GHz (a cloud VM's 4 cores, other work on it), GCC 13,
Release, 48 kHz, blocks of 256, 5 s. The raw report is in
`results/builtin-devices.json`.

| device | defaults | heavy settings |
|---|---|---|
| Amp | 0.68 % | 1.36 % (Dual Mono, Lead, Gain 10) |
| Chorus-Ensemble | 0.32 % | 0.86 % (Ensemble, Warmth 100, High-pass, Feedback 80) |
| Compressor | 0.13 % | |
| Delay | 0.14 % | |
| Disperser | 0.24 % | |
| EQ | 0.02 % | |
| Erosion | 0.21 % | |
| Gate | 0.14 % | |
| Limiter | 0.31 % | 0.69 % (True Peak, Gain +12 dB) |
| Multiband Dynamics | 0.71 % | |
| Over The Top | 0.44 % | |
| Phaser-Flanger | 0.23 % | 0.96 % (42 notches, Feedback 95, Amount 100) |
| Reverb | 1.35 % | 1.45 % (Size 500, Predelay 250 ms, Decay 60 s, Spin and Chorus 100, Diffusion, Scale and Shape 100) |
| Saturator | 0.09 % | 0.67 % (Hi-Quality, Waveshaper, Color, DC, Drive +12 dB) |
| Sidechain | 0.08 % | |
| Spectral Compressor | 0.56 % | 0.80 % (Stereo Link 0, Upward 10, Smoothing 100, Threshold -60, Ratio 20) |
| Utility | 0.01 % | |

Oversampling (the Saturator's Hi-Quality, the Amp) runs the shared half-band
stages a tap at a time over the block, which the compiler vectorises. Before it
did, the same machine measured about 1.4 % for the Saturator's heaviest
Hi-Quality settings and 2.1-2.2 % for the Amp in Dual Mono.
