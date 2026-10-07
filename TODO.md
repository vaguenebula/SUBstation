# TODO

Roadmap for routing, recording and device groups. Phases are in the order to
build them; each one should land with tests before the next starts.

Guiding idea: tracks, the master, group tracks and rack chains are all
**strips** — input → devices → delay compensation → volume/pan/mute/solo →
meter → output. They differ only in where their input comes from. Build that
once (`processStrip`) and reuse it everywhere.

Keep routing open for later: the engine knows **edges** (a strip's output
target), not hierarchy. Hierarchy (which track is in which group) lives in the
application layer's model. Sends, return tracks, resampling and sidechains are
more edges; Phase 6 lets a strip have several outgoing edges (until then each
has one), and Phases 7 and 8 add edge kinds before racks build on the routing.

---

## Phase 1 — Master as a strip, master effects

Engine
- [x] Factor the per-track body of `Renderer::renderChunk` into
      `processStrip(strip, ctx, L, R, frames, audible, flags)`
      (inserts → delay compensation → fader/meter).
- [x] Introduce a `StripRender` base shared by `TrackRender` and the master
      (`params`, `inserts`, `latency`, `compensation`, `delay`, `automation`,
      `volume`, `pan`); replace `snap.master` / `masterVolume` / `masterPan`.
- [x] Make the master a `TrackModel` with inserts (held in `master_`, not `tracks_`).
- [x] Master inserts in `buildAutomationLocked` (it passes `{}` today); lane
      latency = `maxLatency` + latency of the master devices before it.
- [x] Metronome delay = `maxLatency` + master insert latency (it's mixed after
      the master fader, so it would run early otherwise).
- [x] Offline render / export: account for master insert latency in the lag
      that `renderOfflineLocked` drops.

Model / UI
- [x] Master becomes a `Track` (`kind="master"`) in `project.master`, outside
      `project.tracks`; `project.track(MASTER)` returns it.
- [x] Remove `master_volume_db`, `master_pan`, `master_automation`,
      `master_automation_view` from `Project`; route through the track paths.
- [x] Serialization: migrate old projects (master fields → master track, no devices).
- [x] Device panel shows and edits the master's chain.

Tests
- [x] An effect on the master changes the export.
- [x] Metronome stays aligned with a latent plug-in on the master.
- [x] Master device automation plays in time.
- [x] Old project files load unchanged.

---

## Phase 2 — Audio input and recording

Engine
- [x] Track **input source**: none | device channels (mono/stereo pair) |
      *(later)* another track's output. Store it as a routing edge.
- [x] Input monitoring (off / in / auto): a monitored strip's input is the
      live input instead of clips, through `processStrip`.
- [x] Monitored strips skip delay compensation (monitoring latency); note this
      exception in the compensation function.
- [x] Lock-free ring buffer from the audio callback to a disk-writer thread;
      the audio thread never touches files.
- [x] Disk writer thread: WAV files in the project's recordings folder;
      handles overruns and reports them.
- [x] Arm/record API: arm per track, record start/stop with the transport,
      punch in/out from the loop range (later).
- [x] Recorded clip placement: shift by input latency + output latency +
      delay-compensation lag (`DeviceStatus` reports both driver latencies).
- [x] Device changes or sample-rate changes during recording stop it cleanly.

Model / UI
- [x] Track header: arm button, input selector, monitor mode.
- [x] Finished take → audio clip(s) via one undo command; overdub replaces
      what's under the take (as in Arrangement recording).
- [x] Record button and count-in in the transport bar.
- [x] Live waveform while recording (from the meter/ring buffer, not the file).

Tests
- [x] Loopback test (test ASIO driver feeds a known signal): the recorded clip
      lines up with the timeline, with and without latent plug-ins.
- [x] Undo removes a take and its file reference.
- [x] Monitoring through a latent plug-in isn't delayed by compensation.

---

## Phase 3 — MIDI input

Engine
- [x] MIDI device layer (WinMM first; Windows MIDI Services later), opened on
      the main thread like audio devices.
  - [ ] Windows MIDI Services backend.
- [x] Timestamp incoming events against the audio callback clock; hand them to
      the audio thread lock-free.
- [x] Feed live events into the armed / monitored track's event list (beside
      `buildNoteEvents` and preview notes); note-offs are tracked like
      `activeNotes_` so nothing hangs on stop or track removal.
