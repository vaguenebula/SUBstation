# TODO

Roadmap for routing, recording and device groups. Phases are in the order to
build them; each one should land with tests before the next starts.

Guiding idea: tracks, the master, group tracks and rack chains are all
**strips** — input → devices → delay compensation → volume/pan/mute/solo →
meter → output. They differ only in where their input comes from. Build that
once (`processStrip`) and reuse it everywhere.

Keep routing open for later: the engine knows **edges** (a strip's output
target), not hierarchy. Hierarchy (which track is in which group) lives in the
Python model. That keeps sends, return tracks, resampling and sidechains
possible without a redesign.

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
- [ ] MIDI device layer (WinMM first; Windows MIDI Services later), opened on
      the main thread like audio devices.
- [ ] Timestamp incoming events against the audio callback clock; hand them to
      the audio thread lock-free.
- [ ] Feed live events into the armed / monitored track's event list (beside
      `buildNoteEvents` and preview notes); note-offs are tracked like
      `activeNotes_` so nothing hangs on stop or track removal.
- [ ] MIDI input selection per track (all inputs / one device / channel).

Model / UI
- [ ] MIDI preferences: list and enable devices.
- [ ] Recording notes into a `MidiClip`, reusing Phase 2's take/undo flow.
- [ ] Optional: record quantize.

Tests
- [ ] Live notes reach the instrument with correct offsets inside a block.
- [ ] Recorded notes land on the beats they were played (latency-adjusted).
- [ ] Stopping while keys are held releases them.

---

## Phase 4 — Chain ids in the engine API

- [ ] Every device chain gets an id (each track's / the master's main chain,
      later each rack chain).
- [ ] `addBuiltinProcessor(chainId, …)`, `addPluginProcessor(chainId, …)`,
      `setChainOrder(chainId, ids)`, `moveProcessor(id, toChainId, index)`.
- [ ] `processors_`: id → (chain, processor); chains: id → (owning strip, parent rack).
- [ ] Update bindings and `engine_bridge.py`; dragging a device between tracks
      becomes one `moveProcessor` (keeps plug-in state, no reload).

---

## Phase 5 — Group tracks (bus tree)

Model
- [ ] `Track.parent: str | None`, `kind="group"`; `project.tracks` stays flat.
- [ ] Invariant: a group's descendants follow it contiguously. Validate in
      every command that reorders or regroups.
- [ ] `folded` as view state (saved, not undone).
- [ ] Commands: group selected tracks (Ctrl+G), ungroup, move into / out of a
      group, delete group (with or without contents).

Engine
- [ ] Strip `output` = bus id (0: master). The engine sees routing, not groups.
- [ ] Routing is a general graph, not a tree: strips are nodes, and bus
      outputs (later sends, sidechains, resampling) are all edges. The snapshot
      topologically sorts it; `TrackRender.outputIndex` points at its
      destination's buffer. Each node carries its input count, so a future
      parallel scheduler can run any strip whose inputs are done (dependency
      counters, not "children before parent" levels).
- [ ] No cycles: the model rejects an edge that would close one (a group fed by
      its own descendant, A↔B); the snapshot builder asserts the graph is acyclic.
- [ ] Bus accumulation buffers (`kMaxBlock`) allocated edit-side, kept across
      snapshots in `TrackModel` (like `DelayLine`).
- [ ] Delay compensation at every summing point, bottom-up:
      `arrival = inputLatency + ownInserts`, `inputLatency(bus) = max(arrival of its inputs)`,
      `compensation(child) = inputLatency(bus) − arrival(child)`. One function,
      reused by racks.
- [ ] Automation latency per target = latency before that point on its path.
- [ ] Solo: soloing a group solos its contents; soloing a track keeps its
      ancestor groups audible. Ancestor indices in the snapshot; audibility
      worked out once per chunk from the solo atomics.

UI
- [ ] Track headers: group header with fold toggle, indentation, group colour band.
- [ ] Arrangement: folded groups show a summary lane; drag tracks into/out of groups.
- [ ] Group automation lanes and devices (free: groups are tracks).

Tests
- [ ] Group effect processes the sum of its children.
- [ ] Compensation: nested groups, unbalanced trees, latent plug-ins on a group.
- [ ] Solo/mute combinations across levels.
- [ ] Group/ungroup/undo round-trips; serialization.

