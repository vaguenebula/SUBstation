# Warping and audio sources

How audio files become clips the engine can play: decoding into memory with waveform peaks
([AudioSource.h](../../engine/src/AudioSource.h), [AudioSource.cpp](../../engine/src/AudioSource.cpp)), and playing a
clip at another speed or pitch: time stretching with stretch voices, and Re-Pitch resampling
([Warp.h](../../engine/src/Warp.h), [Warp.cpp](../../engine/src/Warp.cpp)). The stretcher is
[Signalsmith Stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch), vendored in
`engine/third_party/`.

What the user sees (the clip view, warp modes, transpose, tempo and key from file names) is in
[guide/audio-clips.md](../guide/audio-clips.md). How clips are mixed into a track is in
[rendering.md](rendering.md).

## Overview

- An `AudioSource` is a whole file decoded to 32-bit float at the engine's sample rate, planar, with six levels of
  min/max peaks for drawing waveforms. It is immutable once loaded, so the UI, snapshots and the audio thread share
  it freely. Long files are not streamed from disk: sources are decoded into memory.
- The snapshot turns each clip's beats into samples at the current tempo. A warped clip's two ends sit on their
  beats, and it plays at `tempo / segment BPM` speed.
- Clips that need it are time-stretched on the audio thread by Signalsmith Stretch (MIT, header-only; vendored with
  its FFT library). Nothing else is needed to build it.
- Stretchers keep state from block to block, so they live in *voices* (`WarpVoice`). These are allocated on the edit
  side, pooled per warp-mode block size, and handed to the audio thread in the snapshot. The renderer binds a voice
  to a playing clip.
- A voice re-seeks when playback jumps (start, locate, loop, clip start). It uses the stretcher's `outputSeek` to
  pre-compute its latency, so the first frame is exactly aligned. A tempo change keeps the source position, so the
  clip carries on without a re-seek.
- Offline renders and exports use fresh voices with a fixed random seed, so they are repeatable and don't disturb
  live playback.
- Clips at their own tempo with no transposition skip the stretcher and play bit-exact. Re-Pitch uses windowed-sinc
  resampling, with the cutoff lowered when speeding up.

## Files

| File | What it holds |
|---|---|
| [AudioSource.h](../../engine/src/AudioSource.h) / [.cpp](../../engine/src/AudioSource.cpp) | `AudioFileInfo`, `AudioSource::probe()`, `AudioSource::load()`, planar data, peak mipmaps |
| [Warp.h](../../engine/src/Warp.h) / [.cpp](../../engine/src/Warp.cpp) | `WarpMode`, `StretchConfig`, `stretchConfigFor()`, `stretchTiming()`, `configureStretcher()`, `kStretchSeed`, `WarpVoice`, `WarpVoiceSet`, `renderResampled()` |
| [Snapshot.h](../../engine/src/Snapshot.h) | `ClipRender` (`Playback::Direct / Resample / Stretch`, `rate`, `sourceAt()`, `key`), `TrackBuffers::kMaxClipVoices` |
| [EngineSnapshot.cpp](../../engine/src/EngineSnapshot.cpp) | clips to `ClipRender`s, choosing the playback; `ensureWarpVoicesLocked()` |
| [Engine.cpp](../../engine/src/Engine.cpp) | the source cache: `loadSource()`, `cachedSource()`, `releaseUnusedSources()`, `reloadSourcesLocked()`, `sourceKey()` |
| [EngineOffline.cpp](../../engine/src/EngineOffline.cpp) | fresh voices for offline renders (`prepareOfflineLocked`) |
| [Renderer.cpp](../../engine/src/Renderer.cpp) | `assignVoices()`, `acquireVoice()`, `renderClips()` |
| [third_party/signalsmith-stretch](../../engine/third_party/signalsmith-stretch) | Signalsmith Stretch 1.3.2 (MIT), one header, unmodified |
| [third_party/signalsmith-linear](../../engine/third_party/signalsmith-linear) | Signalsmith Linear 0.6.4 (MIT): only `stft.h` and `fft.h`, what Stretch needs without platform FFT backends |
| [third_party/miniaudio](../../engine/third_party/miniaudio) | the decoders `AudioSource` uses (compiled once in [miniaudio_impl.c](../../engine/src/miniaudio_impl.c)) |

