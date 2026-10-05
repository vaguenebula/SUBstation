# Scheduler

The scheduler runs the routing graph's tracks on the thread that renders (the audio thread, or
the one rendering offline) and a fixed pool of worker threads. It lives in
[Scheduler.h](../../engine/src/Scheduler.h) and [Scheduler.cpp](../../engine/src/Scheduler.cpp);
the renderer drives it from the middle of each chunk (see [rendering.md](rendering.md#a-chunk)),
and the graph it runs is built with each snapshot ([routing.md](routing.md)).

## Files

| File | What it holds |
|---|---|
| [Scheduler.h](../../engine/src/Scheduler.h) / [.cpp](../../engine/src/Scheduler.cpp) | `TaskGraph` (the dependency graph, its counters, ready queue and ranks) and `Scheduler` (the workers and `run()`). |
| [Renderer.cpp](../../engine/src/Renderer.cpp) | `renderChunk()` decides whether to run in parallel and sets the costs; `renderNode()` is the job; `renderTrack()` measures each track's cost. |
| [EngineSnapshot.cpp](../../engine/src/EngineSnapshot.cpp) | Builds the snapshot's `TaskGraph` and counts `parallelWork`. |
| [Engine.cpp](../../engine/src/Engine.cpp) | `setAudioThreads()`, `defaultAudioThreads()`, `setCostOrdering()`, `trackCosts()`, `nodesOnWorkers()`. |

## The graph

`TaskGraph(nodes, edges)` is made with each snapshot: node `i` is the snapshot's `i`-th track,
and there is an edge `(from, to)` for each routing edge between two tracks (outputs into groups,
sends into returns, input edges, sidechains). Edges into the master go nowhere the graph runs.
The nodes are already in topological order, so every node comes after those that go into it.

It stores the outgoing edges as flat arrays (`first_`, `destinations_`), each node's input
count, and, allocated with it so a run never allocates: per-node pending counters, a queue of
ready nodes (one slot per node), `head_`/`tail_`/`done_` counters on separate cache lines, costs
and ranks, and the roots (nodes without inputs), kept in their last order from run to run.

## A run

`Scheduler::run(graph, job, context, parallel)` runs `job` once for every node, each after the
nodes that go into it, and returns when all are done:

```
reset: pending[i] = inputCount[i]; queue empty; push every root (in rank order)
open the run: state_ = (run number << 1) | 1; wake sleeping workers
every thread (caller as worker 0, pool as 1..):
    take the node at head (CAS head -> head + 1); none ready yet -> pause, then yield
    job(context, node, worker)
    for each destination: if --pending == 0, push it      (acq_rel)
    ++done
    stop when head has passed every node
caller: wait until done == size; close the run; wait until no worker is inside
```

- No locks and no allocation: the counters and the queue are the graph's. Every node is queued
  exactly once per run, so the queue's slots never run out.
- The last input to finish queues a destination; its `fetch_sub` is `acq_rel`, so whoever runs the
  destination sees everything every input wrote (their buffers and edge buffers).
- `parallel` false, no workers, or fewer than two nodes: every node on the calling thread, in the
  graph's order (inputs first).
- One run at a time. The live and offline renderers share one scheduler, never at once: offline
  renders suspend live output first.

`worker` is 0 for the calling thread and 1.. for the pool's; the renderer uses it to pick that
thread's `WorkerScratch` (`Renderer::renderNode`). `nodesOnWorkers()` counts the nodes the pool's
workers rendered since the scheduler started (tests and benchmarks use it to see them work).

## Workers

`Scheduler(threads)` starts `threads - 1` workers; `threads` counts the caller's.

- On Windows each worker joins MMCSS as "Pro Audio" (`AvSetMmThreadCharacteristicsW`, linked with `avrt`)
  for its life; elsewhere workers keep the default priority. Every worker flushes denormals while it renders
  (`ScopedNoDenormals`).
- Between runs a worker spins (with `_mm_pause`) for about 50 microseconds: consecutive chunks of
  one callback come within microseconds. Then it sleeps on `state_` (`std::atomic::wait`) until
  the next run opens. The caller only calls `notify_all()` if some worker is sleeping
  (`sleepers_`).
- Inside a run, a worker waiting for a node to become ready pauses 2000 times, then yields.
- A worker joining a run counts itself into `active_` first, then checks the run is still open;
  the caller closes the run and waits for `active_` to reach zero before returning, so no worker
  can touch the graph or the renderer after `run()` returns.
- `kQuit` in `state_` ends the workers; the destructor sets it and joins them. If a thread can't
  start, those that did are ended and the exception propagates.

**How many.** *Preferences › Audio › Audio Threads* (`Engine::setAudioThreads`) sets how many threads
render: one per core but one by default
(`defaultAudioThreads()`, at least 1, at most `kMaxAudioThreads` = 64); 1 means no workers.
Offline renders and exports use the same threads. Changing it suspends live output (silence),
waits for a callback to pass, swaps in a new `Scheduler` (the renderer reallocates its
per-thread scratch), ends the old one's workers and resumes; a recording goes on, the playhead
waits as well.

## When it runs in parallel

`Renderer::renderChunk()` runs the graph in parallel only if the snapshot has at least two tracks
worth a thread of their own (`parallelWork`: tracks with an enabled device, or clips to stretch
or resample) and `parallelWork * frames >= kMinParallelWork` (256). Small chunks with little work
render serially: waking workers would cost more than they save. Without a graph or a scheduler,
tracks render in snapshot order on the calling thread.

## Ordering by the heaviest path

Each track's render is timed (`renderTrack()`, from clearing its buffer to the end of its edges:
nothing in between waits) in nanoseconds per frame, smoothed over roughly the last ten chunks
(`kCostSmoothing` 0.1), into `TrackBuffers::cost`. A synth costs more while it plays a chord, a
plug-in's first blocks after loading more than later ones; the smoothing follows that.
`trackCosts()` reports them; 0 means not rendered yet.

Before a parallel run the renderer hands the graph each track's cost (`setCost()`) and calls
`orderRoots()`:

- Each node's **rank** is the work from it to the end of its longest path: its own cost plus the
  highest rank among its destinations. Destinations come after a node in the graph's order, so
  one backward pass computes them all.
- The roots are queued by rank, highest first; equal ranks keep the graph's order. They are
  sorted with an insertion sort, since costs change slowly and the last run's order is nearly
  right.

So the tracks with the most work hanging off them (their own, and that of everything they go
into) start first, and a heavy track, or a light one feeding heavy groups, never starts last.
Only roots are ordered; other nodes are queued as they become ready.

`setCostOrdering(false)` gives every node cost 0, so the roots keep the
graph's (routing) order; it is for benchmarks. The order only changes how soon a run is done,
never what it computes.

```
  t0 (1.0)  ------------------------------> master     ranks: t0 1.0
  t1 (2.0)  ----------------> G5 (5.0) ---> master            t1 7.0, t2 5.5
  t2 (0.5)  ---------------->   ^                             t3 6.25, G4 6.0
  t3 (0.25) ---> G4 (1.0) ------+                             G5 5.0, t6 4.0
  t6 (4.0)  ------------------------------> master     queued: t1, t3, t2, t6, t0
```

(This is the hand-made graph the test "the longest paths start first" checks.)

## Determinism

Renders are bit-identical on any number of threads. That holds because:

- The prologue works out everything tracks share (notes, MIDI input, recording, stretch voices,
  solo) serially, before the graph; the epilogue (the master, recording rendered outputs, the
  metronome) runs after it.
- A track renders only from its own `TrackBuffers`, its incoming edges (all done when it starts),
  its own devices and their state; it writes only its own buffer and its edges' buffers.
- A bus sums its incoming edges in the snapshot's fixed order, and the master sums its edges in
  order after the graph, so floating-point sums never depend on which thread finished first.
- Stateful devices (synths, plug-ins) may render on a different thread each chunk; their state
  moves with them, and the `acq_rel` handoff makes it visible.
- Ordering by cost changes when a track starts, not what it computes.

## Invariants and real-time rules

- `run()` and the job never lock, allocate or free. Everything is preallocated with the graph
  (per snapshot) or the renderer's per-thread scratch (per `setScheduler()`).
- A job must depend only on its inputs and its own state, never on which thread runs it or when.
- The graph must be acyclic and listed inputs first; the snapshot builder guarantees both.

## Extending it

- New work inside a track's render needs no scheduler change, as long as it touches only that
  track's state. Work shared across tracks belongs in the renderer's prologue or epilogue.
- A new dependency between tracks must be a graph edge (in `graphEdges` in
  `rebuildSnapshotLocked()`), or the scheduler may run the destination before the source.
- Per-thread scratch goes in `Renderer::WorkerScratch` and is allocated in `setScheduler()`.

## Gotchas

- `nodesOnWorkers()` starts again from 0 whenever the number of threads is set (a new
  `Scheduler`).
- A chunk with a single track worth a thread (`parallelWork` 1) renders serially however many
  threads there are.
- `TaskGraph` is part of a `const` snapshot but is written during runs (counters, costs, ranks);
  that is safe only because one renderer runs at a time.

## Tests and benchmarks

- [tests/engine/test_parallel_engine.cpp](../../tests/engine/test_parallel_engine.cpp): the number of audio threads;
  one track and a stateful plug-in on any thread; random routing graphs (nested groups, sends
  before and after the fader, inputs from other tracks, sidechains, latent plug-ins, solo, mute,
  automation, looping) bit-identical on 1..N threads; a stress test; few tracks rendering
  serially; ranks and queue order on a hand-made graph (`taskGraphOrder()`, in
  [harness/TaskGraphOrder.h](../../tests/engine/harness/TaskGraphOrder.h)); measured costs; cost
  ordering changing nothing but the order.
- [tests/engine/test_parallel_live.cpp](../../tests/engine/test_parallel_live.cpp): workers rendering live through
  the fake ASIO driver in manual mode, with silent tracks beside the tested ones so every buffer's
  tracks are shared out: the MIDI input, recording and resampling tests again (live, held,
  recorded and preview notes, recorded audio), send ramps and sidechains live. Skipped without
  ASIO.
- "ranks follow the longest path through sends" in
  [tests/engine/test_sends_engine.cpp](../../tests/engine/test_sends_engine.cpp), and the "with and without
  workers" tests in the rack and sidechain tests; the MIDI input, recording and resampling tests run again
  live with workers.
- [benchmarks/parallel_render_bench.cpp](../../benchmarks/parallel_render_bench.cpp) (built with
  `-DSUBSTATION_BUILD_BENCHMARKS=ON`): `build/bin/parallel_render_bench` compares render times on 1..N
  threads (offline, or live through the fake ASIO driver with `--live`), and with `--heavy N --compare-ordering`
  the gain from starting heavy tracks first. Results are in [benchmarks/README.md](../../benchmarks/README.md).