- [x] MIDI input selection per track (all inputs / one device / channel).

Model / UI
- [x] MIDI preferences: list and enable devices.
- [x] Recording notes into a `MidiClip`, reusing Phase 2's take/undo flow.
- [x] Optional: record quantize.

Tests
- [x] Live notes reach the instrument with correct offsets inside a block.
- [x] Recorded notes land on the beats they were played (latency-adjusted).
- [x] Stopping while keys are held releases them.

---

## Phase 4 — Chain ids in the engine API

- [x] Every device chain gets an id (each track's / the master's main chain,
      later each rack chain).
- [x] `addBuiltinProcessor(chainId, …)`, `addPluginProcessor(chainId, …)`,
      `setChainOrder(chainId, ids)`, `moveProcessor(id, toChainId, index)`.
- [x] `processors_`: id → (chain, processor); chains: id → (owning strip, parent rack).
- [x] Update the engine bridge (`app/src/audio/BridgeDevices.cpp`); dragging a
      device between tracks becomes one `moveProcessor` (keeps plug-in state, no
      reload).

---

## Phase 5 — Group tracks (bus tree)

Model
- [x] `Track.parent: str | None`, `kind="group"`; `project.tracks` stays flat.
- [x] Invariant: a group's descendants follow it contiguously. Validate in
      every command that reorders or regroups.
- [x] `folded` as view state (saved, not undone).
- [x] Commands: group selected tracks (Ctrl+G), ungroup, move into / out of a
      group, delete group (with or without contents).

Engine
- [x] Strip `output` = bus id (0: master). The engine sees routing, not groups.
- [x] Routing is a general graph, not a tree: strips are nodes, and bus
      outputs (later sends, sidechains, resampling) are all edges. The snapshot
      topologically sorts it; `TrackRender.outputIndex` points at its
      destination's buffer. Each node carries its input count, so a future
      parallel scheduler can run any strip whose inputs are done (dependency
      counters, not "children before parent" levels).
- [x] No cycles: the model rejects an edge that would close one (a group fed by
      its own descendant, A↔B); the snapshot builder asserts the graph is acyclic.
- [x] Bus accumulation buffers (`kMaxBlock`) allocated edit-side, kept across
      snapshots in `TrackModel` (like `DelayLine`).
- [x] Delay compensation at every summing point, bottom-up:
      `arrival = inputLatency + ownInserts`, `inputLatency(bus) = max(arrival of its inputs)`,
      `compensation(child) = inputLatency(bus) − arrival(child)`. One function,
      reused by racks.
- [x] Automation latency per target = latency before that point on its path.
- [x] Solo: soloing a group solos its contents; soloing a track keeps its
      ancestor groups audible. Ancestor indices in the snapshot; audibility
      worked out once per chunk from the solo atomics.

UI
- [x] Track headers: group header with fold toggle, indentation, group colour band.
- [x] Arrangement: folded groups show a summary lane; drag tracks into/out of groups.
- [x] Group automation lanes and devices (free: groups are tracks).

Tests
- [x] Group effect processes the sum of its children.
- [x] Compensation: nested groups, unbalanced trees, latent plug-ins on a group.
- [x] Solo/mute combinations across levels.
- [x] Group/ungroup/undo round-trips; serialization.

---

## Phase 5½ — Parallel track processing

Before racks: Phase 9 adds scratch state (buffers per nesting depth), which
should be per worker from the start. Phase 5's graph (topological order,
`outputIndex`, `inputCount`) is what the scheduler runs. A strip's own chain
(and later a rack's chains) stays serial on one worker; parallel rack chains
come later.

Engine — untangle shared state first (useful and testable on one thread)
- [x] Per-track output buffers, allocated edit-side and kept in `TrackModel`
      (like `bus`), replacing the shared `trackLeft_` / `trackRight_`.
- [x] Buses pull instead of children pushing: `TrackRender.inputs` (indices);
      a bus sums its inputs' buffers in a fixed order when it starts, and the
      master sums its inputs in order after the graph. No two threads write one
      buffer, and the result doesn't depend on which thread finishes first.
- [x] `WorkerScratch` (warp buffers, `autoGain_`, `autoPan*_`),
      one per worker, allocated in `Renderer::prepare`. (Events went per
      track instead, into `TrackBuffers`: see the next item.)
- [x] MIDI in a serial prologue: build every track's events (active, live,
      preview and input notes, MIDI recording) before the graph, into per-track
      event buffers. Stretched clips get their voices there too (the voice
      pool is shared between tracks).
