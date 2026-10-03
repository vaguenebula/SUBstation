# Routing

Routing in the engine is a graph: strips (tracks, and the master) are nodes, and every way a
strip's signal goes on is an edge. Groups, returns, sends, resampling inputs and sidechains are
all edges to the engine; it never sees a group as such. The graph and delay compensation are in
[Routing.h](../../engine/src/Routing.h); the edges are made and checked in
[EngineTracks.cpp](../../engine/src/EngineTracks.cpp) (outputs, sends) and
[EngineChains.cpp](../../engine/src/EngineChains.cpp) (racks, sidechains), and turned into the
snapshot in [EngineSnapshot.cpp](../../engine/src/EngineSnapshot.cpp).

How the renderer plays edges is in [rendering.md](rendering.md); how groups, returns,
sidechains and delay compensation look to the user, in [guide/mixing.md](../guide/mixing.md)
and [guide/devices.md](../guide/devices.md).

## Files

| File | What it holds |
|---|---|
| [Routing.h](../../engine/src/Routing.h) | `RouteEdge`, `ChainSlot`, `alignInputs()`, `topologicalOrder()`, `wouldCycle()`, `GraphLatencies` and `alignGraph()`. Header-only, no engine types: it works on indices. |
| [Rack.h](../../engine/src/Rack.h) | `RackProcessor`: a rack's place in a chain. |
| [EngineTracks.cpp](../../engine/src/EngineTracks.cpp) | `setTrackOutput`, `setTrackSend`, `removeTrackSend`, `removeTrack`, `routeEdgesLocked()`, `sidechainTapLocked()`, `checkRouteLocked()`, `checkSidechainLocked()`. |
| [EngineChains.cpp](../../engine/src/EngineChains.cpp) | Chains, racks (`addRack`, `addRackChain`, `moveProcessor`, nesting checks), `stripSlotsLocked()`, `setProcessorSidechain`. |
| [EngineInput.cpp](../../engine/src/EngineInput.cpp) | `setTrackInputTrack`: input edges (resampling). See [recording.md](recording.md). |
| [EngineSnapshot.cpp](../../engine/src/EngineSnapshot.cpp) | Ordering the graph, delay compensation, `EdgeRender`s, `RackRender`s, the `TaskGraph`. |
| [Snapshot.h](../../engine/src/Snapshot.h) | `EdgeRender`, `EdgeState`, `DelayLine`, `RackRender`, `ChainRender`, `InputEdge`. |

## Edges

Each track has:

- **One output edge** (`Kind::Output`), post-fader, into the master or into another track,
  which sums it into its own input: that track is a group's bus. `setTrackOutput(track, to)`;
  `kMaster` is the default. When a track goes, what went into it goes to the master.
- **Any number of sends** (`Kind::Send`), into other tracks (return tracks), at a level
  (`EdgeState::gain`), tapped after the fader or before it (`preFader`). One send per pair:
  setting it again changes it, and a new level alone is just an atomic store (no new snapshot).
  A muted track's sends are silent either way. Sends to the master are refused. When a track
  goes, the sends into it go too. A send's level automation is a mixer lane of the sending track,
  `send:<track id>` (see [automation.md](automation.md)).
- **An input edge** (`Kind::Input`), if its input is another track's output
  (`setTrackInputTrack`): from that track into this one, tapped after the source's fader and
  before any delay compensation. It orders the graph (the source renders first) and closes cycles
  like any edge, but isn't summed or aligned: the track hears it instead of its clips while
  monitored, and records it. Resampling the master is no edge: the master renders after every
  track. See [recording.md](recording.md).