Both Signalsmith libraries are added as `SYSTEM` include directories in [CMakeLists.txt](../../CMakeLists.txt), and
`Warp.cpp` includes the header with MSVC warnings off. See [building.md](../building.md).

## Audio sources

### Decoding

`AudioSource::load(utf8Path, sampleRate)` decodes with miniaudio's decoder (WAV, FLAC and MP3):

1. It opens the file once to learn its native channel count and rate.
2. It opens it again asking for float output at the engine's rate, with miniaudio's linear resampler at its highest
   low-pass filter order (`MA_MAX_FILTER_ORDER`). Mono files stay mono; anything with two or more channels is decoded
   as stereo (more than two are downmixed by miniaudio).
3. It reads 16384 frames at a time, de-interleaving into one vector per channel, then copies them into one planar
   buffer: channel `c` occupies `[c * frames, (c + 1) * frames)`.
4. It builds the peaks.

It throws `std::runtime_error` if the file can't be opened or contains no audio. Paths are UTF-8 and opened with
`initDecoderFile()` ([MiniaudioFiles.h](../../engine/src/MiniaudioFiles.h)): miniaudio's wide call on Windows, so any
Windows path works, and its narrow one elsewhere, so a path never goes through the C library's locale.

`AudioSource::probe(utf8Path)` reads only what is needed for length and format (`AudioFileInfo`: frames at the file's
own rate, channels, rate, duration). For streams that report no length it counts the frames by decoding.

Accessors: `path()`, `channels()`, `frames()`, `sampleRate()` (the rate it was decoded at), `fileSampleRate()`,
`duration()`, `data()`, `channelData(c)`.

### Peak mipmaps

Six levels (`kNumPeakLevels`). Level *n* holds one (min, max) pair per channel for every `32 * 4^n` frames
(`samplesPerPeak(level)`: 32, 128, 512, 2048, 8192, 32768). Level 0 is computed from the samples; each coarser level
combines four peaks of the one below. The layout of `peaks(level)` is `[numPeaks][channels][2]`.

The application layer hands them to the UI without copying: a `Waveform`
([app/src/audio/Waveform.h](../../app/src/audio/Waveform.h), from `EngineBridge::waveform(path)`) holds a
`shared_ptr` to the source, which keeps it alive, and gives `peaks(level)` and `channelData(c)` as they are. The
arrangement's waveforms and the Sampler's editor draw from these ([ui/arrangement.md](../ui/arrangement.md)); the UI
never includes the engine's headers.

### The source cache

The engine keeps one source per file, keyed by `Engine::sourceKey()` (the path normalised, made preferred and
lower-cased, so two spellings of a path share it).

- `Engine::loadSource(path)` returns the cached source if it is at the engine rate. Otherwise it decodes *without the
  engine lock* (the engine bridge calls it from its decoding threads), then stores it and rebuilds the
  snapshot, so clips waiting for this file become audible. If the device's rate changed meanwhile it decodes again
  (up to four tries, then `std::runtime_error`).
- `cachedSource(path)` returns what is cached, at whatever rate.
- `releaseUnusedSources()` drops sources no clip uses (except the one previewing).
- When the device's sample rate changes, `reloadSourcesLocked()` decodes every cached source again at the new rate;
  files that vanished or became unreadable are dropped, and their clips go silent.
- A snapshot only includes a clip whose source is cached *at the engine rate*; until then the clip is skipped
  ("still loading").

Built-in devices that play files (the Sampler) get theirs through the same cache via a `SourceLoader`
(see [devices.md](devices.md)). The engine bridge decodes sources in a thread pool of its own
([app/engine-bridge.md](../app/engine-bridge.md)).