- [x] Split `renderChunk`: serial prologue (segments, solo, recording input,
      MIDI) → graph (`renderTrack(t, scratch)`) → serial epilogue (master sum,
      master strip, metronome).

Engine — scheduler
- [x] `Scheduler`: a fixed pool of workers plus the audio thread; workers join
      MMCSS ("Pro Audio") and set `ScopedNoDenormals` every chunk.
- [x] Per chunk: reset each node's counter to `inputCount`, queue the nodes
      with none; a finished node decrements its destination's counter and
      queues it at zero. Lock-free, no allocation; idle workers spin briefly,
      then wait on the run state (`atomic::wait`, i.e. WaitOnAddress).
- [x] Serial fallback below a threshold (few tracks, small blocks), where
      waking workers costs more than it saves.
- [x] Offline render / export use the same path.
- [x] Measure per-strip cost and start the most expensive path first: each
      track's render is timed (smoothed, ns per frame, in `TrackBuffers`); the
      prologue ranks every node by the cost from it to the master and queues
      the tracks without inputs by rank (`TaskGraph::orderRoots()`).
  - [ ] Maybe: an "urgent" slot so a bus that becomes ready mid-run on the
        critical path goes before the leaves still queued (only if a benchmark
        shows it matters).
  - [ ] Per-track CPU meters in the UI (`track_costs()` has the numbers).
  - [ ] Serial fallback from measured cost instead of counting busy tracks.
- [x] Wake-ups off the audio thread: each worker sleeps on a word of its own;
      a run wakes worker 1 only, and each worker joining wakes the next four
      (a tree), so the caller pays for one wake-up however many sleep (it paid
      for every one with `notify_all`: ~50 µs a run for 3 workers in a VM).
- [x] As many workers as the work keeps busy: a run takes `threads`, at most
      `parallelWork` and the costs' total over the longest path's, plus one
      (`TaskGraph::parallelism()`); the others stay asleep. A worker leaves a
      run once only one node is left (the last bus).
  - [ ] Measure the wake-up tree's fan-out (4) on a many-core machine: a
        `--live` run at 64 frames with 23 threads, 2 and 4 each.

Model / UI
- [x] Preferences: number of audio worker threads (default: cores − 1; 1 = off).

Tests
- [x] Benchmark first: N tracks of the built-in synth / a heavy plug-in,
      serial vs. parallel (`benchmarks/parallel_render_bench.cpp`).
- [x] Offline renders are bit-identical with and without workers: random
      graphs, nested groups, latent plug-ins, solo/mute.
- [x] Stress: many tracks, many runs (no thread sanitizer on MSVC).
- [x] A plug-in processed on different workers across blocks keeps its state.
- [x] Held, preview and recorded MIDI notes behave as before.

---

## Phase 6 — Return tracks and sends (the graph fans out)

Phases 5 and 5½ call routing a graph, but every part of it assumes one output
per track, so in practice it is a tree:
- `topologicalOrder`, `wouldCycle` and `alignGraph` (Routing.h) take a single
  `outputs[i]`; `wouldCycle` only walks that one chain;
- delay compensation (and its `DelayLine`) is per track: "delay this track to
  line up where it goes";
- `TaskGraph` keeps one output per node and ranks by `cost + rank[output]`;
- solo walks one `ancestors` chain.

Sends, resampling and sidechains each give a track a second output, so all of
this changes once, whichever comes first. Sends are the simplest (an edge with
a gain, no plug-in involved), so they make the change; Phases 7 and 8 then add
edge kinds. Before racks: the routing and scheduler code is fresh, the
bit-identical tests are a ready safety net, and racks barely touch the edges
between strips.

Engine — edges
- [x] `EdgeRender { from, to, kind, tap, compensation, delay, state, level }`:
      kind = output | send (later: input, sidechain); tap = post-fader
      (default) or pre-fader; the gain is in `EdgeState` (an atomic, kept
      across snapshots, like `TrackParams`). A strip has one output edge and
      any number of sends. The snapshot keeps the edges (`snap.edges`) and each
      node's outgoing and incoming ones; `TrackRender.inputs` became `incoming`.