- **Sidechains** (`Kind::Sidechain`): an edge from the source track to one device of the
  destination strip (its aux input), kept with the device (`ProcessorEntry::sidechain`), so it
  moves with it. See [Sidechains](#sidechains).

In the engine's terms (`routeEdgesLocked()`), the edges are listed as `RouteEdge`s between
indices into `tracks_` (-1: the master): each track's output then its sends; then the input
edges; then the sidechains, by destination (the tracks, then the master's devices) and device
slot. `RouteEdge::sums` is false for input edges and sidechains; `tap` says where along the
source's chain it leaves; `device` which slot of the destination a sidechain goes into.

The model behind them, per track (`TrackModel`): `output`, `outputState`, `delay` (the output
edge's line), `sends` (`SendModel`: destination, pre-fader, `EdgeState`, `DelayLine`),
`inputTrack` and `inputState`. Per device: `SidechainModel` (source, tap, tap device,
`EdgeState`, its own delay line and the device's). These outlive snapshots; the snapshot's
`EdgeRender`s point at them.

### In the snapshot

`EdgeRender` is an edge as the renderer plays it: `from` and `to` (snapshot track indices; -1
the master), `kind`, `tap` (`PostFader`, `PreFader`, `AfterDevice` with `tapDevice`),
`compensation` and its `delay` line, `state`, a send's automated `level`, and for a sidechain
the `device` and `deviceDelay` (with its line). Two helpers decide how it is played:

- `ownSignal()`: the source writes the edge's signal into the edge's own buffer when it is
  tapped anywhere but after the fader, or delayed for this edge alone. Otherwise the destination
  reads the source's buffer directly.
- `sums()`: outputs and sends are summed by their destination; an input edge is heard only while
  monitored, a sidechain only by its device.

Snapshot edges are in snapshot order by source: each track's output, then its sends, then the
input edges and sidechains it feeds. Each `TrackRender` lists its `incoming` edges (what it sums,
in that order) and `outgoing` ones; the master's summed edges are `masterInputs`. Summing in this
fixed order is what makes renders independent of the thread that finished first.

## Ordering and cycles

The graph must be acyclic. An edge that would close a cycle is refused where it is made, with
`std::invalid_argument` (`ValueError` in Python):

- `wouldCycle(count, edges, from, to)`: true if `to` is `from`, or already feeds it along some
  path (a depth-first walk over every edge kind). Edges into the master never close one.
- `checkRouteLocked()` guards outputs and sends; `setTrackInputTrack` does the same for inputs;
  `checkSidechainLocked(source, strip)` for sidechains (any source is fine for the master's
  devices: everything goes into the master, so nothing it feeds feeds a track).
- `moveProcessor()` checks a moving device's sidechain, and every sidechained device inside a
  moving rack (`checkRackSidechainsLocked()`), against the strip it goes to.

What the user sees greyed out (a group's own tracks as its sidechain source, a return feeding the
one sending to it) the Python model works out the same way; the engine's checks are the last word.

`topologicalOrder()` then lists the tracks so that each comes after everything that feeds it,
keeping the given order where it can (a stack seeded in reverse). That is the snapshot's track
order. Should it ever find a cycle (it can't, as every cycle is refused), the snapshot builder
asserts and falls back to every track straight into the master, without sends, inputs or
sidechains.

## Groups and returns

A group is a track whose input is the sum of the output edges going into it; a return is a
track whose input is the sum of the sends going into it. Neither has clips (the Python model sees
to that). Both are ordinary strips: devices, fader, their own output edge and sends (a return can
send on into another return). Groups nest: a group's output goes into another group. Removing a
track sends what went into it to the master and takes away the sends into it, the input edges
and sidechains from it, and its chains with their devices.

## Delay compensation

Plug-in delay compensation happens at every summing point, per edge (`alignGraph()`):

```
arrival(edge) = inputLatency[source] + latency of the source's devices before the tap
heard(point)  = max(arrival of its incoming edges)            (alignInputs)
compensation  = heard(point) - arrival(edge)                  per edge
```

- A bus (a group, a return), the master, and inside a strip a rack, hears its inputs as late as
  the latest of them; each other edge is delayed by the difference, after its source's fader. A
  track sending to two returns of different latency is delayed differently on each edge.
- Taps after the fader and before it both come after every device of the strip, so they arrive
  equally late; a tap after a device arrives as that device's signal leaves it.
- `inputLatency` of a node is how late it hears its summed inputs. Its own clips and notes play
  on time: they aren't delayed to line up with its inputs (groups and returns have none).
- An input edge isn't summed, so nothing lines up with it.
- The master hears its inputs `maxLatency` late; its devices add `master.latency`; the output
  lags the timeline by `outputLatency()`. The metronome is delayed as much, and offline renders
  render that much ahead and drop it ([rendering.md](rendering.md)).
- A device's latency counts only while it is switched on (`insertLatency()`, clamped to 2^20
  samples, and 2^20 per strip in all). Switching one on or off, or a plug-in reporting a new
  latency (found in `idle()`), rebuilds the snapshot.
- A monitored track's edges aren't delayed (`Renderer::compensationFor()`), so a player hears
  themselves with only the latency of the track's own devices.

Traversal is bottom-up in topological order: a source's latencies are known before the arrivals
of its edges are needed. `GraphLatencies` holds the results: `inputLatency` per node,
`compensation` and `deviceDelay` per edge, and per node (the master last) per slot
`deviceLatency` (how late each slot hears its signal; the last entry: the strip's latency),
`deviceOut`, and for racks `chainEnd` and `chainCompensation`.

**Delay lines.** `ensureDelay()` keeps an edge's `DelayLine` across snapshots if it is long
enough (it holds what it delays), else makes one of `2 * samples + kMaxBlock`. A `DelayLine`
restarts from silence when its delay changes rather than replaying stale audio. Offline renders
bring lines of their own.

**Automation follows the latency.** Every envelope is delayed by how late its target hears the
timeline: a device's by the strip's input latency plus the devices before it; a fader's by the
strip's input latency plus all its devices; a send's level by how late its destination hears its
inputs (it is applied where the edge is summed, after the edge's delay); a rack chain's fader by
the end of its devices. So automation stays with the audio. See [automation.md](automation.md).

## Sidechains

`setProcessorSidechain(processor, sourceTrack, tap, tapProcessor)` gives a device with an aux
input (`Processor::hasSidechain()`) a sidechain from a track, a group or a return, never the
master (it renders after everything). The tap (`SidechainTap`):

| `SidechainTap` | User's name | Edge tap |
|---|---|---|
| `PostFader` | Post Mixer | after the fader and pan |
| `PreFader` | Post FX | after all its devices, before the fader |
| `AfterDevice` | After a device | after `tapProcessorId`, a device in the source's main chain |
| `PreFx` | Pre FX | before all its devices (`AfterDevice` with tap 0) |

`sidechainTapLocked()` turns that into the edge's tap. A tap after a device that has left the
source's main chain (or is in a rack there) is before the fader until the device comes back.

**Alignment.** A sidechain lines up where it ends, with the strip's signal at its device: as late
as the strip's input plus the devices before it (in a rack: the devices before the rack, then
those before the device in its chain). Whichever comes first waits: the sidechain is delayed
(`compensation`), or the strip's own signal is delayed just before the device (`deviceDelay`),
which makes everything after the device on that strip that much later (its automation too). A
device switched off isn't aligned (`RouteEdge::device` -1).

**Playing it.** The source writes its signal at the tap into the edge's buffer as it renders (a
tap after a device as `processChain()` passes it), and the renderer hands that to the device
before each `process()` call (`setSidechain()`); the scheduler runs the source first, since the
sidechain is a graph edge. A device with no sidechain edge is told so (`setSidechainConnected(false)`); one
whose edge solo leaves out this chunk gets no key signal (`setSidechain()` isn't called). The VST3
adapter then flags its aux bus as silent ([plugins.md](plugins.md)). Solo and mute across sidechains are in
[rendering.md](rendering.md#solo-and-mute).

The sidechain stays with the device when it moves (to another chain, track or rack); a move that
would make it a cycle is refused. When its source goes, it goes too.

## Racks

A rack (`addRack`) is a device in a chain whose own chains each process its input, side by side;
it puts out their sum, and an empty rack passes its input on. Its structure lives in the engine's
chains (`ChainModel` with `parentRack`, and `ProcessorEntry::chains`) and in the snapshot
(`RackRender` with its `ChainRender`s), not in the processor: `RackProcessor` (Rack.h) is only
its place in a chain, its on/off switch, its id, and the latency the last snapshot worked out for
it (for the UI). Its `process()` is never called; the renderer runs its chains
(`Renderer::processRack`).

- **Chains** (`addRackChain`, `removeRackChain`, `setRackChainOrder`): each has its devices and a
  fader (`TrackParams`: gain, pan, mute, solo among the rack's chains), metered like a track
  (`takeMeters()` reports them with `chainId`). Chain faders are automated as lanes of the rack:
  `chain:<chain id>:volume` and `chain:<chain id>:pan`.
- **Nesting**: racks nest at most `kMaxRackDepth` (8) deep. `addRack` checks the chain's depth
  (`chainDepthLocked()`); `moveProcessor` refuses a rack moving into one of its own chains, or one
  whose height (`rackHeightLocked()`) would take it too deep.
- **Moving**: `moveProcessor` takes a device (or a rack, with everything in it) to any chain on
  any strip, keeping its state. Moving to another strip resets what it holds of the old signal
  (`requestReset()`), its rack's devices too, and the rack's chains take the new strip id. Its
  automation stays with the strip it came from, and plays again if it comes back.
- **Removing** a rack removes its chains and everything in them (`removeChainContentsLocked()`).
- **Notes**: every chain's devices hear the strip's notes, so a rack of instruments layers them.

**Latency.** Delay compensation sees a strip's devices as a tree of `ChainSlot`s, depth first:
each device of the main chain and, after a rack, the devices of its chains, chain by chain. A
rack is a summing point inside the strip: its chains all hear its input, it hears them as late as
the slowest, and the others are delayed after their fader to line up with it (the same
`alignInputs()`). So a latent device in one chain doesn't smear the others, and the rack's latency
is compensated like any device's. A rack switched off (or one inside one) passes its input on, so
its chains don't count. A sidechain into a device in a chain lines up with the signal there, and
a delay before that device makes its chain later, and so maybe the rack.

```
strip input --+--> [dev A] --+--> rack R ----------------------------------+--> [dev D] --> fader
              |              |   chain 1: [dev B, 64]  -> fader -> delay 0  |
              |              |   chain 2: [dev C, 0]   -> fader -> delay 64 | (sum)
slots:        0              1   2 (B, rack 1, chain 0)  3 (C, rack 1, chain 1)  4 (D)
```

## Extending it

- **A new kind of edge**: add it to `routeEdgesLocked()` (with an `EdgeOrigin`), decide whether
  it `sums` and where it taps, build its `EdgeRender` in `rebuildSnapshotLocked()`, and teach
  `workOutSolo()` whether solo passes along it. Allocate its `EdgeState` (and delay line) with
  the model object that owns it, so it survives snapshots. Its creation must call
  `wouldCycle()`.
- **A new summing point** uses `alignInputs()`, as buses and racks do.
- Keep `Routing.h` free of engine types: it is index maths, and easy to test alone.

## Gotchas

- `RouteEdge` indices are `tracks_` indices; `EdgeRender` indices are snapshot (topological)
  indices. `EngineSnapshot.cpp` maps between them with `position`.
- A sidechain's `tap` in `RouteEdge` counts devices of the source's own chain only (a rack counts
  as one); a tap after a device inside a rack is before the fader.
- Changing a send from pre- to post-fader rebuilds the snapshot; changing only its level doesn't.
- Delay lines for an edge are reused only while long enough; a larger delay starts the line
  again from silence (a short gap in that edge's audio).

## Tests

- [test_groups_engine.py](../../tests/test_groups_engine.py): a group's effect on the sum of its
  tracks, cycles refused, removing a group, meters, solo and mute across levels, compensation in
  nested groups, group automation in time.
- [test_sends_engine.py](../../tests/test_sends_engine.py): sends before and after the fader,
  returns, muted tracks, cycles across sends and outputs, removing a return, solo and mute,
  compensation per edge (one track into two returns of different latency, a pre-fader tap after
  a latent device), send automation in time.
- [test_sidechain_engine.py](../../tests/test_sidechain_engine.py): taps, alignment both ways,
  a tap before a device waiting for its own sidechain, groups and the master, automation after a
  waiting device, cycles, the source going, mute and solo, silence flagged, workers.
- [test_racks_engine.py](../../tests/test_racks_engine.py): chains summing, faders, latency not
  smearing, nested automation, instrument racks, sidechains in racks, moves, nesting limits,
  cycles through racks, workers.
- [test_resampling_engine.py](../../tests/test_resampling_engine.py): input edges and their
  cycles.
- The model side of the same rules: `test_groups_model.py`, `test_sends_model.py`,
  `test_sidechain_model.py`, `test_racks_model.py`, `test_resampling_model.py`.
