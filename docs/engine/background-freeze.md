# Background freezing (design)

**Status: a design, not implemented.** The measurements it cites were taken with
`benchmarks/plugin_cpu_bench.cpp` on the `perf-experiments` branch (real VST3 plug-ins: LSP, Dragonfly,
ZamAudio, DISTRHO; a 4-core Linux VM), and its instrumentation (per-device timing, silence detection) is
there too.

The engine renders what hasn't changed into a cache in the background, with CPU nobody else is using,
and plays it from there instead of running its devices. Anything that changes what a cached signal
would be invalidates only the part of the cache it touches, and that part plays live again from the
next chunk. Nothing of it shows: no UI, no files in the project, no change to undo, no edit that waits
for it. Manual freezing ([app/model.md](../app/model.md#freezing)) stays what it is: a user's decision,
in the project, that also unloads plug-ins. Background freezing only saves CPU.

## Why, and what the measurements say

On a 16-track song (instruments, EQ and compressor chains, two groups, a reverb and a delay return, a
mastering chain), rendered on one thread:

- Devices take **85%** of the render; everything else (clips, summing, faders, and the measuring itself)
  15%. A song played wholly from cache costs at most that 15% (less: cached strips don't render their
  clips either), plus reading the cache.
- **36%** of the devices' time is spent on calls whose input and output are silent; **56%** of calls get
  silent input. Plug-ins don't skip silence themselves. (Silence suspension, on the experiments
  branch, takes that part; caching takes the rest.)
- The longest chain of work that must happen in order (the slowest track, the reverb return, the
  master) is **11.4 s of a 31.7 s** render: no number of threads renders it more than 2.8× faster.
  A cached master and cached returns take that path away.
- Plug-ins cost more in small blocks: **+8%** at 128 frames, **+22%** at 64, **+37%** at 32, against 1024.
  The cache is rendered in 1024-frame blocks whatever the device's buffer.

And what constrains the design:

- **No plug-in reports a usable tail.** All 25 measured report 0 (`getTailSamples()`), reverbs and
  delays included. Tails, and how long a plug-in needs to warm up, are measured, never taken from it.
- **Plug-ins need warming up** before their output can take over from the cache, and how long varies a
  lot. Error of a plug-in started cold (from a reset) at a point inside a phrase, against one that ran
  from the start, over the 2 s after it (energy of the difference / of the signal):

  | Plug-in | Cold | Pre-roll to match (≤ −90 dB, or as close as two plain renders) |
  |---|---|---|
  | LSP Parametric EQ, Dynamics; Soul Force; ZamCompX2, ZaMaximX2 | identical | none |
  | LSP Filter | −49 dB | 10 ms |
  | LSP Limiter / ZamDelay | −24 dB / −13 dB | 0.2 s |
  | LSP Compressor | −46 dB | 0.5 s |
  | Dragonfly Plate | −26 dB | 1–2 s |
  | LSP Multiband Compressor / Graphic EQ x32 | −81 dB / −50 dB | 2 s |
  | MVerb | −9 dB | 4–8 s |
  | ZamVerb | −21 dB | never below about −85 dB |
  | Dragonfly Hall and Room, LSP Chorus | −12 to −27 dB | never: two plain renders already differ (−5 to −22 dB) |

  A switch from the cache to a cold plug-in is clearly audible (a delay's echoes and a reverb's tail go
  missing); a switch to a warmed one is not, and for most plug-ins is bit-exact.
- **Some plug-ins never render the same twice** (free-running random modulation: Dragonfly Hall and
  Room, LSP Chorus). Their cache is a different take, as good as the live one; switching between the
  two needs a crossfade, never a cut.
- **Plug-ins may work outside `process()`.** LSP's bundle runs a polling thread costing about 40% as
  much CPU as the whole render; the cache saves none of that. Not everything a plug-in costs is ours to
  save.

## Overview

```
 edit side (API thread, under mutex_)                 background (low priority)
 ┌──────────────────────────────────────┐             ┌───────────────────────────────────────────┐
 │ Engine API call                      │             │ Planner: what to render next, by value     │
 │  ├─ edit model, as today             │  dirty      │   (cost × how soon it plays)               │
 │  ├─ DependencyTracker:               │──ranges────►│ Cache renderers: shadow snapshot, shadow   │
 │  │   what changed, over which ranges │  stamps     │   plug-ins, their own Renderer, 1024-frame │
 │  │   → dirty log (snapshot)          │             │   blocks; warm-up and convergence checks   │
 │  │   → stamps (atomics, no rebuild)  │             │ Governor: only spare CPU                   │
 │  └─ rebuildSnapshotLocked()          │             │ Warm-up workers: plug-ins taking over      │
 └──────────────┬───────────────────────┘             │   from the cache                          │
                │ snapshot (one atomic swap)          └───────────────┬───────────────────────────┘
                ▼                                                     │ blocks (immutable),
 audio thread + workers                                               │ published per cache point
 ┌──────────────────────────────────────┐   CacheStore (RAM, disk)    │
 │ renderTrack(): per chunk, per point: │◄────────────────────────────┘
 │  valid cache? → read it, skip devices│   live capture: what plays live, unchanged,
 │  else → live (warm), crossfade seams │───────────────────────────► goes into the cache too
 └──────────────────────────────────────┘
```

Three rules keep it correct and out of the way:

1. **The snapshot and the stamps are the truth; the cache is a hint.** A cached block is played only if
   it was rendered from exactly what the current snapshot and stamps describe (one opt-in exception:
   the grace at an [unplanned seam](#seams)). The audio thread checks that itself, every chunk, so a
   stale block can't play, whatever the background threads are doing.
2. **Nothing waits for the cache.** Edits record what changed and return. The audio thread reads
   immutable blocks or plays live. Background work is cancelled by generation numbers, never joined
   from an edit.
3. **What the user can see moving keeps moving.** A device whose editor is open, whose display is
   shown, or that is being edited plays live.

## What is cached: cache points

A **cache point** is a signal at a fixed place in a strip:

- **A strip's output**: after its devices, before its fader: what manual freezing captures
  (`Renderer::renderTrackOffline`). Fader, pan, mute, solo and send levels apply after it, so moving
  them never invalidates the strip's own cache, and meters, pre- and post-fader sends work as they do
  now.
- **Checkpoints** inside a chain: the signal after device *k*. With one after the instrument, tweaking
  the EQ after a heavy synth plays the synth from cache and the EQ live. A device tapped by a sidechain
  (`EdgeRender::Tap::AfterDevice`) always has a checkpoint after it, or its strip can't be cached.
- **Buses** (groups, returns, the master) are strips like any other: their output point. Their input
  is what their edges bring, so a bus's cache depends on its sources' points and on their faders and
  send levels (see [Dependencies](#dependencies-and-invalidation)).
- **Variants**: a point keeps up to a few versions of its blocks, keyed by stamp (below). Soloing a
  track changes what goes into its group; un-soloing brings back the stamp the group's blocks were
  rendered with, and they play again without a new render. The same goes for A/B-ing a device's
  switch, or undoing a knob move.

Which checkpoints to keep is a cost question: the planner measures each device (the per-device timing
on the experiments branch) and keeps a checkpoint after the expensive prefix of a chain when what
follows it is cheap or is what the user edits. The default: the output of every strip; after the
instrument on instrument tracks; after a device whose cumulative cost is at least half the chain's.

Racks are one device to the cache (their chains, faders and macros are part of its state); caching
inside racks can come later.

## Time and blocks

Cached audio is stored in the **strip's own time**, as the renderer produces it at that point: the
timeline as late as the strip's input latency plus its devices' (see [routing.md](routing.md)). Each
block records the latency it was rendered with. A latency change *elsewhere* (another path, a
destination's delay compensation) only shifts where this strip's signal is read from: the block is
realigned, not invalidated. A latency change on this strip's own input path changes how its inputs line
up, and invalidates it.

Blocks are 16384 frames (0.34 s at 48 kHz) on a fixed grid, stereo float32 (exactly what the renderer
makes: no conversion, no loss, so a seam with live audio can be bit-exact). A block of zeros is stored as
a flag, not samples. (Storing near-silent blocks, below −120 dBFS, as zeros too would save more, since
tails rarely reach exact zero; it costs exactness, so it is a setting, off in tests.) Each block carries:

```cpp
struct CacheBlock {
    int64_t start;            // in the strip's time, a multiple of kBlockFrames
    uint64_t stamp;           // the point's stamp it was rendered with
    uint64_t generation;      // the edit generation it was rendered from (the dirty log's clock)
    int latency;              // the strip's arrival latency then
    Entry entry;              // how the render arrived here: Linear, or AfterWrap{loopEnd}
    bool silent;
    std::unique_ptr<float[]> samples;  // 2 × kBlockFrames, planar; none if silent (or on disk)
};
```

A point's blocks are an immutable sorted list (`BlockSet`), replaced by atomic pointer swap and retired
through the existing epoch pool (`DeferredReleasePool`), as snapshots are.

## Dependencies and invalidation

Two mechanisms, because the engine changes things two ways: by rebuilding the snapshot, and by storing
an atomic without one (gain, pan, mute, solo, send levels, device parameters:
[README.md](README.md#the-edit-model)).

**The dirty log** (time-local changes, in the snapshot). Every snapshot carries, per cache point, the
ranges changed, each with the edit generation that changed it. A block is invalid if a range overlapping
it has a newer generation than the block. An entry goes once every block it covers has been rendered
again or evicted, so the log stays as short as the edits not yet caught up with. The
`DependencyTracker` works the ranges out on the edit side, while `rebuildSnapshotLocked()` runs, from
what the call changed: the engine API replaces whole lists (`setTrackClips`, `setTrackNotes`,
`setTrackAutomation`), so the tracker compares the old list with the new one, by identity
(`ClipDesc::id`) and content, and keeps only what differs.

**Stamps** (time-global changes, atomics). Every cache point has a 64-bit stamp: a hash of everything
time-global it depends on. A block plays only if its stamp is the point's current stamp. Stamps are
*value-addressed*: each input contributes `hash(what, value)`, combined by XOR, so a change updates a
stamp in O(1) (`stamp ^= h(param, old) ^ h(param, new)`), and putting a value back restores the old
stamp, and with it the old blocks (undo, A/B, solo off). The API thread updates the stamps of the
point the change is at and of every point downstream of it (it knows the graph), with relaxed atomic
XORs; the audio thread reads one atomic per point per chunk. Each point is updated once per change even
when the change reaches it by several paths (a track's send and its group both going into the master):
an XOR applied twice would cancel. A point's stamp is the XOR of per-device and per-mixer-control hashes,
each kept up to date incrementally, so a structural change (which changes what a point depends on)
recomputes a stamp in time linear in its dependencies, not in their parameters.

| Change | Mechanism | Invalidates |
|---|---|---|
| A clip added, removed, moved, trimmed; its gain, pan, fades, warp, transpose; its source reloaded (a file changed, a reversal) | dirty log | The strip's points over the old and new extents of the clip, then downstream (below) |
| Notes added, removed or changed | dirty log | The changed notes' extents, from note-on to note-off |
| An automation envelope edited | dirty log | Where the old and new envelopes differ (worked out exactly, segment by segment), shifted by the lane's latency (`AutomationRender::latency`) |
| Device parameter (not automated), or a plug-in's editor edit (`ParamEdited`) | stamp | Its strip's points from that device on, and downstream |
| Plug-in state: `setProcessorState`, a preset loaded, `StateDirty`, `kReloadComponent`, parameter titles or values changed by the plug-in | stamp (a state epoch, or the hash of the state's bytes) | As above |
| State nobody reported: a plug-in's editor closed | the hash of `getState()` compared with the one rendered from (on the main thread, in `idle()`) | As above, if it differs |
| Device switched on or off, added, removed, moved; rack chains, macros | snapshot (structure) | Its strip's points from that device on, and downstream |
| Latency of a device | snapshot | Its strip's points from that device on; realignment (not invalidation) elsewhere |
| Fader, pan, mute, solo, send level | stamp | Not the strip's own points; the points of the buses it goes into (their inputs change), and downstream |
| Routing (outputs, sends added or removed, a send between pre- and post-fader, inputs from tracks) | snapshot | The old and new destinations' points, and downstream |
| Sidechain source or tap | snapshot | The keyed device's points onward |
| Tempo, time signature | snapshot | Every point: the engine has one tempo, so every position in samples moves (and plug-ins see tempo and bars) |
| Clip fade length, sample rate | snapshot | Everything |
| Loop on, off, moved | — | Nothing. A loop wrap is an entry context (below), not a change |
| Arming, monitoring, MIDI input, an editor opened | — | Nothing; the strip plays live while it lasts (a live input also makes everything downstream live) |

**How far a change reaches in time.** A change at the input of device *k* over `[a, b)` changes its
output from `a` until its effect has died away: its tail, which plug-ins don't report. So a re-render
doesn't stop at `b`: it carries on and compares its output with the old cache, block by block, and stops
when they match (≤ −120 dB over a whole block, twice in a row). The old blocks after that point stay
valid. That finds every plug-in's real tail, and a delay's last echo, without being told. A plug-in
that never converges (free-running modulation) is re-rendered to the end of the range, in the
background, lowest priority first.

The convergence check needs the old blocks until the re-render has passed them, so invalidation marks
blocks invalid for playback (the dirty log does that) but the store keeps them for comparison until the
re-render's frontier passes.

**Downstream.** A point's dirty range becomes its destination's dirty input range through each edge
(shifted by the edge's delay compensation), then goes through the destination's chain as above.
Rendering follows the same order: a bus's re-render waits for its sources' re-renders of the same
range, or reads them live.

## Eligibility: what is worth caching, and when

A strip (or a checkpoint) is cached once nothing has changed it for a while (**10 s** by default), and
it costs enough to be worth it (its devices take more than reading the cache does, about 1 µs per
chunk per point; in practice anything with a plug-in). It is not cached while:

- it is armed, monitored, recording, or hears MIDI input; then everything downstream is live too, as
  its signal is unpredictable;
- a device's editor is open, its display is shown in the device view, or its parameters are being
  dragged: the UI tells the engine which devices are observed (`Engine::setProcessorObserved`);
- it holds a device whose class is excluded (below);
- its last change was less than the idle time ago ("hot"). Hot strips play live even where valid
  blocks exist: they are likely to change again, and their warm instance is ready.

Excluded plug-in classes, learned or listed:

- **Nondeterministic** ones are *not* excluded (their cache is a take like any other), but they are
  noted: the planner renders twice once per class, in the background, on its own material; those whose
  renders differ get long crossfades at every seam (below).
- **Generative, external, or clock-reading** ones (network audio, hardware inserts, plug-ins reading the
  wall clock), and ones that talk to their other instances (a shadow instance would show up in their
  UI): a list per class id, kept with the plug-in index, editable in a hidden setting.
- Any device the verification mode (below) caught disagreeing with its cache.

## Rendering into the cache

**Shadow instances.** The cache can't be rendered by the plug-ins playing live: they are busy, and they
can't be in two places in time. Each cached device gets a *shadow*: a second instance of the same class,
created on the main thread as VST3 requires, given the live one's state (`getState`/`setState`), and
processed on the cache renderers' threads (processing off the main thread is what render jobs already
do). Shadows are rendered in `kRealtime` mode like the live ones, never `kOffline`: a plug-in that
renders better offline would make seams audible.

- **Main-thread budget.** Creating a shadow and copying state are main-thread plug-in calls that can be
  slow. They run in `Engine::idle()` within a budget (**4 ms per call**, like the 20 ms budget for
  destroying plug-ins), only when the user hasn't touched anything for **2 s** (the bridge passes on the
  time of the last input event), one instance at a time. A class whose live instance took more than
  **100 ms** to load gets shadows only after a minute of inactivity.
- **Memory.** A shadow costs what the plug-in costs (a sampler with gigabytes of samples, twice). The
  engine measures the process's memory before and after loading each live instance; a class over
  **200 MB** gets no shadow. Its strip is cached only by *live capture* (below).
- **State sync.** When a device's stamp changes, its shadow is out of date. The planner re-syncs it
  (main thread, budgeted) before rendering for the new stamp; until then the strip plays live.

**The shadow snapshot.** A cache render is an offline render of part of the graph: its own `Renderer`
(the class already supports several; offline renders make one), its own delay lines and stretch voices
(`setWarpVoices`, `setDelayLines`), and a *shadow snapshot*: the live snapshot with the cached strips'
processors swapped for their shadows, and the strips it doesn't render replaced by their cached outputs
as inputs. It renders in 1024-frame chunks. No renderer change is needed beyond rendering a subset of the
graph and reading inputs from the cache.

**Warm-up.** A render of `[a, b)` starts at `a − W`: the strip's input is replayed from `a − W` and its
output thrown away until `a`. `W` is learned per device (below). Where valid old blocks overlap the
warm-up, the render compares with them: matching blocks mean the shadow is warm, so it can start
writing early; not matching by `a` means `W` was too short, so it is lengthened for that class.

**Learning warm-up and tail.** Per plug-in class and per instance, kept with the plug-in index across
sessions: how long it took to converge on entry (warm-up) and after a change (tail), as the
re-renders above measure them. A class not seen yet starts with 8 s. (Measured values above: from none
to several seconds; never for free-running modulation.)

**Live capture.** What plays live is cache material too, for free: while a strip plays live with its
stamp unchanged, no dirty range, and the live instance warm (it has played continuously from at least
`W` before), the renderer copies its points' signals into a per-strip ring (preallocated, two seconds),
which the store's thread turns into blocks. The second time a section plays (a loop, a section listened
to again), it plays from the cache. This is how plug-ins too big for a shadow get cached, and it needs no
background CPU at all. The capture is only valid in the context it played in: a block captured right
after a loop wrap is stored as `AfterWrap{loopEnd}`, not `Linear`.

**Where the CPU comes from.** Cache renderers are threads of their own (as many as the cores the audio
threads don't use, at least one), at the lowest priority the system offers: `THREAD_PRIORITY_IDLE` and
EcoQoS on Windows (which on hybrid CPUs puts them on efficiency cores, leaving the performance cores to
the audio threads), `SCHED_IDLE` on Linux. They never take MMCSS. A governor watches the live callbacks
(load, and how close the slowest of the last second came to its buffer): above **60%** it halves the
renderers' share, above **80%** or after a dropout it pauses them for 5 s. On battery power it renders
only what plays next. The audio threads preempt them anyway; the governor is for what preemption doesn't
cover (shared caches, memory bandwidth, turbo budget).

**What to render first.** Each candidate range has a value: what its devices cost per second (measured)
× how soon it will play. In order: the next seconds after the playhead on the costliest strips, the loop
range (and its `AfterWrap` blocks) while looping, then outward from the playhead, then the rest of the
arrangement. Stopped, outward from the playhead. Jobs are a few blocks long, so a new plan takes effect
within milliseconds; a job whose inputs changed under it (its generation or stamp is out of date when it
finishes) is dropped.

## Playing from the cache

Per chunk, `renderTrack()` asks each of the strip's points, last first: is there a valid block over the
whole chunk? (Its stamp is the point's current stamp; no dirty range newer than it; resident in RAM; the
right entry context.)

- **The output point is valid**: the chunk is read into the track's buffer; no clips, no devices. The
  fader, sends and meters run as now.
- **A checkpoint after device *k* is valid**: it is read, and devices *k+1…n* run live.
- **Neither**: everything runs live, as now.

Reading a block is a copy (or a fill, for a silent block), outside any lock. A strip played from its cache
doesn't call its devices, so they don't run, and its upstream strips run only if something else needs
them: they are cached too in the common case, so reading them is cheap; their meters need their signal.
(A later refinement: a group whose whole subtree is cached plays only its own output, and its tracks'
meters play from peak envelopes stored with the blocks.)

The decision is per chunk and per strip, and doesn't depend on which thread renders the strip, so the
scheduler and its determinism are unchanged.

## Seams

A seam is where a strip's output changes between the cache and live devices. Every seam is a crossfade:
**5 ms** where both sides should agree and the class is deterministic (warmed, they are identical or
within −90 dB, so it is inaudible), **50 ms** for nondeterministic classes (two takes of the same reverb),
**20 ms** where they differ on purpose (an edit).

**Planned seams** (the cache's coverage ends ahead, the strip turns hot, a locate into an uncached
range). The live instance must be warm when the playhead reaches the seam at `s`. When the playhead is
`W` + margin before `s`, a warm-up worker takes the idle live instance (`ProcessGuard`: the audio
thread isn't calling it, the strip being cached), replays the strip's input over `[s − W, s)` into it,
faster than real time, and hands it back. At `s` the audio thread calls it as usual. Entering the cache
needs nothing: the cache was rendered warm across its start.

**Idle instances are kept clean.** Once a strip plays from its cache, its live instance isn't called, and
its state goes stale (old echoes, an old tail). So it is reset then, on the main thread in `idle()`
(`resetOffline()`: deactivate and activate, the only full reset VST3 has; budgeted like the rest). An
instance taken up suddenly starts from silence, never from stale audio.

**Unplanned seams** (an edit invalidates the cache at the playhead). The edit is heard at once; the tail
catches up a moment later:

1. **Next chunk**: the edit's snapshot or stamp makes the blocks invalid, and the strip runs live on its
   (clean, cold) live instance, crossfading from the old blocks over 20 ms. A cold instance has no tail
   yet: a reverb or delay would be thin until its tail builds up again (−9 to −46 dB of error above).
2. **Meanwhile** a warm-up worker gives the strip's *shadow* instance the same change (parameters go
   through its lock-free queue; a new state through the main thread's budget) and replays the strip's
   input into it from `s − W` up to just ahead of the playhead, at full speed. (A shadow busy rendering
   the cache gives way within a block; that job is dropped anyway, as its inputs just changed.) A track's chain runs at
   tens to hundreds of times real time, so a few seconds of warm-up take a few to tens of milliseconds.
   With little time, it warms as far back as it can: the most recent input matters most.
3. **Swap**: at the first chunk after the shadow has caught up, the two instances swap roles. The
   audio thread calls the warmed one from there on, crossfading from the cold one (5 ms, or 50 ms for a
   nondeterministic class). The cold one becomes the shadow. The tail is back, usually within 50 ms of
   the edit.

A strip whose plug-ins have no shadows (too big) can't swap. For those, a setting chooses: the cold
switch alone (the edit at once, a thin tail for a moment), or a short grace (up to 50 ms) during which the
pre-edit blocks play on, from a list the edit itself handed over, while the live instance warms. The
grace is the one place where invalidated blocks may play.

The strip's input for warm-ups comes from its clips and notes (rendered again: deterministic), its
sources' cached blocks, or, for sources that play live, a ring of their last 8 s of output, which every
strip that feeds a cacheable one keeps.

**Loop wraps.** Live, a reverb tail at the loop's end carries into its start. The linear cache at the loop
start doesn't have it (it was rendered from what precedes the start on the timeline). So while looping,
the planner renders `AfterWrap{loopEnd}` blocks: the strip from `loopEnd − W` to `loopEnd`, then on from
`loopStart` until its output converges with the linear blocks there (its tail). At a wrap, playback
reads `AfterWrap` blocks until they run out, then the linear ones. Moving the loop needs new `AfterWrap`
blocks only.

**Locate.** Live, plug-ins keep their state across a locate (only notes are released), so the old
position's tails ring on into the new one. From the cache a locate plays the linear render, as an export
would. That is the one audible difference by design, and arguably an improvement (what plays after a
locate no longer depends on where the playhead was before).

**Stop.** Live, tails ring out after a stop. A strip playing from its cache at the stop gets its tail
from its live instance (idle and clean): a warm-up worker warms it on the strip's input up to the stop
point, then the audio thread runs it on silence. Until it is warm (usually within 50 ms) the cached
block after the stop point fades out under it.

## Never holding up an edit

- The edit side does, under the existing `mutex_`, what it does now plus the tracker's diff: a clip
  list compared with its predecessor, an envelope with its old self. That is linear in what changed,
  allocation-light, and has no I/O.
- Stamps are atomics, updated on the calling thread; no lock is shared with the audio thread or the
  cache renderers.
- Nothing on the edit side waits for a background thread: jobs are cancelled by generation and dropped
  when they finish late. Block sets are replaced by pointer swap and freed through the epoch pool.
- Main-thread plug-in calls the cache needs (shadows, state copies, hashing state when an editor closes)
  run in `idle()`, budgeted, and never while the user is interacting.
- The cache store's disk I/O is on its own thread. The audio thread only reads blocks a prefetcher has
  made resident; a missing block is a cache miss (the strip plays live), never a page fault.

## The store

- **RAM** first: a budget (default a quarter of physical memory, at most 4 GB) shared by all points,
  allocated in 16384-frame blocks from a pool. Silent blocks take no samples. (A 4-minute song is 92 MB
  per point at 48 kHz: 40 tracks with a checkpoint each are 7.4 GB, about half that if half the blocks
  are silent. So RAM holds what plays soon, and disk the rest.)
- **Disk** behind it, in the user's cache folder (never the project folder), keyed by project id: blocks
  evicted from RAM go there, and a prefetcher keeps the next 10 s after the playhead, the loop range and
  the warm-up ring resident. An SSD reads what 80 points play (31 MB/s) without trying.
- **Eviction** by value: CPU saved per byte, times how soon it plays; old variants first.
- **Persistence** (later): block keys are content (stamps and the dirty log's content hashes), so a project
  opened again can find its blocks on disk and play from them at once.

## Interaction with the rest of the engine

- **Manual freezing** is unchanged and takes precedence: a frozen track has no devices to cache.
- **Exports and freezes** (`exportWav`, `startTrackRender`) may read blocks whose entry context is
  `Linear` from the start of the arrangement; otherwise they render as now. An export of an unchanged,
  fully cached song is a copy.
- **Recording** makes its tracks live; a take that lands is a clip edit.
- **Resampling** (an input edge from a track) reads the source's output, from the cache or not.
- **Sidechains**: the keyed device depends on the source's tap point like any input.
- **Silence suspension** (experiments branch) and caching complement each other: silent blocks are free
  to store and free to play; a strip playing live sleeps its silent devices.
- **Offline renders** suspend live output as now; cache renderers pause during them (they share no
  processors, but a render job wants the CPU).
- **Sample rate change** clears everything.

## Changes by file

| Where | What |
|---|---|
| `engine/src/cache/CacheTypes.h` | `CachePointId`, `CacheBlock`, `BlockSet`, `Entry`, stamps |
| `engine/src/cache/DependencyTracker.h/.cpp` | Edit side: list and envelope diffs into dirty ranges, stamp updates downstream, the dirty log in the snapshot |
| `engine/src/cache/CacheStore.h/.cpp` | Blocks in RAM and on disk, the prefetcher, eviction, live capture rings |
| `engine/src/cache/CachePlanner.h/.cpp` | Eligibility, values, jobs, the governor, learned warm-ups and tails |
| `engine/src/cache/CacheRenderer.h/.cpp` | Shadow snapshots and instances, warm-up and convergence, the warm-up workers |
| `Snapshot.h` | Per strip: its cache points, their dirty logs; per `TrackBuffers`: seam state (crossfade, warm-up handover) |
| `Renderer.cpp` | The per-chunk decision in `renderTrack()`, starting `processChain()` after a checkpoint, capture |
| `Processor.h`, `Vst3Processor` | A stamp contribution per device (parameter hash, state epoch); `ProcessGuard` used by warm-up workers |
| `EngineSnapshot.cpp`, `EngineTracks.cpp`, `EngineChains.cpp` | Calling the tracker where the model changes |
| `Engine.h` | `setProcessorObserved()`, `noteUserActivity()`, a hidden `setBackgroundFreezing(settings)`, statistics for tests |
| `app/src/audio` | The bridge passes on observed devices (open editors, device views) and the time of the last input event |

## Testing

- **The rule that matters**: with deterministic devices (the test plug-ins, the built-in devices), a song
  played or rendered with the cache on is the same as with it off, to the sample, through any sequence of
  edits. A property test in the style of the random-graph tests in
  [test_parallel_engine.cpp](../../tests/engine/test_parallel_engine.cpp): random arrangements, random
  edits between chunks, cache on and off, compared.
- **Seams**: with real plug-ins, the error at seams against an uncached render (the `warmup` mode of
  `plugin_cpu_bench`), per class.
- **Never blocking**: the time of every API call with cache renderers busy, against without; no call may
  get slower by more than its diff.
- **Live**: edits during playback through the Bench driver (experiments branch): the strip goes live at
  the next chunk, the edit is heard within 50 ms, no dropouts.
- **Verification mode** (hidden setting, for development and for catching unreported plug-in state):
  every so often a cached range is also rendered live, quietly, and compared; a class that disagrees
  beyond its known nondeterminism is excluded and logged.

## Plan

1. **Live capture and playback** (no shadows, no background rendering): stamps, the dirty log, the RAM
   store, capture, playing from the cache, idle instances reset, planned seams warmed, unplanned ones
   with the cold switch or the grace. Gain: a section heard twice costs devices once, which is most of
   mixing.
2. **Background rendering**: shadow instances, the planner and the governor, warm-up and convergence,
   learned tails. Gain: playback of anything unchanged costs no device time at all (85% of the render in
   the song above).
3. **Reuse inside and across strips**: checkpoints, buses and variants, realignment on latency changes,
   the disk store, instance swaps for unplanned seams.
4. **Polish**: a persistent cache, exports from the cache, collapsing cached groups, undo
   revalidation of dirty ranges by content hash.

## Open questions

- **Hidden plug-in state.** A plug-in can change without telling the host (an internal randomiser, a
  file it watches). Hashing state when its editor closes, and verification mode, narrow this; they
  can't close it. The exclusion list is the backstop.
- **Block-size dependence.** Some plug-ins smooth parameters per block, so a cache rendered in 1024-frame
  blocks differs slightly from live 64-frame blocks under automation. The 5 ms crossfade hides it at
  seams; the cache's own content is arguably the better render.
- **Two instances of one plug-in.** Licence limits, demo modes and plug-ins that show their other
  instances could make shadows visible. Per-class exclusion handles known cases; capture-only caching is
  the fallback.
- **Memory pressure** from shadows on large sessions: the budget may need to drop shadows of strips that
  rarely play rather than refuse new ones.
- **Tempo-independent plug-ins** could keep their caches across tempo changes, if the planner probed them
  (render at two tempos, compare). Not worth it until tempo changes turn out to matter in practice.