---

## Phase 6 — Device groups (racks)

Engine
- [ ] Rack structure lives in the snapshot (not a self-mutating composite
      `Processor`): `InsertNode { processor | chains }`,
      `ChainRender { id, params (TrackParams), inserts, compensation, delay, automation, volume, pan }`.
- [ ] Renderer: rack copies its input to each chain, `processStrip`s it, sums.
- [ ] Scratch buffers per nesting depth, preallocated in `Renderer::prepare`;
      cap the depth (e.g. 8) in the API.
- [ ] Rack latency = slowest chain; other chains compensated internally (the
      Phase 5 function).
- [ ] Automation: walk the tree when building lanes; nested devices keep
      `automate()` direct calls; lane latency = path latency.
- [ ] MIDI: chains get the track's events (instrument racks / layering).
- [ ] Chain meters (`MeterReading` by chain id).

Model / UI
- [ ] `Device(kind="rack", chains=[Chain(id, name, devices, volume_db, pan, mute, solo)])`.
- [ ] Recursive device lookup (`device_path()`); serialization recurses.
- [ ] Device panel: rack with chain list, chain mixer, nested device view.
- [ ] Group selected devices into a rack (Ctrl+G in the device panel), ungroup.
- [ ] Macros (Python first): rack parameters mapped to (device, param, range) targets.
- [ ] Rack presets: the save button / browser flow from "Device presets" below
      also covers racks (chains, nested devices, plug-in state, macros).

Tests
- [ ] Parallel chains sum correctly; chain mute/solo.
- [ ] Latent device in one chain doesn't smear the others.
- [ ] Nested-device automation in time.
- [ ] Instrument rack layers two synths.
- [ ] Saved rack round-trips: save → load renders the same as the original,
      including plug-in state and macro mappings; loading twice gives distinct ids.

---

## Device presets

Plug-in presets don't depend on racks and can be built any time; rack presets
plug into the same flow once Phase 6 lands.

Model / UI
- [ ] Small save button in every device's title bar (plug-ins, built-ins,
      racks). Asks for a name and writes a preset file to the user library.
- [ ] Preset file = the `Device` subtree via the project serializer: device
      kind/plug-in id, parameter values and the plug-in's state chunk.
- [ ] Browser "Presets" section, grouped by device (plug-in name / built-in /
      racks). Drag onto a track's chain or double-click to insert a new device;
      drop onto an existing device of the same kind to load the state into it.
- [ ] Loading is one undo command; inserted devices get fresh ids.
- [ ] Missing plug-in on load: keep the device, mark it missing (as project
      loading does); inside a rack, don't fail the whole rack.
- [ ] Rename / delete / show-in-folder from the browser's context menu.

Tests
- [ ] Plug-in preset round-trips: save → load restores parameters and state
      (renders the same).
- [ ] Loading a preset onto an existing device is undoable.
- [ ] Preset for a missing plug-in loads as a missing device.

---

## Later

- [ ] Return tracks and sends (a send = second edge with a gain; compensation
      per edge).
- [ ] Track input from another track (resampling) — an input edge.
- [ ] Sidechain inputs for plug-ins — an edge into a processor.
  - Tap point per edge: pre-fader, post-fader or after a given device. First
    version: the consumer waits for the whole source strip; splitting a strip
    at the tap (so it can start earlier) is a later optimisation.
  - Each edge has its own buffer, allocated edit-side like bus buffers (it must
    stay valid until the consumer reads it, possibly on another thread).
  - Delay compensation per edge: align the source's arrival with the consumer's
    latency before that device (the Phase 5 function).
  - Source picker greys out sources that would make a cycle (e.g. a track
    inside the group being sidechained).
  - VST3: activate and arrange the aux input bus edit-side
    (`activateBus`, `setBusArrangements`), never in the callback.
  - Expect less parallelism: a sidechain serialises source → consumer, so
    "kick ducks everything" turns into kick, then the rest.
- [ ] Macro automation in the engine (a lane fanning out to its targets).
- [ ] Key/velocity zones on rack chains.
- [ ] Freeze / flatten.
