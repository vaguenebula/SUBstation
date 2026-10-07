# Intelligence

The intelligence module is what SUBstation works out about music and sound, on threads of its own: for now **sound
similarity** (Find Similar Sounds in the browser); later MIDI generation, MIDI humanisation (an XGBoost model through
its C++ library), chord recognition, and an MCP server for agents. It is a layer of its own, as the browser's backend
is: the static library `sub_intelligence` ([intelligence/src](../intelligence/src), namespace `sub::intelligence`),
with no Qt, and knowing nothing of the engine or the browser; its application side is
[app/src/intelligence](../app/src/intelligence) (`SoundSimilarity`, `Session.similarity`). How Find Similar behaves
for the user is in [guide/browser.md](guide/browser.md#find-similar-sounds).

```
 application thread (Qt)                         sub::intelligence (C++, no Qt)
 -----------------------                         ----------------------------------------------
 BrowserController ── findSimilar(path, part) ─► SoundSimilarity::find ─► SoundIndex
   │                                                                       keeper (background priority)
   │  FileIndex::updated ──► libraryChanged ────────────────────────────►    sound-index.bin <-> fingerprints
   │                         source: the browser's snapshot ◄── called ──   takes the library from its source
   │                                                                       analysers ×N (background priority)
   │                                                                         decode (miniaudio) -> SoundAnalyzer
   │                                                                       search thread (normal priority)
   │                                                                         the sound's fingerprint, then every file's
   │  queued call ◄──────────────────────────────────────────────────── wake callback (result / progress)
   │  SoundSimilarity::take() -> found(SimilarSounds)
   ▼
 FileIndex::search(text, "similar", ..., scorer) ─► sub::browser: Sort::Score, by each file's similarity
```

## Sound similarity: the method, and why

### What was weighed

| Approach | Quality | Cost | Verdict |
|---|---|---|---|
| Learned embeddings (CLAP, PANNs, OpenL3, VGGish; what Ableton Live 12's similarity search and Sononym's use, by their own accounts) | best at *what* a sound is (kick vs snare vs vocal), which is what they are trained on | a runtime (ONNX Runtime, ~20 MB) and a model (CLAP: 89–158 M parameters, hundreds of MB), ~50 ms a file on a fast CPU: 300 000 files are 4 hours | not now: too heavy to start in the background of a DAW, and to ship. The design keeps the door open (below). |
| Hand-made descriptors (MFCCs, spectral shape, envelope; Freesound's Essentia/Gaia, research on timbre spaces) | good at *how* a sound sounds, which is what picking the next kick needs | ~10 ms a file, no dependencies | **chosen**, several kinds together |
| MFCCs alone | the classic, but blind to the attack and the envelope | cheapest | not enough on their own (below) |

Research on percussive timbre agrees on what listeners hear first: the **log attack time** and the **spectral
centroid** (Lakatos 2000; McAdams), then the spectral envelope (MFCCs) and how the sound dies away. Sononym, a sample
browser built on similarity, lets its users weigh *spectrum*, *timbre*, *pitch* and *amplitude* separately. The
fingerprint follows both: several aspects, each a group of features, compared each as a whole.

### The fingerprint ([SoundFeatures.h](../intelligence/src/similarity/SoundFeatures.h))

47 numbers in six aspects, from the start of the sound (leading silence skipped, the next 6 s; a 2 ms pre-roll
always the same, so where frames fall doesn't depend on the silence before), its level made relative to its peak:

| Aspect | Features | Why |
|---|---|---|
| Timbre | MFCCs 1–12 of 40 mel bands (20 Hz–16 kHz), the mean over the sound, louder frames counting more (those 50 dB down not at all) | the spectral envelope, independent of level |
| TimbreMotion | MFCCs 1–4 of the attack (first 30 ms), the body (to 250 ms) and the tail | a snare's crack and its ring, a clap's bursts: the best single aspect on real drums |
| Spectrum | centroid and its spread over time, bandwidth, 85% roll-off (octaves), flatness (dB), the share below 120 Hz and above 8 kHz (dB), the attack's centroid | brightness, noisiness, sub-bass (808s), air (hats) |
| Envelope | log attack time (20%→90% of the peak amplitude), effective duration (within 30 dB of the peak), temporal centroid, the level in eight octave-wide windows after the peak (20 ms … 2.6 s) | closed vs open hats, tight vs boomy kicks, one-shots vs pads |
| Pitch | how periodic (YIN on 93 ms windows from the peak, down to 21.5 Hz), and that confidence times the pitch in octaves | tuned 808s and tonal one-shots near their pitch; noise near noise |
| Rhythm | onsets per second after the first (spectral flux that also raises the level 3 dB), the file's length | loops apart from one-shots, a 4-bar loop apart from a 1-bar one |

Frames are about 23 ms (1024 at 44.1/48 kHz), a quarter hop; the amplitude envelope is in 2 ms blocks. A part of a
file (an audio clip's) is analysed from its start for its length. Files above 48 kHz are read at 48 kHz.

### Comparing ([Similarity.h](../intelligence/src/similarity/Similarity.h))

Each feature is measured in standard deviations *of the library searched* (z-scores; never dividing by less than a
small spread of the feature's own), its squared difference clipped at 9 (3 standard deviations: one wild feature, a
pitch an octave off, can't outweigh the rest); an aspect is the mean of its features', and the distance the aspects'
weighted mean, so an aspect counts by its weight however many features it has. Similarity is `exp(-distance / 2)`:
1 for the same sound, about 0.37 for two unrelated ones.

The weights (Timbre 0.5, TimbreMotion 1.5, Spectrum 2.5, Envelope 2, Pitch 0.5, Rhythm 0.5) were tuned on a real
library, rounded rather than taken at the optimum so as not to fit it too closely.

### How well it works

[benchmarks/sound_similarity_bench.cpp](../benchmarks/sound_similarity_bench.cpp) on a 5 091-file sample library
(Splice packs; labels from file and folder names: kick, snare, clap, closed and open hat, tom, cymbal, rim, 808...).
Each of 1 100 labelled one-shots is a query over the whole library (loops, vocals, FX and all); precision@10 is the
share of its 10 nearest that are one-shots of its kind:

| | P@10, all queries | kicks | snares | 808s | closed hats | open hats | claps |
|---|---|---|---|---|---|---|---|
| MFCCs alone (the classic) | 0.352 | 0.52 | 0.37 | 0.39 | 0.21 | 0.11 | 0.31 |
| the first fingerprint, untuned | 0.669 | 0.82 | 0.76 | 0.69 | 0.49 | 0.54 | 0.54 |
| the fingerprint as it is: + attack/body/tail timbre, 23 ms frames, tuned weights | **0.694** | **0.84** | **0.79** | **0.73** | **0.53** | **0.59** | **0.56** |

No aspect alone gets past 0.25 (mean over kinds); together they reach 0.45, so the combination is what works: it
doubles what MFCCs alone find. What is left are mostly neighbours a listener would call close (claps and snares, open
hats and crashes, toms and tonal one-shots), which a score by labels counts as misses.

Speed (the index itself, at background priority, its default 4 analysers on a 24-thread Intel Core Ultra 7):

| | |
|---|---|
| analysing 5 091 files, first time | 11.5 s (about 10 ms a file a thread) |
| next start: reading the saved fingerprints | 10 ms (1.7 MB), all files checked against their stamps in 20 ms |
| a search over 5 084 fingerprints | 0.5 ms (300 000 files: about 30 ms) |
| a search from a clip's part (analysed then) | 1.6 ms |

### What would make it better

A learned embedding added as another aspect: the `Fingerprint` grows by the model's output (or a projection of
it), `kFeatureVersion` goes up so saved fingerprints are made again, and the comparison weighs it like the rest.
The module is where such models go anyway (humanisation's XGBoost). An embedding small enough to run in the
background (an EfficientAT or a distilled OpenL3, a few MB) would mostly help *across* kinds; the descriptors stay
what tells one kick from another.

## Files

### The module (`intelligence/src`)

| File | What it holds |
|---|---|
| [core/AudioReader.h](../intelligence/src/core/AudioReader.h) | `readMono()`: decodes a file (or a part) with miniaudio, mixed down, at most 48 kHz; `MonoAudio`, `AudioError` |
| [core/Fft.h](../intelligence/src/core/Fft.h) | `Fft`: radix-2, complex in place and real through a half-size complex one; one per thread |
| [core/Platform.h](../intelligence/src/core/Platform.h) | `FileStamp`/`stamp()`, `enterBackgroundMode()`, `replaceFile()`, `openFile()`, WTF-8 to UTF-16; [Platform.cpp](../intelligence/src/core/Platform.cpp) (Windows), [PlatformPosix.cpp](../intelligence/src/core/PlatformPosix.cpp) |
| [similarity/SoundFeatures.h](../intelligence/src/similarity/SoundFeatures.h) | `SoundAnalyzer` (one per thread), the fingerprint's layout (`feature::`), `Aspect`, `featureInfo()`, `kFeatureVersion` |
| [similarity/Similarity.h](../intelligence/src/similarity/Similarity.h) | `AspectWeights`, `Comparison` (`fit`, `distance`, `similarity`) |
| [similarity/SoundStore.h](../intelligence/src/similarity/SoundStore.h) | sound-index.bin: `StoreWriter`, `writeStore()`, `readStore()` |
| [similarity/SoundIndex.h](../intelligence/src/similarity/SoundIndex.h) | `SoundIndex` (its threads), `SoundQuery`, `SimilarityResult`, `SoundIndexStatus`, `SoundIndexOptions` |

### The application side (`app/src/intelligence`)

| File | What it holds |
|---|---|
| [SoundSimilarity.h](../app/src/intelligence/SoundSimilarity.h) | `SoundSimilarity` (`Session.similarity`): the index on the application's thread; `setLibrary(FileIndex*)`, `find()`, `found(SimilarSounds)`, `progressChanged`; `SimilarSounds` (a result: `similarity(path)`, `best(n)`, `scorer()`) |

The browser's side of Find Similar is in [BrowserController](../app/src/browser/BrowserController.h)
(`findSimilar()`, `clearSimilar()`, `similarTo`) and the backend's `Sort::Score` ([browser.md](browser.md)).

## The index

### Threads

| Thread | Priority | Does |
|---|---|---|
| keeper | background | reads sound-index.bin at start; takes the library from its source after `libraryChanged()` (at most once a second, `refreshSeconds`); saves 10 s after fingerprints change, and on `close()` |
| analysers (a quarter of the cores, 1–4; `threads`) | background | new files first: stamp, decode, fingerprint; then the saved ones: their stamp (size, last-write time) compared, analysed again if it changed |
| search | normal (the user waits) | the sound's fingerprint (its saved one if a whole library file and up to date; else analysed now, and kept), then every analysed library file compared with it |

Background priority is the browser indexer's: `THREAD_MODE_BACKGROUND_BEGIN` on Windows (CPU, I/O and memory
priority), the lowest nice value and the idle I/O class on Linux. Playback and the UI never wait on it.

Nothing of the module's ever calls into the application but the wake callback, which (as the browser's) is called
once until the next `take()`, and only posts: `SoundSimilarity` takes on its own thread, then emits `found` (a
search's result, only the latest one's) and `progressChanged` (at most four times a second while analysing).

### The library

The application sets a *source*: a function the keeper calls to get the library's paths. `SoundSimilarity` makes it
from the browser's index (`setLibrary(FileIndex*)`): the latest `browser::Snapshot`'s files, folder by folder (which
keeps the analysers on one part of the disk at a time), in the browser's own form (`Snapshot::join`), and not again
while the snapshot's version is the same. So nothing is indexed by hand, and nothing walks the disk twice: what the
browser finds is analysed soon after, what goes is dropped from searches. `setLibrarySource()` waits for a call to the
old source to end: once it returns, the old one is never called again (the browser lets go of it before its index
closes).

### Searches

`find(SoundQuery{path, start, length})` replaces a waiting search. The result (`SimilarityResult`) holds every
analysed library file's similarity, looked up by path (an open-addressing table of the paths' 64-bit FNV-1a hashes:
no path is copied), the 256 best (not the sound itself), how many were scored, and why the sound couldn't be analysed
if it couldn't. A search right after start waits for the saved fingerprints, and for the first library if it is on
its way (at most 3 s).

The statistics the features are measured by (each one's mean and spread) are those of the library's analysed files at
the time of the search, worked out with it (one pass more over the fingerprints).

### The store

`sound-index.bin` in `localDataDir()` (`%LOCALAPPDATA%\SUBstation` on Windows, `~/.local/share/SUBstation`
elsewhere), or `SUBSTATION_SOUND_INDEX` (the tests set it). Written through `.tmp` and moved over the old one. It
keeps the library's files (analysed, or not analysable) and the 1 000 sounds outside it searched from most recently.
The format is in [SoundStore.h](../intelligence/src/similarity/SoundStore.h); a file of another format, feature version
or size, or with a bad checksum, is ignored and everything is analysed again. Bump `kFeatureVersion` whenever what
`SoundAnalyzer` computes changes.

Memory and disk: about 300 bytes a file (the fingerprint, the path, the stamp): 1.7 MB for 5 000 files, about
100 MB for 300 000.

## The application side

`Session` makes the `SoundSimilarity` before the browser and hands it to the `BrowserController`
(`Options::similarity`), which points it at its index and offers *Find Similar Sounds*. On shutdown the browser lets
go of it first, then the session closes it (saving the fingerprints). `Session::Options::analyseSounds` turns the
background analysis off (the tests' sessions list the user's Music folder): searches then only analyse the sound
searched from.

A `SimilarSounds` keeps its result alive; `scorer()` is what the browser's search sorts by, `best(n)` what the sampler
and the drum rack will step through to swap in similar sounds, and `similarity(path)` takes Qt's paths.

## Invariants

- The module includes nothing of Qt, the application, the UI, the engine or the browser (`ctest -R boundaries`); it
  decodes through the `miniaudio` library target, not through the engine.
- Its threads never call into the application but the wake callback, which only posts; the library source is called
  on the keeper's thread and must only read immutable data.
- Only the latest search's result is handed out, once.
- A fingerprint depends only on the sound: not on its level, the silence before it, or the library (the library only
  sets the scale searches measure in).

## Extending it

- **A new feature**: add it to `feature::` and `featureInfo()` (its name, aspect and minimum spread), compute it in
  `SoundAnalyzer::analyze()`, bump `kFeatureVersion`, and run `sound_similarity_bench --tune` on a labelled library
  to see that it helps.
- **Weights the user picks** (as Sononym's aspects): `SoundIndexOptions::weights` (`AspectWeights`); a search could
  take its own.
- **Swapping similar samples** (the sampler, the drum rack, a file manager): `SoundSimilarity::find()` with the
  sample's file, then step through `SimilarSounds::best()`.
- **Another part of the module** (MIDI generation, chord recognition...): a folder of its own under
  `intelligence/src` beside `similarity/`, sharing `core/`; its Qt side under `app/src/intelligence`.

## Tests

- [tests/intelligence](../tests/intelligence) (`intelligence_tests`, Qt-free, the engine tests' harness): the FFT
  against a direct DFT; reading (mono mix, parts, 96 kHz to 48 kHz, broken files); the fingerprint (level and leading
  silence don't change it, silence has none, pitch of tones and noise, envelopes, spectra, onsets of loops and
  one-shots); synthetic kicks, snares, hats and claps finding their own kind; comparing (clipping, weights); the store
  (round trip, damage, another version); the index (analysing a library, saving and checking stamps, new and changed
  files, files leaving the library, sounds outside it and parts of files, undecodable files, a library file searched
  from before it was analysed, only the latest result, its threads and its source).
- [tests/app/test_sound_similarity.cpp](../tests/app/test_sound_similarity.cpp): the browser's files analysed; Find
  Similar's list, sort, status, filtering by text and places; ending it (another sort, another list, clearing, Ctrl+F);
  a clip's part of a loop; a sound outside the library and `SimilarSounds`; files found later; a sound that can't be
  analysed; the result menu.
- [tests/app/test_browser_native.cpp](../tests/app/test_browser_native.cpp): the backend's `Sort::Score` against the
  reference.
- The UI: the similar bar ([test_ui_browser.cpp](../tests/app/test_ui_browser.cpp)), an audio clip's *Find Similar
  Sounds* ([test_ui_arrangement.cpp](../tests/app/test_ui_arrangement.cpp)), the browser showing for it
  ([test_ui_mainwindow.cpp](../tests/app/test_ui_mainwindow.cpp)).

## Sources

- Lakatos, "A common perceptual space for harmonic and percussive timbres" (2000); McAdams et al. on timbre spaces;
  Peeters, "A large set of audio features for sound description" (CUIDADO, 2004): log attack time, spectral centroid
  and the temporal descriptors.
- de Cheveigné and Kawahara, "YIN, a fundamental frequency estimator for speech and music" (2002).
- Mehrabi, Dixon, Sandler, "Vocal imitation of percussion sounds: similarity measures for vocal-based drum sample
  retrieval using deep convolutional auto-encoders" (ICASSP 2018): learned features predict drum similarity better
  than MFCCs or temporal descriptors alone.
- Ableton Live 12's *Similarity Search* and *Similar Sample Swapping*; Sononym's aspects (overall, spectrum, timbre,
  pitch, amplitude); Freesound's similarity (Essentia descriptors, PCA, nearest neighbours).