The Sampler warps its sample too (its Warp: the whole sample in so many beats at the tempo), with the same modes: it
resamples for Re-Pitch, and otherwise runs a Signalsmith stretcher per note, configured as the clips' are
(`configureStretcher()`: the block sizes of `stretchTiming()`, split computation) with the same seed
(`kStretchSeed`). Its stretchers are its own, made on the main side while
it stretches ([devices.md](devices.md#sampler-builtinsampler-instrument)).

## Warp modes and how a clip plays

```cpp
enum class WarpMode : uint8_t { Transients, Standard, Smooth, Formants, RePitch };
```

The order matches the application's list (`kWarpModes` in [app/src/model/Clip.h](../../app/src/model/Clip.h)). Projects
saved with the earlier Ableton-style names load into the equivalent mode (a model concern; see
[app/serialization.md](../app/serialization.md)).

| Mode | Playback | Stretch config | Block / interval |
|---|---|---|---|
| Transients | Stretch | `Transient` | 80 ms / 20 ms: tight attacks (drums) |
| Standard | Stretch | `Standard` | 120 ms / 30 ms: Signalsmith's default preset |
| Smooth | Stretch | `Smooth` | 200 ms / 50 ms: pads, textures, noise; softer attacks |
| Formants | Stretch | `Standard` | as Standard, keeping formants in place when transposing |
| Re-Pitch | Resample | (none) | windowed-sinc; speed and pitch change together |

When the snapshot is built (`rebuildSnapshotLocked`, see [rendering.md](rendering.md)), each `ClipDesc` becomes a
`ClipRender`:

- `start` = `llround(startBeat * samplesPerBeat)`; `sourceOffset` = `offsetSec` in samples.
- **Warped** (`warp` and `segmentBpm > 0`): `rate = tempo / segmentBpm`; the end sits on its beat:
  `endBeat = startBeat + durationSec * segmentBpm / 60`, and `length` runs from start to `endBeat` in samples. Both
  ends follow the tempo.
- **Unwarped**: `rate` 1, and the length is `durationSec` in samples, so its length in beats follows the tempo.
- The length is cut so the clip never plays past the end of the file.
- Playback:
  - Re-Pitch: `Resample` if the speed changes, else `Direct`.
  - Otherwise `Stretch` if the speed changes or `transpose` is not 0 (Transpose and Detune shift the pitch without
    changing the speed, warped or not); else `Direct`, bit-exact. `transpose` is in semitones (fractions for
    detune); `preserveFormants` is set for Formants. Re-Pitch ignores transpose.
- `key` identifies the clip across snapshots: a hash of the clip's `id` (or its index if it has none) mixed with
  the track id. Edits that rebuild the snapshot don't interrupt a stretching clip because the key is stable.
- `fadeIn` / `fadeOut`: its fades in timeline samples (its own, in source seconds, at its speed; or the short one
  against clicks where an edge cuts into the file), and their curves: see [rendering.md](rendering.md).

`ClipRender::sourceAt(t)` = `sourceOffset + (t - start) * rate`: the fractional source frame heard at timeline
sample `t`.

## Stretch voices

### Allocation (edit side)

`WarpVoice(config, sampleRate)` configures a `SignalsmithStretch<float>` for two channels with its config's block
and interval, with *split computation* on: each spectral block's work is spread over the following interval instead
of being done in one callback. That adds an interval of latency, which the alignment below compensates. Constructing a
voice allocates; `render()` does not.

`ensureWarpVoicesLocked(needed)` grows the pools (`Engine::warpVoices_`, a `WarpVoiceSet`: one vector per
`StretchConfig`) up to what the snapshot needs, at most 64 per configuration. Per track it counts at most 3 stretched
clips per configuration, since clips on a track don't overlap and only a few play in any one block. Pools only grow;
the snapshot holds the pools (`snap->warpVoices`).

### Binding voices (audio thread)

In the chunk's prologue `Renderer::assignVoices()` walks the clips a track plays in this chunk's segments, in the
order `renderClips()` will play them, and gives each stretched clip a slot (at most `kMaxClipVoices` = 32 per track
per chunk). `acquireVoice()` picks the voice that played this clip last (same `key`) if there is one; otherwise the
voice idle the longest; never one already used in this block. `continuing` is true when the voice played this clip in
the previous block. If there are more simultaneous stretched clips than voices, the extra clip stays silent. A
monitored track plays its input instead of its clips, so it gets no voices.

### `WarpVoice::render()`

```
 source frames:   ... [inputPos - lead] ........ [inputPos] -> fed next
                         ^ the frame being output now
 lead = inputLatency() + rate * outputLatency()
```

1. If the transpose or formant setting changed (or the voice is new), it sets `setTransposeSemitones()` and
   `setFormantFactor(1, preserveFormants)`.
2. It works out where the output is (`inputPos - lead`) and where it should be (`sourceAt(position)`). If the voice
   is not continuing this clip, or isn't primed, or the two differ by more than `2 + 2 * rate` frames, it re-seeks:
   `outputSeek()` with `outputSeekLength(rate)` frames from `floor(wanted)`, which pre-computes the pre-roll so the
   very next output frame is that source frame. Starts, locates and loop jumps are sample-accurate. A tempo change
   rescales timeline positions but keeps the source position within that tolerance, so it carries on.
3. It feeds exactly enough input to reach the source position the block should end on:
   `llround(sourceAt(position + frames) + lead) - inputPos`. The target is absolute, so rounding never accumulates.
4. It writes (does not add) the output into the caller's buffers. A mono source feeds both channels. Outside the
   file the input is silence.

The renderer then applies the clip's gain, fades and pan per sample (`renderClips`).

### Offline renders

`Engine::prepareOfflineLocked()` makes fresh voices, as many as live playback has, for the offline renderer
(`setWarpVoices`). With the fixed seed (`kStretchSeed`, the stretcher randomises some phases), offline renders and
exports come out the same every time, and live voices keep their state.

## Re-Pitch resampling

`renderResampled(clip, position, frames, outL, outR)` is stateless: for each output frame it convolves the source
around `sourceAt(t)` with a windowed-sinc (Lanczos) kernel of 8 zero crossings each side, tabulated at 512 points per
zero crossing and linearly interpolated (`SincTable`, built during static initialisation, never on the audio thread).

Speeding up moves content above the output Nyquist, so the cutoff is lowered to `1 / rate` and the kernel widened by
the same factor: the content is filtered out instead of aliasing. The rate used for this is clamped to 1..4
(`kMaxRateFiltered`), a cost cap. The output is scaled by the cutoff to keep the gain.

## Invariants and real-time rules

- `AudioSource` is immutable after `load()`; snapshots hold `shared_ptr`s, and retired snapshots are freed on the UI
  thread once the audio thread's epoch has moved past them.
- `WarpVoice` construction and `configure()` allocate: only on the edit side. `render()`, `outputSeek()` and
  `renderResampled()` don't allocate or lock.
- A voice is used by one clip per block: `acquireVoice()` never hands out a voice stamped with the current block.
- Clips on one track don't overlap (the model resolves overlaps), which is what bounds voices per track.

## Extending it

- **A new warp mode**: add it to `WarpMode` (and to `WARP_MODES` in the model, in the same order), map it in
  `stretchConfigFor()`, and if it needs another block size add a `StretchConfig` and a `kTimings` entry
  (`kNumStretchConfigs` grows with it). Anything that changes the stretcher's settings per clip goes in
  `ClipRender` and is applied at the top of `WarpVoice::render()`.
- **Warp markers** (warping within a clip) are not implemented: a warped clip has one segment BPM. They would turn
  `ClipRender::rate` and `sourceAt()` into a piecewise mapping; `WarpVoice::render()` already feeds input by
  absolute source position, so it would follow.
- **Streaming long files** is not implemented; `AudioSource` would need a different storage behind `channelData()`.

## Gotchas

- `ClipDesc::durationSec` and `offsetSec` are in seconds of *source* audio; a warped clip's length in beats is
  derived from them and `segmentBpm`.
- A clip whose source is cached at another rate is left out of the snapshot until the source is decoded again.
- The Sampler's sample keeps the rate it was loaded at; it plays at `source rate / engine rate`, so its pitch stays
  right after a rate change.
- More than 64 voices per configuration, or 32 stretched clips on a track in one chunk, leave the extra clips
  silent.

## Tests

- [tests/engine/test_warp.cpp](../../tests/engine/test_warp.cpp): transpose shifts pitch but not length (and detune
  in fractions), warped clips follow the tempo and keep their pitch in every mode, slow down, are bit-exact at their
  own tempo, only warped clips rescale on a tempo change, are sample-aligned (also after a locate and when
  transposed), offline renders are repeatable, Re-Pitch changes speed and pitch together and filters rather than
  aliases, stereo and mono sources, clip pan, many warped clips in sequence, and exports include warped audio.
- [tests/engine/test_engine_render.cpp](../../tests/engine/test_engine_render.cpp): source peaks and samples,
  resampling to the engine rate, `probe`, clips waiting for their source.