- [x] Routing.h on edge lists (`RouteEdge`): topological order over all edges;
      the cycle check searches every path from the new edge's destination back
      to its source; `alignGraph` aligns each summing point over its incoming
      edges (per edge `tapLatency`, ready for after-device taps).
- [x] Delay compensation per edge, not per track:
      `arrival(edge) = inputLatency(from) + latency of from's devices before the tap`;
      a summing point delays each incoming edge to the latest arrival. One
      `DelayLine` per edge (edit-side, kept across snapshots). A track that sends
      to two returns with different latencies is delayed differently on each edge.
      The delay now comes after the source's fader (it was before it), so fader
      automation is as late as the strip's own arrival.
- [x] Edge buffers: a pre-fader tap needs the signal before the fader, so the
      source writes it to the edge's own buffer; so does an edge delayed on its
      own (compensation > 0); other post-fader edges read the source's track
      buffer. The destination applies the send gain while it sums its inputs,
      in a fixed order (as buses do now), so results stay independent of threads.
- [x] Return tracks: no clips, fed only by sends, output to the master. A return
      may send to another return; cycles refused. (To the engine they're just
      tracks; it refuses a send to the master.)
- [x] Send level is a mixer target like volume and pan (automation lane
      `processorId 0, param "send:<return's engine id>"`). The gain is applied
      where the edge is summed, after its delay, so its lane latency is the
      edge's arrival plus its compensation: the destination's input latency.
- [x] Mute: a muted track's sends go silent too (pre-fader ones as well: they
      take the fader's mute ramp).
- [x] Solo across sends: an edge plays if its source is downstream of a solo
      (soloed or fed by something soloed: solo in place, the returns it sends
      to included) or its destination upstream of one (soloed or feeding
      something soloed: what sends to a soloed return keeps sending, but not
      into the master). A track none of whose edges play is silenced at its
      fader. Worked out once per chunk (`Renderer::workOutSolo`); live, an
      edge's level and solo ramp like a fader (`EdgeState`).
- [x] Scheduler: `TaskGraph` with outgoing-edge lists (flat arrays); a
      finished node counts down every destination; `inputCount` counts incoming
      edges; rank = cost + the highest rank among its destinations.

Model / UI
- [x] Return tracks in `project.returns` (beside `project.tracks` and the
      master); `Track.sends: {return id: Send(level_db, pre_fader)}`.
      Serialization (version 9; older projects simply have none; sends to a
      missing return or closing a cycle are dropped on load).
- [x] Create / delete return tracks (Ctrl+Alt+T); deleting one removes the
      sends to it (and their automation), as one undo command.
