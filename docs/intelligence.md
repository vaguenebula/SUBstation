# Intelligence

The intelligence module is what SUBstation works out about music and sound: for now **sound similarity** (Find
Similar Sounds in the browser, on threads of its own; its descriptors made with
[Essentia](../intelligence/third_party/essentia), vendored), **harmony** (a song's chords and key, inferred from its MIDI:
the piano roll's chord lane, its notes out of the key in red, and Generate's block chords and bass lines) and
**humanizing** (the piano roll's Humanize › Velocity: velocities from HUMANBRO's XGBoost model, through its C++ library,
vendored); later MIDI generation by machine learning (melodies, accompaniment), a timing model for Humanize › Timing,
chords from audio, and an MCP server for agents, which will take the harmony as context. It is a layer of its own, as
the browser's backend is: the static library `sub_intelligence` ([intelligence/src](../intelligence/src), namespace
`sub::intelligence`), with no Qt, and knowing nothing of the engine or the browser; its application side is
[app/src/intelligence](../app/src/intelligence) (`SoundSimilarity`, `Session.similarity`; `Harmony`,
`Session.harmony`; `Humanizer`, `Session.humanizer`). How Find Similar behaves for the user is in
[guide/browser.md](guide/browser.md#find-similar-sounds), the chords, the key, Generate and Humanize in
[guide/midi.md](guide/midi.md#chords-and-key).

```
 application thread (Qt)                         sub::intelligence (C++, no Qt)
 -----------------------                         ----------------------------------------------
 BrowserController ── findSimilar(path, part) ─► SoundSimilarity::find ─► SoundIndex
   │                                                                       keeper (background priority)
   │  FileIndex::updated ──► libraryChanged ────────────────────────────►    sound-index.bin <-> fingerprints
   │                         source: the browser's snapshot ◄── called ──   takes the library from its source
   │                                                                       analysers ×N (background priority)
   │                                                                         decode (miniaudio), 44.1 kHz -> EssentiaExtractor
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
| Learned embeddings (CLAP, PANNs, OpenL3, VGGish; what Ableton Live 12's similarity search and Sononym's use, by their own accounts) | best at *what* a sound is (kick vs snare vs vocal), which is what they are trained on | a runtime (ONNX Runtime, ~20 MB) and a model (CLAP: 89–158 M parameters, hundreds of MB), ~50 ms a file on a fast CPU: 300 000 files are 4 hours | not now: too heavy to start in the background of a DAW, and to ship. The extractor is replaceable for when one is small enough ([below](#what-would-make-it-better)). |
| Hand-made descriptors (MFCCs, spectral shape, envelope; Freesound's Essentia/Gaia, research on timbre spaces) | good at *how* a sound sounds, which is what picking the next kick needs | ~10 ms a file | **chosen**, several kinds together, made with **Essentia** |
| MFCCs alone | the classic, but blind to the attack and the envelope | cheapest | not enough on their own (below) |

Research on percussive timbre agrees on what listeners hear first: the **log attack time** and the **spectral
centroid** (Lakatos 2000; McAdams), then the spectral envelope (MFCCs) and how the sound dies away. Sononym, a sample
browser built on similarity, lets its users weigh *spectrum*, *timbre*, *pitch* and *amplitude* separately. The
fingerprint follows both: several aspects, each a group of features, compared each as a whole.

The descriptors are [Essentia](https://essentia.upf.edu)'s (the Music Technology Group's audio analysis library,
Freesound's descriptors), vendored at its latest release, 2.1-beta5: only its core and the 29 algorithms used, with
KISS FFT, nothing else (no FFTW, FFmpeg, libsamplerate, YAML, Gaia or TensorFlow;
[VERSION.txt](../intelligence/third_party/essentia/VERSION.txt)). SUBstation decodes and resamples, and hands it PCM.
It is AGPLv3: [licensing.md](licensing.md). It replaced the module's own descriptors (the same first six aspects,
hand-written; in git's history), on which it adds four aspects of descriptors Essentia has and they hadn't, and which
measurably find more sounds of a query's kind ([How well it works](#how-well-it-works)).

### The fingerprint ([EssentiaExtractor.h](../intelligence/src/similarity/EssentiaExtractor.h))

81 numbers in ten aspects. Every sound is analysed at 44.1 kHz (decoded, or resampled, to it: a 22, 48 or 96 kHz copy
of a sound is measured as the 44.1 kHz one is), from where it starts (leading silence skipped, the next 6 s; a 2 ms
pre-roll always the same, so where frames fall doesn't depend on the silence before), its level made relative to its
peak (MFCC 0, a frame's level, is left out): neither the level nor the rate changes a fingerprint.

| Aspect | Features (Essentia's algorithms) | Why |
|---|---|---|
| Timbre | MFCCs 1–12 (`MFCC`: 40 HTK mel bands, 20 Hz–16 kHz, power in dB, DCT-II), the mean over the sound, louder frames counting more (those 50 dB down not at all) | the spectral envelope, independent of level |
| TimbreMotion | MFCCs 1–4 of the attack (first 30 ms), the body (to 250 ms) and the tail | a snare's crack and its ring, a clap's bursts: an attack's character kept apart, not averaged away |
| TimbreSpread | each MFCC's spread over the sound | a loop's timbre moves from hit to hit, a pad's hardly; the best single gain on loops |
| Spectrum | centroid (`Centroid`) and its spread over time, bandwidth (`CentralMoments`, `DistributionShape`), 85% roll-off (`RollOff`), flatness (`Flatness`, dB), the share below 120 Hz and above 8 kHz (`EnergyBand`, dB), the attack's centroid and flatness | brightness, noisiness, sub-bass (808s), air (hats); a click against a thump |
| Contrast | peaks against valleys in six bands, and the valleys (`SpectralContrast`) | tonal against noisy, band by band: the best single aspect |
| SpectralShape | skewness and kurtosis (`DistributionShape`), `Crest`, `ZeroCrossingRate`, change from frame to frame (`Flux`, from the second frame on), slope (`Decrease`) | the shape beyond its centre and spread |
| Tonality | how many peaks within 60 dB of the loudest, 20 Hz–16 kHz (`SpectralPeaks`: the spectral complexity), how they beat (`Dissonance`, the lowest 100), how strongly the spectrum repeats 200 Hz–5 kHz apart (`PitchSalience`, every fourth frame) | metal (cymbals, cowbells), detuned stabs, harmonic against inharmonic |
| Envelope | log attack time (`LogAttackTime`, 20%→90% of the peak amplitude), effective duration (`EffectiveDuration`, within 30 dB of the peak), temporal centroid (`Centroid` over time), the level in eight octave-wide windows after the peak (20 ms … 2.6 s) | closed vs open hats, tight vs boomy kicks, one-shots vs pads |
| Pitch | how periodic (`PitchYin`, YIN, on 93 ms windows from the peak, down to 21.5 Hz), and that confidence times the pitch in octaves | tuned 808s and tonal one-shots near their pitch; noise near noise |
| Rhythm | onsets per second after the first (spectral flux of the mel bands that also raises the level 3 dB), the file's length | loops apart from one-shots, a 4-bar loop apart from a 1-bar one |

Frames are 23 ms (1024 samples), a quarter hop; the amplitude envelope is in 2 ms blocks. Essentia computes each
frame's descriptors and the envelope's; the module frames, sums up (weighted means; the attack, body and tail apart)
and adds what Essentia has no algorithm for, or one that doesn't suit one-shots: the level after the peak, onsets
after the first, the pitch's summary ([Descriptors.h](../intelligence/src/similarity/Descriptors.h)). So a fingerprint
is the same size for a 5 ms click and a 6 s loop, and a short one-shot's attack isn't lost in an average.

What was learnt making it:

- **`PitchYinFFT` can't hear an 808.** Its loudness weighting leaves nothing of a 40 Hz fundamental (no pitch at all
  at 41 Hz); `PitchYin` on the sound brought down to 11 kHz finds it to 5 cents.
- **`SpectralContrast` wants magnitudes below 1.** It raises each band's peak/valley to 1/ln(the band's mean) and blows
  up (to 10³³) where a mean nears 1, which unnormalised FFT magnitudes do. It is handed the spectrum scaled to a full
  scale of 1.
- **miniaudio's resampler is linear**, which takes a few dB off the top octave: a 48 kHz copy of a sound analysed
  darker than the 44.1 kHz one. Resampling is band-limited (a Kaiser-windowed sinc, `resampleMono`), as a polyphase
  filter made once for a pair of rates: 6.5 s from 48 kHz in 4 ms, from 192 kHz in 10 ms.
- **Some of Essentia's algorithms don't fit as they are.** `SpectralComplexity` counts peaks only between 100 Hz and
  5 kHz (a hat's, all above, count as none): the peaks `SpectralPeaks` finds in the whole band are counted instead.
  `HFC` sums the whole spectrum, so anything above 16 kHz (there in a WAV, gone from its MP3) moved it without bound;
  over the band alone it is the centroid again, and leaving it out lost nothing on the benchmark. `Flux`'s first frame
  is measured against silence, so it is left out: a click's flux would otherwise be its spectrum.
- **Essentia's logger isn't thread-safe** (a global indent counter): it is compiled out (`DEBUGGING_ENABLED=0`).
- Descriptors that didn't help, and so aren't in: GFCCs (ERB-band cepstra), inharmonicity and the odd-to-even harmonic
  ratio, and the envelope's `DerivativeSFX` and `FlatnessSFX` (each lowered the held-out precision).

### Comparing ([Similarity.h](../intelligence/src/similarity/Similarity.h))

Each feature is measured in spreads *of the library searched* (z-scores; never dividing by less than a small spread
of the feature's own, set at 5–10% of its typical spread), its squared difference clipped at 9 (3 spreads: one wild
feature, a pitch an octave off, can't outweigh the rest); an aspect is the mean of its features', and the distance the
aspects' weighted mean, so an aspect counts by its weight however many features it has. Similarity is
`exp(-distance / 2)`: 1 for the same sound, about 0.37 for two unrelated ones. Neither level nor length can dominate:
the level isn't a feature, and the length and the effective duration are one feature each of aspects of several.

The library's statistics (each feature's spread) are measured robustly, its extreme 1% at either end held at the 1st
and 99th percentiles (winsorized), so a few broken or freakish files don't flatten the scale for everything else. The
index keeps them and saves them with the fingerprints, measured again when it saves after a change, or when a search
finds that fingerprints changed for a tenth as many files as they describe, or the library grew or shrank by a tenth
(on at most 20 000 of its fingerprints, without holding up the index's other threads).

The weights: Timbre 0.5, TimbreMotion 1.5, Spectrum 2.5, Envelope 2, Pitch 0.5, Rhythm 0.5 (tuned on a 5 000-file
Splice library for the module's own descriptors, rounded rather than taken at the optimum), and TimbreSpread,
Contrast, SpectralShape, Tonality 1 each: untuned, round, and each measured to help on its own (below). They are the
extractor's (`FeatureSchema::weights`); a search could take others (`SoundIndexOptions::weights`).

### How well it works

[benchmarks/sound_similarity_bench.cpp](../benchmarks/sound_similarity_bench.cpp) on
[Dirt-Samples](https://github.com/tidalcycles/Dirt-Samples) (TidalCycles' sample set, not in the repository: 2 068 WAV
files, 221 folders, at 44.1, 22.05, 48, 16 kHz and odd rates), with 30 drum loops added (made from its kicks, snares and
hats at 90–174 BPM). Labels come from folder and file names (kick, snare, clap, closed and open hat, tom, cymbal, rim,
808, synth stab, pluck, synth bass, sub bass, arp; loops). Each of 583 labelled one-shots is a query over the whole
library; precision@10 is the share of its 10 nearest that are one-shots of its kind (unlabelled files count as misses,
so these are lower bounds). Each of the 40 loops is a query too, its hits other loops. Both extractors on the same
files, labels and queries:

| P@10 | the module's own descriptors (before) | **Essentia's** |
|---|---|---|
| one-shots, mean over kinds | 0.467 | **0.492** |
| one-shots, all queries | 0.580 | **0.602** |
| P@1, mean over kinds | 0.671 | **0.683** |
| loops | 0.462 | **0.528** |
| kicks (113 queries) | 0.455 | **0.504** |
| snares (129) | 0.692 | **0.692** |
| toms (75) | 0.895 | **0.901** |
| cymbals (62) | 0.673 | **0.687** |
| closed hats (56) | **0.352** | 0.348 |
| open hats (20) | 0.370 | **0.390** |
| claps (11) | **0.455** | 0.436 |
| synth stabs (23) | 0.804 | **0.870** |
| plucks (17) | 0.794 | **0.835** |
| synth bass (28) | 0.414 | **0.504** |
| 808s (11) | 0.227 | **0.264** |
| sub bass (20) | **0.150** | 0.140 |

Whether each added aspect helps was decided on half the queries held out: the queries' folders split in two by a hash
(`--split`), each aspect added alone to the six, and kept only if one-shot precision (over kinds and over all
queries) rose on the held-out half and didn't fall on the other. Held out (320 queries): the six aspects 0.469 (mean
over kinds) / 0.588 (all queries) / 0.491 (loops); all ten 0.514 / 0.628 / 0.591. Weights
tuned on one half did *worse* on the other (0.514 → 0.498, while the half tuned on rose from 0.407 to 0.426): the halves hold different kinds, so the round weights
stay. Alone, Contrast (0.390) beats every one of the module's own aspects (the best, TimbreMotion, 0.368). What is left
are mostly neighbours a listener would call close (claps and snares, open hats and crashes, sub basses and long 808
kicks) or files whose names don't say what they are.

The module's own descriptors had 0.694 on the 5 091-file Splice library they were tuned on (kicks 0.84, snares 0.79,
closed hats 0.53): that library's labels are cleaner than Dirt-Samples' file names, so the two sets' numbers don't
compare with each other, only the extractors on one set.

Speed (a release build on a 4-core cloud machine, the index's own analysers at background priority):

| | |
|---|---|
| analysing a file | median 10 ms (95%: 58 ms; a 6 s loop up to 130 ms): the module's own descriptors took 1.4 ms. About half of it is the four aspects added (their gain was judged worth it); most of the rest the spectrum, PitchYin and MFCCs |
| analysing 2 068 files, first time, 4 analysers | 8.2 s (252 files a second): 300 000 files are about 20 minutes, once |
| next start: reading the saved fingerprints | 3 ms (0.8 MB), all files checked against their stamps in 8 ms |
| a search over 2 068 fingerprints | 0.7 ms, asked to taken (300 000 files: about 100 ms) |

### What would make it better

A learned embedding as an aspect of its own: an extractor ([FeatureExtractor.h](../intelligence/src/similarity/FeatureExtractor.h))
whose schema has Essentia's descriptors and an `Embedding` aspect (a small model's output, or a projection of it), its
weight set by the benchmark. Nothing else changes: the index, the store and the comparison follow the schema, and the
fingerprints saved by the previous extractor are made again. An embedding small enough to run in the background (an
EfficientAT or a distilled OpenL3, a few MB) would mostly help *across* kinds; the descriptors stay what tells one kick
from another. Essentia's TensorFlow models would bring their runtime; ONNX Runtime with an exported model is lighter.

## Harmony: chords and keys from MIDI

[harmony/](../intelligence/src/harmony) (namespace `sub::intelligence::harmony`) infers a song's chords and key from
its notes, and writes parts from chords. It is pure functions: no threads, no state, nothing saved. The application
asks again whenever the song has changed and somebody wants to know (see [the application side](#harmony-the-application-side)).

### What was weighed

| Approach | Quality | Cost | Verdict |
|---|---|---|---|
| Learned models (an HMM trained on annotated songs, a transformer over MIDI) | the best at ambiguous passages and at style | a model to train, ship and run; annotated data | not now: the rules below get the clear cases right, and the module is where a model would go |
| Chord templates against pitch-class profiles, smoothed (Pardo and Birmingham; Fujishima's chroma templates; Sheh and Ellis's HMM smoothing, its transitions written by hand) | right wherever chords or a bass are played; a melody alone is a guess | a song's thousands of notes in a few milliseconds | **chosen** |
| Note counting per bar | breaks on passing notes and on chords that change mid-bar | the cheapest | no |

### The chords ([ChordInference.h](../intelligence/src/harmony/ChordInference.h))

The song is cut into steps of an eighth note (longer for a song past 65 536 steps). Each step has two profiles of the
twelve pitch classes: how long each sounds (louder notes counting up to twice as much as the softest), and how long
each is the lowest note, counting fully up to E2, a quarter from G3 up (a melody's lowest note is no bass).

Each of 132 chords (12 roots × major, minor, diminished, augmented, sus2, sus4, 7, maj7, m7, m7b5, dim7; a sixth
chord is its relative minor seventh with another bass: C6 is Am7/C) is scored against each step:

| Term | Value |
|---|---|
| explained | the share of the step's profile on the chord's notes, less half the share off them |
| missing notes | for each chord note below 30 % of the step's loudest pitch class, up to 0.25 for the root or a seventh, 0.15 for the third (a sus chord's second or fourth, a diminished or augmented fifth), 0.05 for a perfect fifth |
| bass | + 0.25 × how much the root is the bass, + 0.08 for its other notes, − 0.1 for notes outside it |
| quality | 0 for major and minor; − 0.02 to − 0.04 for sevenths, − 0.06 to − 0.1 for the rest |
| key | + 0.05 when every note is in the key |
| thin steps | with fewer than three pitch classes sounding (a melody, a bass line), up to + 0.3 for the key's primary triads (I, IV, V; i, iv), less for the others, − 0.2 outside the key, − 0.3 for sus, augmented and diminished sevenths: a melody alone implies plain triads |

A step counts by how much sounds in it (two notes all through it count fully), so a lone melody note weighs less than
a chord. Then a Viterbi search finds the sequence of chords (or "no chord", which scores 0.1 a beat where nothing
sounds) with the best total, where changing chord costs 0.25 on a bar line, 0.45 on half a bar, 0.7 on a beat and 1.0
between beats. So a passing note doesn't make a chord, a chord played clearly changes where it is played (an eighth
before the bar, too), a rest of a beat keeps the chord and one of more than a bar and a half has none. With "stay or
change to the best", the search is linear in the chords: steps × 133.

A chord's bass is the pitch class lowest under it the longest; if that is another of its notes, and is the bass under
at least half of it at full strength, it is an inversion ("C/E").

### The key

Krumhansl and Kessler's major and minor key profiles, correlated with how long each pitch class sounds, counting again
how long it is the bass (by the same register weighting): the bass tells C → G (C major) from the same notes in E
minor. Fewer than 2 beats of notes or three pitch classes, or a best correlation under 0.4, is no key. A minor key's
scale is its natural minor (as in Ableton's scales); the classic confusion of a key with its relative changes no note
in or out of it.

### Parts from chords ([Accompaniment.h](../intelligence/src/harmony/Accompaniment.h))

- **Chords**: a close voicing for each, between C2 and G4, the first in root position near D3, each next one the
  inversion and octave that moves the voices least (each note to the nearest of the other chord's, both ways, plus a
  quarter of how far it strays from D3): C, G, Am, F come out as C3 E3 G3, B2 D3 G3, C3 E3 A3, C3 F3 A3. Velocity 90.
- **Bass**: each chord's bass note (an inversion's, else its root) in the octave from C1. Velocity 100.
- Both are struck again at every bar line.
- **A progression to start from**, where a song has no chords: a chord a bar, I V vi IV in a major key, i VI III VII in
  a minor one.

Rules, not a model: a melody, or an accompaniment with a rhythm of its own, needs machine learning, which comes later.

### How well it works

The tests ([tests/intelligence/test_harmony.cpp](../tests/intelligence/test_harmony.cpp)) hold it to triads and
sevenths in root position, inversions, a progression with its bass on another track, a melody running over chords
(heard as the chords), arpeggios (as the chords they spell), rests short and long, and a melody alone (Twinkle,
Twinkle: C | F G | F C | Dm C). Speed: a 400-bar song of 4 400 notes, chords and a melody, in about 5 ms (a release
build, one core of a 4-core cloud machine); a typical song is under a millisecond.

### What would make it better

Chords from audio: the same scoring over the chroma of audio clips (the similarity's FFTs and frames). Time signature
changes, and tempo changes, if the project gets them. A model trained on annotated MIDI as one more term of the score
(or in place of the hand-set transitions), for the ambiguous passages.

## Humanizing: velocities by machine learning

[humanize/](../intelligence/src/humanize) (namespace `sub::intelligence::humanize`) gives a part's notes the
velocities a pianist would play them with. Velocity and timing are separate models: velocity is this one; timing is
to come (Humanize › Timing nudges starts at random until then: `notes::humanizedTiming`).

### The model

HUMANBRO, the project's own model: an XGBoost regressor (1 986 trees of depth 8) trained on the MAESTRO v3
performances (1 276 recorded piano performances, 7 million notes), which predicts each note's velocity from 127
features of its musical context, every one of them something a score shows: its pitch and register, where it falls in
the bar (downbeat, beat, offbeat), its length, the intervals to the notes before and after it and the melody's contour,
the texture around it (how many notes in the last and next seconds and beats, their range), its place in its chord
(top, bottom, inner voice), the phrase (rests, how far into it), and the piece as a whole. It sees no velocity: the
same notes always get the same prediction, and a part's own velocities don't leak into it. How it was trained,
evaluated and checked for leakage is in HUMANBRO's README; its C++ runtime is vendored in
[intelligence/third_party/humanbro](../intelligence/third_party/humanbro) (its own README and VERSION.txt: what was
copied, and the one local change, loading a model from memory so the file is opened through the module's wide-path
`platform::openFile`). It is built as its own static library, `humanbro`, with its strict floating-point flags
(`/fp:precise`; `-ffp-contract=off -fno-fast-math`): its features match its Python pipeline bit for bit.

The model shipped is [intelligence/models/velocity.hbm](../intelligence/models/velocity.hbm) (15.7 MB, in the
repository), HUMANBRO's *quantized* model: trained on the performances with every note snapped to the beat grid, and
snapping the notes it is given to the grid itself. A DAW's notes are drawn, quantized or played in against a tempo
map, which is what it suits; HUMANBRO's other model, trained on the performances' own timing, does much worse on
quantized notes (it leans on the timing pianists play with). The build copies it to `bin/models/velocity.hbm`, next
to the executables, where the application looks for it.

| On MAESTRO's test split (177 performances, 741 410 notes) | MAE | Pearson r | within a performance r |
|---|---|---|---|
| the quantized model, on quantized notes | 11.3 | 0.62 | 0.59 |
| every note at the training set's mean velocity | 15.3 | – | – |
| each pitch at its mean velocity in the training set | 14.3 | 0.32 | 0.36 |

### A part ([VelocityModel.h](../intelligence/src/humanize/VelocityModel.h))

A note's features look at the notes before and after it, so a part is humanized as a whole: `humanize(part, meter,
amount)` takes every note of it, those to humanize marked `target`, the others context, and returns the part's
velocities with only the targets' changed.

- **Where it is**: notes are in quarter-note beats on the song's timeline, bar lines every bar from beat 0; the part
  is handed to the model from the bar its first note is in (at 960 ticks a quarter note, with the song's tempo and
  time signature), as a score would start: the features count beats and bars from the start, so a part whole bars
  later in the song gets the same velocities.
- **Its level**: how loud a part is overall a score doesn't say (MAESTRO's recordings differ by session and piano), so
  the model's velocities are levelled to the targets' own mean velocity: the targets keep their loudness and take the
  model's shape. Then each target moves `amount` (0..1) of the way to its levelled velocity, rounded, held to 1..127.
- **What it hears**: on a melody over chords it plays the melody about 25 louder than the accompaniment, a chord's
  top note a little louder than the rest, a phrase's first note softer. Its dynamics are compressed, as a
  squared-error model's are (HUMANBRO measured its unquantized model's spread at a standard deviation of 13 where the
  pianists' is 19); amounts are how far toward it, never past.

Speed (a release build on an Intel Core Ultra 7 270K Plus): loading the model, 18 ms and 16 MB; 200 notes, 29 ms;
500, 71 ms (on one core: the runtime spreads rows over cores from 512 notes on); 2 500, 81 ms; 10 000, 108 ms.

### Humanizing: the application side

`Humanizer` ([Humanizer.h](../app/src/intelligence/Humanizer.h), `Session.humanizer`) lives on the application's
thread. It loads the model the first time velocities are asked for (from `velocityModelPath()`,
`<the application's folder>/models/velocity.hbm`) and keeps it; if it can't (missing, damaged, locked), it says so
on `statusMessage`, gives no velocities, and tries again the next time. `velocityAvailable` (the file is there) is what greys out the menu entry.

`velocities(targets, amount)` takes notes of clips (each with its track and clip) and judges each track as one part:
every note its clips are heard playing (`Clip::heardNotes()`: none of a deactivated clip, nor deactivated notes, each
where it plays on the timeline) is context, and the targets are put where their clips play them (deactivated or
hidden by a trim, they are still humanized). Drum tracks (`Harmony::isDrumTrack()`) are left as they are, and the
status line says so: the model knows pianos. Each track's targets keep their own mean, so humanizing a quiet pad and a
loud lead together leaves each as loud as it was. Nothing runs in the background: a click on Humanize › Velocity
waits for it, a tenth of a second for the longest parts.

## Files

### The module (`intelligence/src`)

| File | What it holds |
|---|---|
| [core/AudioReader.h](../intelligence/src/core/AudioReader.h) | `readMono()`: decodes a file (or a part) with miniaudio, mixed down, at most 48 kHz; `readMonoAt()`, at a given rate; `resampleMono()` (band-limited, Kaiser-windowed sinc, polyphase; cancellable); `MonoAudio`, `AudioError` |
| [core/Hash.h](../intelligence/src/core/Hash.h) | `fnv1a()`: the stores' checksums, schema keys, result paths |
| [core/Platform.h](../intelligence/src/core/Platform.h) | `FileStamp`/`stamp()`, `enterBackgroundMode()`, `replaceFile()`, `openFile()`, WTF-8 to UTF-16; [Platform.cpp](../intelligence/src/core/Platform.cpp) (Windows), [PlatformPosix.cpp](../intelligence/src/core/PlatformPosix.cpp) |
| [similarity/FeatureSchema.h](../intelligence/src/similarity/FeatureSchema.h) | `Aspect`, `AspectWeights`, `FeatureInfo`, `FeatureSchema` (an extractor's features, weights, and what saved fingerprints must match: `key()`) |
| [similarity/FeatureExtractor.h](../intelligence/src/similarity/FeatureExtractor.h) | `FeatureExtractor` (`extract()` from PCM, `extractFile()`; cancellable), `SoundBuffer`, `Extraction`, `ExtractorFactory` (the schema, and how to make an extractor), `defaultExtractorFactory()` |
| [similarity/EssentiaExtractor.h](../intelligence/src/similarity/EssentiaExtractor.h) | `EssentiaExtractor` (one per thread), the fingerprint's layout (`feature::`), `featureInfo()`, `essentiaSchema()`, `kFeatureVersion`, `kAnalysisRate` |
| [similarity/Descriptors.h](../intelligence/src/similarity/Descriptors.h) | (internal) preparing a sound, its 2 ms envelope, the level after the peak, onsets, the pitch's summary |
| [similarity/Similarity.h](../intelligence/src/similarity/Similarity.h) | `FeatureStatistics` (`measure()`: winsorized spreads), `Comparison` (`fit`, `distance`, `similarity`) |
| [similarity/SoundStore.h](../intelligence/src/similarity/SoundStore.h) | sound-index.bin: `StoreWriter`, `writeStore()`, `readStore()`, `StoreContents` |
| [similarity/SoundIndex.h](../intelligence/src/similarity/SoundIndex.h) | `SoundIndex` (its threads; `find()`, `cancelSearch()`, `schema()`), `SoundQuery`, `SimilarityResult`, `SoundIndexStatus`, `SoundIndexOptions` |
| [third_party/essentia](../intelligence/third_party/essentia) | Essentia 2.1-beta5's core and 29 algorithms (the `essentia` library), [VERSION.txt](../intelligence/third_party/essentia/VERSION.txt), [vendor.sh](../intelligence/third_party/essentia/vendor.sh), [local-changes.patch](../intelligence/third_party/essentia/local-changes.patch) |
| [harmony/Chords.h](../intelligence/src/harmony/Chords.h) | `Quality`, `Chord` (its notes, its name: "Am7", "C/E"), `Key` (its scale, its triads), `pitchClass()`, `noteName()` |
| [harmony/ChordInference.h](../intelligence/src/harmony/ChordInference.h) | `Note`, `ChordSpan`, `InferenceOptions`, `Harmony`; `estimateKey()`, `inferHarmony()` |
| [harmony/Accompaniment.h](../intelligence/src/harmony/Accompaniment.h) | `GeneratedNote`; `chordPart()`, `bassPart()`, `starterProgression()` |
| [humanize/VelocityModel.h](../intelligence/src/humanize/VelocityModel.h) | `VelocityModel` (`humanize()`, `predict()`), `Note` (a part's, `target` or context), `Meter`, `ModelError` |
| [third_party/humanbro](../intelligence/third_party/humanbro) | HUMANBRO's C++ runtime (the `humanbro` library): `humanbro::Humanizer`, its features and tree walker |
| [models/velocity.hbm](../intelligence/models) | The velocity model (HUMANBRO's quantized one); [models/README.md](../intelligence/models/README.md) says where it came from and how to replace it |

### The application side (`app/src/intelligence`)

| File | What it holds |
|---|---|
| [SoundSimilarity.h](../app/src/intelligence/SoundSimilarity.h) | `SoundSimilarity` (`Session.similarity`): the index on the application's thread; `setLibrary(FileIndex*)`, `find()`, `cancel()`, `found(SimilarSounds)`, `progressChanged`; `SimilarSounds` (a result: `similarity(path)`, `best(n)`, `scorer()`) |
| [Harmony.h](../app/src/intelligence/Harmony.h) | `Harmony` (`Session.harmony`): the song's notes (`songNotes()`), its chords and key inferred from them when asked after a change, the key (the project's, else inferred), `shown` (the setting C toggles) |
| [Humanizer.h](../app/src/intelligence/Humanizer.h) | `Humanizer` (`Session.humanizer`): the velocity model loaded on first use (`velocityModelPath()`, `velocityAvailable`), `velocities(targets, amount)` with each track's notes as context, `statusMessage` |

The browser's side of Find Similar is in [BrowserController](../app/src/browser/BrowserController.h)
(`findSimilar()`, `clearSimilar()`, `similarTo`; leaving the similar list cancels its search) and the backend's
`Sort::Score` ([browser.md](browser.md)).

## The index

### Threads

| Thread | Priority | Does |
|---|---|---|
| keeper | background | reads sound-index.bin at start; takes the library from its source after `libraryChanged()` (at most once a second, `refreshSeconds`); saves 10 s after fingerprints change, and on `close()` |
| analysers (a quarter of the cores, 1–4; `threads`), each with its own extractor, made on its thread | background | new files first: stamp, decode (at 44.1 kHz), fingerprint; then the saved ones: their stamp (size, last-write time) compared, analysed again if it changed. Files are checked again whenever the library is taken again, if their last check is more than a minute old (`recheckSeconds`): a sample exported again over itself is analysed again |
| search | normal (the user waits) | the sound's fingerprint (its saved one if a whole library file and up to date; else analysed now, its extractor made the first time, and kept), then every analysed library file compared with it |

Closing stops the analysers in the middle of a file (extractors look at a flag between frames, and while resampling):
the file stays to analyse next time, neither analysed nor failed. Searches are stopped the same way when a newer one replaces them or
`cancelSearch()` drops them (`SoundSimilarity::cancel()`, when the browser leaves its similar list).

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

Files are told apart by `platform::pathKey()`, as the browser's keys are: on Windows the path with backslashes in
Windows' own lower case, so a clip's `c:/drums/KICK.wav` is the library's `C:\Drums\Kick.wav` (its saved fingerprint is
used, and it isn't among its own best matches); elsewhere the path as it is. A file keeps the library's spelling,
which is what results are looked up by. The keys are made on the keeper's thread before it takes the lock, so a large
library's refresh holds up searches and analysers only for the lookups.

### Searches

`find(SoundQuery{path, start, length})` replaces a waiting search and stops a running one. The result (`SimilarityResult`) holds every
analysed library file's similarity, looked up by path (an open-addressing table of the paths' 64-bit FNV-1a hashes:
no path is copied), the 256 best (not the sound itself), how many were scored, and why the sound couldn't be analysed
if it couldn't. A search right after start waits for the saved fingerprints, and for the first library if it is on
its way (at most 3 s).

The statistics the features are measured by (each one's spread, winsorized) are the library's, as last measured, or
as read with the fingerprints at start. A save measures them again if fingerprints changed since (so they are at most
10 s old); a search, once fingerprints changed (were analysed, or analysed again) for more than a tenth as many files
as they describe, or the library grew or shrank by more than a tenth: so the first run's early searches don't keep the
scale of a few files, and searches measure in the same scale from one run to the next. Until a library is taken (a search right after start), the saved ones stay.
They are measured on at most 20 000 of the library's fingerprints, picked evenly and copied under the lock, and
measured without it (2 ms for 2 000 files, about 25 ms at the cap): the analysers, the application's `take()` and
other searches aren't held up.

### The store

`sound-index.bin` in `localDataDir()` (`%LOCALAPPDATA%\SUBstation` on Windows, `~/.local/share/SUBstation`
elsewhere), or `SUBSTATION_SOUND_INDEX` (the tests set it). Written through `.tmp` and moved over the old one. It
keeps the library's files (analysed, or not analysable); the files that left it in the last 90 days (`keepDays`: each
file records when it was last in the library), so a place on a drive that is unplugged for a while, or removed and
added again, isn't analysed again when it comes back; and the 1 000 sounds outside it searched from most recently.
Files gone for longer are dropped at the next save.
The format is in [SoundStore.h](../intelligence/src/similarity/SoundStore.h). Its header has the key of what made the
fingerprints (`FeatureSchema::key()`, an FNV-1a hash of the extractor, its version, its settings, Essentia's version
among them: the analysis rate, frame and hop, bands..., and its features) and their size. A store of another extractor, version or settings, of another
format, or with a bad checksum, is ignored and everything is analysed again: bump `kFeatureVersion` whenever what
`EssentiaExtractor` computes changes. A file is told by its path and its stamp (size, last-write time): a changed stamp
and it is analysed again; files of the library not in the store are analysed, and only they (incremental indexing). The
library's statistics are saved after the files.

Memory and disk: about 400 bytes a file (81 features, the path, the stamp): 0.8 MB for 2 000 files, 2 MB for 5 000,
about 120 MB for 300 000.

## The application side

`Session` makes the `SoundSimilarity` before the browser and hands it to the `BrowserController`
(`Options::similarity`), which points it at its index and offers *Find Similar Sounds*. On shutdown the browser lets
go of it first, then the session closes it (saving the fingerprints). `Session::Options::analyseSounds` turns the
background analysis off (the tests' sessions list the user's Music folder): searches then only analyse the sound
searched from.

A `SimilarSounds` keeps its result alive; `scorer()` is what the browser's search sorts by, `best(n)` what the sampler
and the drum rack will step through to swap in similar sounds, and `similarity(path)` takes Qt's paths.

### Harmony: the application side

`Harmony` ([Harmony.h](../app/src/intelligence/Harmony.h)) lives on the application's thread. Its notes are those the
song's MIDI tracks play (`songNotes()`): of every MIDI track heard (not muted, nor in a muted group) whose name doesn't
say it plays drums ("Drums", "Kick", "Snare", "Hats", "Perc", "Claps"...: `isDrumTrack()`), what its clips play, where
they play it on the timeline (nothing of a deactivated clip, nor deactivated notes: `Clip::heardNotes()`). Audio tracks are for later. The bars are the project's time signature's; the key its
key, if it has one.

A change to what it hears (the clips of a track it hears; which tracks those are, as tracks come, go, are muted,
renamed or grouped; the time signature, the key; a new project) marks the result stale and, 40 ms later, emits
`changed` once for every change in that time (a drag's edits). A fader, the loop, the tempo or an audio track change
nothing: what they change is compared first (`heardTracks()`, the time signature and the key). The
chords and key are inferred again only when asked for after that, on the application's thread: nothing runs in the
background, and with no piano roll showing nobody asks. The piano roll asks when it hears `changed` (and when it opens
a clip or shows), maps the chords over the part its clip plays into the clip's own beats, and draws them; Generate asks
too. `shown` (View › Chords and Key, C) is a setting (`pianoroll/show_harmony`), on at first.

## Invariants

- The module includes nothing of Qt, the application, the UI, the engine or the browser (`ctest -R boundaries`); it
  decodes through the `miniaudio` library target, not through the engine.
- Its threads never call into the application but the wake callback, which only posts; the library source is called
  on the keeper's thread and must only read immutable data.
- Only the latest search's result is handed out, once.
- A fingerprint depends only on the sound: not on its level, its sample rate, the silence before it, or the library
  (the library only sets the scale searches measure in).
- Saved fingerprints are used only by the extractor that made them, as it was (its name, version, settings and
  features); nothing else is ever compared.
- Essentia runs only on the index's threads (one set of its algorithms per thread; its factories filled once,
  `essentia::init()` under `std::call_once`), never on the audio thread; its logger is compiled out.
- Harmony is pure functions of the notes and options: the same song always gives the same chords, key and parts.
  It keeps nothing and starts no thread; the application infers it on its own thread, only when asked after a change.
- Humanized velocities are a pure function of the part's notes, the meter and the amount: the model never sees a
  velocity (only the targets' mean, which levels it), and the same part always comes out the same. Only targets
  change. The vendored runtime (`intelligence/third_party/humanbro`, outside the boundary check's folders) includes
  nothing of Qt either.

## Extending it

- **A new feature**: add it to `feature::` and `featureInfo()` (its name, aspect and minimum spread: 5–10% of its
  spread over a library), compute it in `EssentiaExtractor` (an Essentia algorithm not vendored yet: add it to
  `vendor.sh`'s list and run it), bump `kFeatureVersion`, and run `sound_similarity_bench --split` on a labelled
  library to see that it helps on both halves.
- **Another extractor** (a learned embedding): a `FeatureExtractor` with a schema of its own, handed to the index as
  `SoundIndexOptions::extractor` (an `ExtractorFactory`: the schema, and a function making one extractor, called on
  each thread that needs one) and made the default in `defaultExtractorFactory()`. Its aspects' weights in its schema;
  `Aspect::Embedding` is there for it. Saved fingerprints of the previous one are made again on their own. It must
  poll the cancel flag on long work, and be safe to run several of at once. Making one may fail (a model missing):
  the index then analyses nothing and says why (`SoundIndexStatus::error`), and searches that need to analyse their
  sound give the reason as their error.
- **Weights the user picks** (as Sononym's aspects): `SoundIndexOptions::weights` (`AspectWeights`); a search could
  take its own.
- **Swapping similar samples** (the sampler, the drum rack, a file manager): `SoundSimilarity::find()` with the
  sample's file, then step through `SimilarSounds::best()`.
- **Another part of the module** (MIDI generation, an MCP server...): a folder of its own under `intelligence/src`
  beside `similarity/` and `harmony/`, sharing `core/`; its Qt side under `app/src/intelligence`. What it needs to know
  of a song's harmony: `Session.harmony` (`chords()`, `key()`), or `inferHarmony()` on notes of its own.
- **A chord quality**: its intervals and suffix in `Quality` and `qualityInfo()` (Chords.cpp), its prior in `kPrior`
  (ChordInference.cpp), and how thin steps take it in `thinPrior()`.
- **Another part to generate**: a function from `ChordSpan`s to `GeneratedNote`s in Accompaniment.h, a
  `Q_INVOKABLE` on `PianoRoll` like `generateBass()`, and an entry in the clip view's Generate menu.
- **The timing model**: a `TimingModel` beside `VelocityModel` in humanize/ (a part of notes in, each target's start
  offset out), its file as `models/timing.hbm` (copied by intelligence/CMakeLists.txt as velocity.hbm is), a
  `timings()` on `Humanizer` beside `velocities()`, and `PianoRoll::humanizeTiming()` asking it in place of
  `notes::humanizedTiming` (the menu and its amount are there already).
- **Another velocity model** (retrained, or HUMANBRO's unquantized one for played-in parts): export it with HUMANBRO's
  `export_cpp_model.py`, replace `models/velocity.hbm` (the build copies it again), and run `intelligence_tests`
  ("humaniz", "velocity", "melody"...) and `test_humanizer`. A model of another target mode (HUMANBRO's residual
  ones) is refused on loading; a newer runtime goes into third_party/humanbro with its local change kept.

## Tests

- [tests/intelligence](../tests/intelligence) (`intelligence_tests`, Qt-free, the engine tests' harness): reading
  (mono mix, parts, 96 kHz to 48 kHz, at a given rate, broken files) and resampling (pitch, length, no delay, flat to
  the top, a tone from common and odd rates, cancelling); Essentia's spectrum (a sine's centroid and roll-off at its frequency at five rates); the fingerprint (level,
  leading silence and the sample rate don't change it; silence has none; clicks of one to three samples, 8–192 kHz,
  NaN and infinity, truncated and junk files handled; pitch of tones down to 41 Hz and of an 808, noise has none;
  envelopes, spectra, peaks, contrast, shape and change of tones, noise, drums and a loop, a click's flux, high
  partials' complexity; a part of a file as long as what is left of it; onsets of loops and one-shots); kicks, snares, hats, claps, synth stabs, plucks and drum loops finding their own kind; level and length
  not outweighing timbre; cancelling; comparing (clipping, weights); the library's statistics (robust to wild files);
  the schema and its key; the store (round trip, statistics, damage, another extractor, version or setting, a
  fingerprint of the wrong size refused); the index
  (analysing a library, saving and checking stamps, new and changed files, files leaving the library, sounds outside
  it and parts of files, undecodable files, a library file searched from before it was analysed, only the latest
  result, its threads and its source, the statistics saved and searched with, kept until a library is taken and
  measured again as it changes, an extractor that can't be made, another extractor's fingerprints made again, closing
  in the middle of a file, a search replaced or cancelled).
- [tests/intelligence/test_harmony.cpp](../tests/intelligence/test_harmony.cpp) (`intelligence_tests`): naming chords
  and keys, scales and their triads; the key (cadences in major and minor, the bass telling C → G from E minor, too
  little to tell); chords: triads and sevenths, inversions, a progression with its bass on another track, a melody's
  passing notes, arpeggios, a melody alone, a chord pushed off the beat, rests, the key given, a long song's speed;
  the chord part's voicings and voice leading, striking again at bar lines, the bass, the starter progression.
- [tests/app/test_harmony.cpp](../tests/app/test_harmony.cpp): the song's notes (MIDI tracks heard, not drums, what
  clips play where), following the project once edits settle, the key (the project's or inferred), the bars of the
  time signature, the `shown` setting.
- [tests/intelligence/test_humanize.cpp](../tests/intelligence/test_humanize.cpp) (`intelligence_tests`, with the
  model in the repository): loading it, and refusing a missing, junk or truncated file; predictions independent of
  the notes' velocities and of whole bars before the part, but not of where in the bar notes fall or of the time
  signature; the targets' level kept and the context untouched; the amount (none, half); a melody over chords louder
  than the inner voices; a note alone, a chord alone, notes of no length, no notes; a long part's speed.
- [tests/app/test_humanizer.cpp](../tests/app/test_humanizer.cpp): the model next to the tests (`bin/models`); a
  clip's velocities where it plays on the timeline, at the notes' own level; a track's other clips as context, its
  deactivated notes and clips not; each track keeping its own level; targets whose clip is gone.
- [tests/app/test_ui_pianoroll.cpp](../tests/app/test_ui_pianoroll.cpp): the chord lane over the clip (hidden with
  the key, clicks going through it), notes out of the key in red, Generate › Chords and › Bass; Humanize's menu
  (Velocity and Timing, each with its own amount, the notes getting the keyboard back);
  [test_ui_mainwindow.cpp](../tests/app/test_ui_mainwindow.cpp): C.
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
- Krumhansl and Kessler, "Tracing the dynamic changes in perceived tonal organization in a spatial representation of
  musical keys" (1982): the key profiles; Krumhansl, *Cognitive Foundations of Musical Pitch* (1990).
- Pardo and Birmingham, "Algorithms for chordal analysis" (Computer Music Journal, 2002): chord templates scored
  against segments of notes, and segmenting by the best total.
- Fujishima, "Realtime chord recognition of musical sound" (ICMC 1999): pitch-class profiles against chord templates.
- Sheh and Ellis, "Chord segmentation and recognition using EM-trained hidden Markov models" (ISMIR 2003): smoothing
  chord sequences with a Viterbi search.