- [x] Send knobs on track headers (groups and returns too: they're tracks);
      pre/post toggle (the knob's right-click menu); send automation lanes
      (*Mixer › Send A*). Returns show as compact rows pinned above the master
      (headers and lanes like the master's), not among the arrangement's rows.
  - [ ] Maybe: returns in the scrolling lanes after the tracks, resizable, as
        in Ableton (the lanes canvas indexes rows by `project.tracks`: clip
        drags, time selections and drops would need to skip them).
  - [ ] A mixer view (sends of every track side by side).

Tests
- [x] Post- and pre-fader sends at their levels; a return's effect processes
      the sum of what is sent to it.
- [x] Compensation: a latent plug-in on a return; a latent track sending; one
      track sending to two returns of different latency; a pre-fader tap after
      a latent device. Clicks line up at the master.
- [x] Cycles refused across sends, outputs and returns.
- [x] Solo/mute across sends.
- [x] Random graphs with sends (fan-out, pre/post taps): bit-identical with and
      without workers (`addReturns` beside `randomProject` in
      tests/engine/test_parallel_engine.cpp).
- [x] Undo and serialization round-trips.

---

## Phase 7 — Resampling (track input from another track)

Small once edges exist: `InputEdge` already has room for another source.

Engine
- [x] `InputEdge::Source::Track`: an incoming edge from another strip (tap
      post-fader; `EdgeRender::Kind::Input`). The source renders first; a track
      can't take input from something it feeds (the Phase 6 cycle check). The
      edge isn't summed or aligned (`RouteEdge::sums`), and solo crosses it only
      while its track monitors it.
- [x] Monitoring as with device input: a monitored track hears its source
      instead of its clips, not delay-compensated (`compensationFor`).
- [x] Recording: the renderer hands the source's buffer to the take, like
      device input (its ring, its disk writer), in the epilogue. Placement: no
      device latencies; the take lands where the source was heard (moved back
      by the edge's arrival, not by input + output latency): each take has its
      own placement.
- [x] The master as a source ("Resampling", `InputEdge::Source::Master`): the
      master renders after the graph, so it is recorded in the epilogue (after
      its fader, before the metronome); a track resampling the master can't
      monitor it (that would feed back).

Model / UI
- [x] `Track.input_track` (a track, group or return id, or MASTER), project
      version 10; inputs that would close a cycle (moving a track into its
      source's group) or whose source goes are dropped in the same undo step.
- [x] Input selector lists tracks, groups, returns and "Resampling" (the
      master); sources that would make a cycle are greyed out.
- [x] Recording reuses Phase 2's take/undo flow.

Tests
- [x] A take resampled from a track equals that track's render sample for
      sample, with latent plug-ins on the source (and on the master).
- [x] Cycles refused; monitoring a resampled source isn't delayed.
- [x] Undo removes the take.

---

## Phase 8 — Sidechain inputs

An edge that ends at a device instead of at a strip's input.

Engine
- [x] `kind = sidechain` edges into a processor (`EdgeRender::Kind::Sidechain`:
      `to` = the strip, `device` = the insert). The engine keeps a sidechain with
      its processor id (`setProcessorSidechain`), so it survives moves between
      chains and, later, racks; `moveProcessor` refuses a move that would make it
      a cycle. It isn't summed (`RouteEdge::sums`), but orders the graph and
      closes cycles like any edge; one into the master's devices never can.
- [x] Tap point per edge: post-fader, pre-fader, after a given device or
      before them all (`Tap::AfterDevice`: `processInserts` copies the signal
      after that insert, or before the first (`SidechainTap::PreFx`), into the
      edge's buffer; while the device isn't on the source, before the fader). A
      tap leaves as late as the devices before it, not counting a delay before
      the next one (which waits for its own sidechain). The consumer waits for
      the whole source strip.
  - [ ] Maybe: splitting a strip at the tap, so the consumer can start earlier
        (only if a benchmark shows it matters).
- [x] Each edge has its own buffer (Phase 6), but an undelayed post-fader tap
      (it reads the source's): the source writes it, the consumer's device reads
      it in the same chunk, on whichever thread (the scheduler runs the source first).
- [x] Delay compensation per edge: `alignGraph` walks each strip's chain and
      lines every sidechain up with the signal at its device (`alignInputs`, the
      Phase 5 function, over the two). The earlier one is delayed: the
      sidechain, or the consumer's own signal just before the device
      (`EdgeRender::deviceDelay`), which makes everything after it on that strip
      later (`GraphLatencies::deviceLatency`: its automation, its edges). Not
      while the consumer is monitored (monitoring latency).
- [x] Mute and solo: a sidechain isn't heard on its own. Solo goes up it
      (while the consumer is heard: soloed, fed by a solo or feeding one, the
      source keeps keying it) but not down (soloing the
      source doesn't make the consumer heard); into the master's devices it always
      plays. Mute and solo silence it only after the source's fader (a muted
      kick still keys from before it).
- [x] VST3: the first aux input bus is the sidechain, arranged (stereo if the
      plug-in takes it) and activated edit-side with the other buses, never in
      the callback; the adapter fills it from the edge's buffer (silence flags
      set when there is no source, or solo leaves it out). `Processor::hasSidechain()`,
      `setSidechain()`: a built-in device could have one too.
- [x] Expect less parallelism: a sidechain serialises source → consumer, so
      "kick ducks everything" turns into kick, then the rest. Cost ordering
      (Phase 5½) already ranks the source by what waits on it.

Model / UI
- [x] `Device.sidechain: Sidechain(track id, tap) | None` (tap: post-fader,
      pre-fader, pre-FX or a device id; pre-FX on a MIDI track is after its
      instrument), offered only for devices with an aux input
      (`ProcessorInfo.has_sidechain`); serialized (project version 11; one from a
      missing track, the master, or closing a cycle is dropped on load). A deleted
      source track turns it off (in the same undo step), as do ungrouping a group
      it comes from and moving tracks into groups (or the device to a track)
      where it would close a cycle. Sidechains are edges of the model's routing
      graph, so sends and inputs that would close a cycle with one are refused.
- [x] Source picker in the device title bar (the sidechain button, lit while
      set): the tracks, groups and returns, those that would make a cycle (e.g.
      the group the device's track is in) greyed out; then where it is taken,
      along the signal as in Ableton: Pre FX, after each device, Post FX, Post Mixer.
  - [ ] Maybe: a built-in compressor (or a gate) with a sidechain, as Ableton's.

Tests
- [x] A test plug-in with an aux input (SUB Test Sidechain: its input plus its
      sidechain) hears the source sample-exactly, with latent plug-ins before
      and after the tap and on the consumer's track before and after the device;
      into groups' and the master's devices; live with workers too.
- [x] Cycles refused; deleting the source; undo and serialization.
- [x] Random graphs with sidechains: bit-identical with and without workers.

---

## Phase 9 — Device groups (racks)

Engine
- [x] Rack structure lives in the snapshot (not a self-mutating composite
      `Processor`): a rack is a `RackProcessor` (its place, switch and id) in a
      chain, `StripRender.racks` per insert, and `RackRender { chains }` with
      `ChainRender : StripRender { id, params (TrackParams), inserts, compensation, delay, automation, volume, pan }`.
- [x] Renderer: rack copies its input to each chain, runs its devices
      (`processChain`, the same code as a strip's), its fader, delay; sums. A
      strip's devices now process the whole chunk one after another (each in the
      chunk's stretches), so a rack runs its chains over the chunk.
- [x] Scratch buffers per nesting depth, preallocated in `Renderer::prepare`
      (per worker: `WorkerScratch::racks`); depth capped at `kMaxRackDepth` (8)
      in the API (`addRack`, `moveProcessor`).
- [x] Rack latency = slowest chain; other chains compensated internally (the
      Phase 5 function, in `alignGraph`'s walk of each strip's device tree: `ChainSlot`).
- [x] Automation: lanes built per chain walking the tree (`buildChainLocked`);
      nested devices keep `automate()` direct calls; lane latency = path latency.
      Chain faders are lanes of their rack (`"chain:<id>:volume"`, `":pan"`).
- [x] MIDI: chains get the track's events (instrument racks / layering).
- [x] Chain meters (`MeterReading.chainId`).
- [x] Sidechains (Phase 8) into a device inside a rack: its latency before the
      device includes the rack's chain up to it (and a delay before it makes its chain, and so the rack, later).
  - [x] Taps after a device inside a rack: `RouteEdge::tap` counts the source's slots
        (depth first), and the chain the device is in writes the tap
        (`ChainRender::deviceTaps`), before its fader; in a rack switched off, after
        that rack. The sidechain menu lists them ("After Rack › Chain › Device").
  - [ ] Maybe: a rack's chains on several workers (only if a benchmark shows it matters).

Model / UI
- [x] `Device(kind="rack", chains=[Chain(id, name, devices, volume_db, pan, mute, solo)], macros=(MacroMapping...))`.
- [x] Recursive device lookup (`device_path()`, `find_device()`, `project.device()`); serialization recurses (version 12).
- [x] Device panel: rack with chain list, chain mixer (and meters), nested device view (the chain clicked, beside the rack).
  - [x] The chain list hidden until shown, the chain's devices hidden when asked
        (buttons at the rack's left, as Ableton's; view state, saved).
- [x] Group selected devices into a rack (Ctrl+G in the device panel), ungroup (Ctrl+Shift+G).
- [x] Macros (the application layer first): rack parameters mapped to (device, param, range) targets.
  - [x] 4 by default, 1 to 16 (+ and − at the rack's left), named by the user
        (project version 18: `macro_names`).
  - [x] Automated: lanes of the rack; the bridge plays each mapped parameter along
        its macro's envelope over its range (the engine has no macros).
- [x] Rack presets: the save button saves a rack (chains, nested devices,
      plug-in state, macros) as a `.gilpreset`; right-click beside the devices to load one.
  - [x] The browser's Presets section (see "Device presets" below) lists them too.
  - [x] Macros' ranges edited in the UI (a macro's Edit Mappings…: Min, Max, Invert, Unmap).

Tests
- [x] Parallel chains sum correctly; chain mute/solo.
- [x] Latent device in one chain doesn't smear the others.
- [x] Nested-device automation in time.
- [x] Instrument rack layers two synths.
- [x] Saved rack round-trips: save → load renders the same as the original,
      including plug-in state and macro mappings; loading twice gives distinct ids.

---

## Device presets

Plug-in presets don't depend on racks and can be built any time; rack presets
plug into the same flow once Phase 9 lands.

Model / UI
- [x] Small save button in every device's title bar (plug-ins, built-ins,
      racks). Asks for a name and writes a preset file to the user library
      (`Documents\SUBstation\Presets\<device name>\<name>.gilpreset`;
      `app/src/io/Presets.cpp`), asking before replacing one.
- [x] Preset file = the `Device` subtree via the project serializer: device
      kind/plug-in id, parameter values and the plug-in's state chunk.
- [x] Browser "Presets" section, grouped by device (plug-in name / built-in /
      racks). Drag onto a track's chain or double-click to insert a new device;
      drop onto an existing device of the same kind to load the state into it
      (`ProjectEditor.load_preset_into`; the device is outlined while the drag
      is over it). Presets are searched in "All" too; dropped on a track in the
      arrangement they go on it, and an instrument preset makes a MIDI track.
- [x] Loading is one undo command; inserted devices get fresh ids.
- [x] Missing plug-in on load: keep the device, mark it missing (as project
      loading does); inside a rack, don't fail the whole rack. (A built-in
      device this version doesn't have is missing the same way.)
- [x] Rename / delete / show-in-folder from the browser's context menu
      (delete: to the recycle bin). The library is watched, so changes made in
      Explorer show up.
- [x] Racks are titled with the name of the preset they were saved as or loaded
      from (`Device.name`, project version 13).
  - [ ] Maybe: rename a rack by hand (double-click its title), and plug-ins and
        built-in devices titled with their presets' names too.
- [x] Default presets: right-click a device › Save as Default Preset; new devices
      of that kind (that plug-in) start as it (`Defaults` folder of the library;
      Clear Default Preset).
  - [ ] Maybe: a preset of the device under the mouse while browsing (hot-swap,
        as in Ableton).

Tests
- [x] Plug-in preset round-trips: save → load restores parameters and state
      (renders the same).
- [x] Loading a preset onto an existing device is undoable.
- [x] Preset for a missing plug-in loads as a missing device.

---

## Later

- [x] Freeze / flatten (Ctrl+Shift+F; *Edit › Flatten Track*). An offline
      render of one track's signal before its fader (`render_track_to_wav`),
      from beat 0 to the arrangement's end plus its tail:
  - [x] a frozen track plays its rendered audio; its plug-ins are unloaded, their
    state kept for unfreezing;
  - [x] sends and sidechain sources tap the frozen audio: the render is before the
    fader, so post- and pre-fader taps both work; Pre FX and after-device taps
    keep the track from freezing;
  - [x] a frozen group renders its bus, and the engine stops rendering what goes only
    into it; what is in it is locked; frozen device automation is baked, volume
    and pan stay live;
  - [x] a tempo change plays the frozen audio warped;
  - [x] flatten = keep the rendered clip, drop the devices: one undo command.
  - [ ] Maybe: render only from the track's first sound (smaller files).
  - [ ] Maybe: re-render frozen tracks after a tempo change, instead of warping.
- [ ] Macro automation in the engine (a lane fanning out to its targets).
- [ ] Key/velocity zones on rack chains.

---

## Intelligence (`intelligence/`, docs/intelligence.md)

Sound similarity
- [x] Fingerprints (timbre, its attack/body/tail, spectrum, envelope, pitch,
      rhythm), compared aspect by aspect; weights tuned on a real library
      (`sound_similarity_bench`).
- [x] The browser's files analysed in the background, saved to
      sound-index.bin; only new or changed files analysed again.
- [x] Find Similar Sounds: an audio file's menu in the browser, an audio
      clip's (its part of the file) in the arrangement; the Similarity sort.
- [ ] Swap in similar samples in the sampler (step through
      `SimilarSounds::best()`).
- [ ] The same in the drum rack (once there is one), per pad.
- [ ] A file manager view: swap a project's audio files (by track) for similar ones.
- [ ] Maybe: the user weighs the aspects (as Sononym does).
- [ ] Maybe: a small learned embedding as another aspect (better across kinds).

Later
- [ ] MIDI generation.
- [ ] MIDI humanisation (an XGBoost model, its C++ library).
- [ ] Chord recognition over a whole project.
- [ ] An MCP server for agents.
