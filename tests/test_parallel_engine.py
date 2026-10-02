"""Parallel track processing: the tracks of a chunk render on several threads
(the rendering thread and the scheduler's workers), each after what goes into
it. Whatever the threads, the result is the same bit for bit: random routing
graphs with nested groups, latent plug-ins, solo, mute and automation render
identically on one thread and on several. Rendered offline, so no audio device
is needed (test_parallel_live.py plays live, with the fake ASIO driver)."""

import os

import numpy as np
import pytest

from gilstudio import _engine as ge

from .conftest import SAMPLE_RATE, TEST_PLUGINS

SPB = SAMPLE_RATE // 2  # samples per beat at 120 BPM
PLUGINS = str(TEST_PLUGINS)
FX_GAIN, FX_LATENCY = 0, 1  # GIL Test Effect's parameters
# Workers run whatever the computer's cores (oversubscribed on fewer).
THREADS = 4

needs_plugins = pytest.mark.skipif(not TEST_PLUGINS.exists(), reason="test plug-ins not built")


@pytest.fixture
def engine():
    e = ge.Engine()
    yield e
    e.close_device()


@pytest.fixture(scope="module")
def uids():
    return {d.name: d.uid for d in ge.scan_vst3(PLUGINS)} if TEST_PLUGINS.exists() else {}


def render(engine, threads, frames, start_beat=0.0, **options):
    engine.audio_threads = threads
    assert engine.audio_threads == threads
    return engine.render_offline(start_beat, frames, **options)


def settle(engine):
    """A render to let the parameters set since the last one arrive: a Utility
    ramps to a new gain in its first block (in offline renders too)."""
    engine.render_offline(0.0, 4096)


def assert_same_on_any_threads(engine, frames, start_beat=0.0, **options):
    """Renders on one thread and on several: the same samples, and the workers rendered some tracks."""
    settle(engine)
    serial = render(engine, 1, frames, start_beat, **options)
    parallel = render(engine, THREADS, frames, start_beat, **options)
    assert engine.nodes_on_workers > 0, "the workers rendered nothing"
    assert np.abs(serial).max() > 0.0
    np.testing.assert_array_equal(parallel, serial)
    return serial


def synth(engine, track, notes, wave=0):
    processor = engine.add_builtin_processor(engine.track_chain(track), "synth")
    engine.set_processor_param(processor, 0, wave)
    engine.set_track_notes(track, [ge.NoteDesc(*note) for note in notes])
    return processor


def utility(engine, track, gain_db):
    processor = engine.add_builtin_processor(engine.track_chain(track), "utility")
    engine.set_processor_param(processor, engine.processor_param_index(processor, "gain"), gain_db)
    return processor


def latent_effect(engine, uids, track, latency, gain=1.0):
    effect = engine.add_plugin_processor(engine.track_chain(track), "VST3", PLUGINS, uids["GIL Test Effect"])
    engine.set_processor_param(effect, FX_LATENCY, latency)
    engine.set_processor_param(effect, FX_GAIN, gain)
    engine.idle()  # the plug-in asked for a restart to change its latency
    return effect


def random_notes(rng, beats=8.0, count=12):
    notes = []
    for _ in range(count):
        start = round(float(rng.uniform(0.0, beats - 0.25)), 3)
        notes.append((start, round(float(rng.uniform(0.05, 2.0)), 3), int(rng.integers(36, 96)),
                      int(rng.integers(20, 128))))
    return notes


def random_project(engine, rng, make_wav, uids, tracks=12, beats=8.0):
    """Clip, synth and group tracks routed at random (groups in groups, tracks
    going into groups made after them), with devices (latent ones too), gain,
    pan, solo, mute and automation."""
    noise = [make_wav(rng.uniform(-0.5, 0.5, (SAMPLE_RATE, 2))) for _ in range(3)]
    for path in noise:
        engine.load_source(path)
    groups, leaves = [], []
    for _ in range(tracks):
        track = engine.add_track()
        kind = rng.choice(["clip", "synth", "group"], p=[0.4, 0.35, 0.25])
        if kind == "group":
            groups.append(track)
        else:
            leaves.append(track)
        if kind == "clip":
            engine.set_track_clips(track, [
                ge.ClipDesc(str(rng.choice(noise)), round(float(rng.uniform(0.0, beats - 1.0)), 3), 1.0)
                for _ in range(int(rng.integers(1, 4)))])
        elif kind == "synth":
            synth(engine, track, random_notes(rng, beats), wave=int(rng.integers(0, 4)))
        if rng.random() < 0.5:
            utility(engine, track, float(rng.uniform(-12.0, 6.0)))
        if uids and rng.random() < 0.4:
            latent_effect(engine, uids, track, int(rng.integers(0, 600)), float(rng.uniform(0.25, 1.0)))
        engine.set_track_gain(track, float(rng.uniform(0.3, 1.0)))
        engine.set_track_pan(track, float(rng.uniform(-1.0, 1.0)))
        engine.set_track_mute(track, bool(rng.random() < 0.1))
        engine.set_track_solo(track, bool(rng.random() < 0.1))
        if rng.random() < 0.25:
            points = sorted(rng.uniform(0.0, beats, 4))
            engine.set_track_automation(track, [ge.AutomationLane(0, "volume", [
                ge.AutomationPoint(float(p), float(rng.uniform(0.2, 1.0))) for p in points])])
        if groups and rng.random() < 0.7:  # into a group made before it (groups nest)
            parent = int(rng.choice(groups))
            if parent != track:
                engine.set_track_output(track, parent)
    for track in leaves:  # some into groups made after them: the snapshot reorders them
        if groups and rng.random() < 0.3:
            engine.set_track_output(track, int(rng.choice(groups)))
    if uids and rng.random() < 0.5:
        latent_effect(engine, uids, ge.MASTER, int(rng.integers(0, 300)))
    return groups, leaves


def add_returns(engine, rng, uids, tracks, count=3, beats=8.0):
    """Return tracks (with devices, latent ones too), and sends into them at
    random: from tracks and groups, from returns into later ones, after the
    fader or before it, at random levels, some automated, a few soloed or muted."""
    returns = [engine.add_track() for _ in range(count)]
    for ret in returns:
        utility(engine, ret, float(rng.uniform(-6.0, 0.0)))
        if uids and rng.random() < 0.6:
            latent_effect(engine, uids, ret, int(rng.integers(0, 500)), float(rng.uniform(0.25, 1.0)))
        engine.set_track_gain(ret, float(rng.uniform(0.3, 1.0)))
        engine.set_track_mute(ret, bool(rng.random() < 0.1))
        engine.set_track_solo(ret, bool(rng.random() < 0.15))
    for i, ret in enumerate(returns):  # into later returns only: no cycles
        for later in returns[i + 1:]:
            if rng.random() < 0.4:
                engine.set_track_send(ret, later, float(rng.uniform(0.1, 1.0)), bool(rng.random() < 0.5))
    for track in tracks:
        for ret in returns:
            if rng.random() < 0.5:
                engine.set_track_send(track, ret, float(rng.uniform(0.1, 1.0)), bool(rng.random() < 0.4))
                if rng.random() < 0.2:
                    points = sorted(rng.uniform(0.0, beats, 3))
                    engine.set_track_automation(track, [ge.AutomationLane(0, f"send:{ret}", [
                        ge.AutomationPoint(float(p), float(rng.uniform(0.0, 1.0))) for p in points])])
    return returns


def test_the_number_of_audio_threads(engine):
    default = ge.Engine.default_audio_threads()
    assert default == max(1, min(64, (os.cpu_count() or 1) - 1))
    assert engine.audio_threads == default
    engine.audio_threads = 3
    assert engine.audio_threads == 3
    engine.audio_threads = 0  # at least the audio thread
    assert engine.audio_threads == 1
    engine.audio_threads = 1000
    assert engine.audio_threads == 64


def test_a_track_renders_the_same_on_any_thread(engine):
    """Built-in synths (with state: their voices go on across blocks) on many
    tracks, each moving between threads from block to block."""
    rng = np.random.default_rng(1)
    for _ in range(16):
        track = engine.add_track()
        synth(engine, track, random_notes(rng, 16.0, 24), wave=int(rng.integers(0, 4)))
    assert_same_on_any_threads(engine, 16 * SPB)


@needs_plugins
def test_a_plugin_keeps_its_state_on_any_thread(engine, uids, make_wav):
    """A plug-in's delay line (its latency) carries audio from block to block, on
    whichever worker it ran: the render is the same as on one thread."""
    rng = np.random.default_rng(2)
    for _ in range(8):
        track = engine.add_track()
        path = make_wav(rng.uniform(-0.5, 0.5, (SAMPLE_RATE, 2)))
        engine.load_source(path)
        engine.set_track_clips(track, [ge.ClipDesc(path, 0.0, 1.0)])
        latent_effect(engine, uids, track, int(rng.integers(1, 2000)))
    assert_same_on_any_threads(engine, 4 * SPB)


@pytest.mark.parametrize("seed", range(12))
def test_random_graphs_render_the_same_on_any_threads(engine, make_wav, uids, seed):
    rng = np.random.default_rng(seed)
    random_project(engine, rng, make_wav, uids, tracks=int(rng.integers(4, 20)))
    settle(engine)
    serial = render(engine, 1, 8 * SPB)
    parallel = render(engine, THREADS, 8 * SPB)
    np.testing.assert_array_equal(parallel, serial)
    # Looping and the metronome too, from another position.
    engine.set_loop(True, 2.0, 5.0)
    serial = render(engine, 1, 6 * SPB, 1.5, loop=True, metronome=True)
    parallel = render(engine, THREADS, 6 * SPB, 1.5, loop=True, metronome=True)
    np.testing.assert_array_equal(parallel, serial)


@pytest.mark.parametrize("seed", range(10))
def test_random_graphs_with_sends_render_the_same_on_any_threads(engine, make_wav, uids, seed):
    """The graph fans out: tracks going into a group and several returns, pre-
    and post-fader, returns into returns, each edge delayed on its own."""
    rng = np.random.default_rng(100 + seed)
    groups, leaves = random_project(engine, rng, make_wav, uids, tracks=int(rng.integers(4, 16)))
    add_returns(engine, rng, uids, groups + leaves, count=int(rng.integers(1, 5)))
    settle(engine)
    serial = render(engine, 1, 8 * SPB)
    parallel = render(engine, THREADS, 8 * SPB)
    assert np.abs(serial).max() > 0.0
    np.testing.assert_array_equal(parallel, serial)
    engine.set_loop(True, 2.0, 5.0)
    serial = render(engine, 1, 6 * SPB, 1.5, loop=True, metronome=True)
    parallel = render(engine, THREADS, 6 * SPB, 1.5, loop=True, metronome=True)
    np.testing.assert_array_equal(parallel, serial)


@pytest.mark.parametrize("seed", range(6))
def test_random_graphs_with_inputs_render_the_same_on_any_threads(engine, make_wav, uids, seed):
    """Tracks taking their input from others (resampling): an input edge isn't
    heard offline, but orders the graph like any edge (the source first)."""
    rng = np.random.default_rng(200 + seed)
    groups, leaves = random_project(engine, rng, make_wav, uids, tracks=int(rng.integers(4, 16)))
    tracks = groups + leaves + add_returns(engine, rng, uids, groups + leaves, count=int(rng.integers(0, 3)))
    inputs = 0
    for track in leaves:
        if rng.random() < 0.6:
            try:
                engine.set_track_input_track(track, int(rng.choice([ge.MASTER, *tracks])))
                inputs += 1
            except ValueError:
                pass  # itself, or a track it feeds
    assert inputs > 0 or not leaves
    settle(engine)
    serial = render(engine, 1, 8 * SPB)
    parallel = render(engine, THREADS, 8 * SPB)
    np.testing.assert_array_equal(parallel, serial)


def test_nested_groups_on_any_threads(engine, make_wav, uids):
    """Groups three deep, with tracks at every level: each bus sums what goes into it in a fixed order."""
    rng = np.random.default_rng(7)
    noise = make_wav(rng.uniform(-0.5, 0.5, (SAMPLE_RATE, 2)))
    engine.load_source(noise)
    outer = engine.add_track()
    middle = engine.add_track()
    inner = engine.add_track()
    engine.set_track_output(middle, outer)
    engine.set_track_output(inner, middle)
    for bus in (outer, middle, inner):
        utility(engine, bus, -3.0)
        for i in range(4):
            track = engine.add_track()
            engine.set_track_output(track, bus)
            if i % 2:
                synth(engine, track, random_notes(rng, 4.0))
            else:
                engine.set_track_clips(track, [ge.ClipDesc(noise, i * 0.5, 1.0)])
                utility(engine, track, float(i))
            if uids and i == 3:
                latent_effect(engine, uids, track, 100 * (bus % 4) + 37)
    assert_same_on_any_threads(engine, 4 * SPB)


def test_solo_and_mute_on_any_threads(engine, make_wav):
    rng = np.random.default_rng(3)
    bus = engine.add_track()
    tracks = []
    for i in range(8):
        track = engine.add_track()
        synth(engine, track, random_notes(rng, 4.0))
        if i < 4:
            engine.set_track_output(track, bus)
        tracks.append(track)
    for solo, mute in [((tracks[0],), ()), ((bus,), (tracks[1],)), ((tracks[5], tracks[2]), (bus,)), ((), (tracks[6],))]:
        for track in [bus, *tracks]:
            engine.set_track_solo(track, track in solo)
            engine.set_track_mute(track, track in mute)
        assert_same_on_any_threads(engine, 4 * SPB)


def test_stress(engine, make_wav):
    """Many tracks, many renders, the thread count changing in between: always the same."""
    rng = np.random.default_rng(4)
    noise = make_wav(rng.uniform(-0.5, 0.5, (SAMPLE_RATE, 2)))
    engine.load_source(noise)
    groups = [engine.add_track() for _ in range(6)]
    for i, group in enumerate(groups[1:], 1):
        engine.set_track_output(group, groups[i // 2])
    for i in range(58):
        track = engine.add_track()
        engine.set_track_output(track, groups[i % len(groups)])
        if i % 3 == 0:
            synth(engine, track, random_notes(rng, 2.0, 6))
        else:
            engine.set_track_clips(track, [ge.ClipDesc(noise, float(rng.uniform(0.0, 1.0)), 1.0)])
            utility(engine, track, float(rng.uniform(-12.0, 0.0)))
    settle(engine)
    expected = render(engine, 1, 2 * SPB)
    assert np.abs(expected).max() > 0.0
    for run in range(30):
        threads = [2, 3, THREADS, 8][run % 4]
        np.testing.assert_array_equal(render(engine, threads, 2 * SPB), expected)
    assert engine.nodes_on_workers > 0


def test_few_tracks_render_serially(engine, make_wav):
    """Below the threshold (few tracks with work, small blocks) the workers aren't woken."""
    track = engine.add_track()
    synth(engine, track, [(0.0, 4.0, 60, 100)])
    render(engine, THREADS, 4 * SPB)
    assert engine.nodes_on_workers == 0  # one track with devices: nothing to share


# --- Cost ordering ------------------------------------------------------------------


def test_the_longest_paths_start_first():
    """A node's rank is the work from it to the end of its path; the nodes without
    inputs are queued by rank, highest first, ties (and unknown costs) in order."""
    # 0 -> master; 1, 2 -> group 5 -> master; 3 -> group 4 -> group 5; 6 -> master
    outputs = [-1, 5, 5, 4, 5, -1, -1]
    costs = [1.0, 2.0, 0.5, 0.25, 1.0, 5.0, 4.0]
    order, ranks = ge.task_graph_order(outputs, costs)
    assert ranks == pytest.approx([1.0, 7.0, 5.5, 6.25, 6.0, 5.0, 4.0])
    assert order == [1, 3, 2, 6, 0]  # a light track feeding heavy groups goes before a heavier one alone
    assert ge.task_graph_order(outputs, [0.0] * 7)[0] == [0, 1, 2, 3, 6]
    assert ge.task_graph_order([-1, -1, -1], [2.0, 2.0, 3.0])[0] == [2, 0, 1]
    with pytest.raises(ValueError):
        ge.task_graph_order([-1, 0], [1.0, 1.0])  # a group listed before what goes into it
    with pytest.raises(ValueError):
        ge.task_graph_order([-1, -1], [1.0])


def test_track_costs_are_measured(engine):
    light = engine.add_track()
    utility(engine, light, 0.0)
    heavy = engine.add_track()
    synth(engine, heavy, [(0.0, 4.0, key, 100) for key in range(48, 72, 3)])
    for _ in range(4):
        engine.add_builtin_processor(engine.track_chain(heavy), "ott")
    assert {c.track_id: c.ns_per_frame for c in engine.track_costs()} == {light: 0.0, heavy: 0.0}
    render(engine, THREADS, 4 * SPB)
    costs = {c.track_id: c.ns_per_frame for c in engine.track_costs()}
    assert costs[light] > 0.0
    assert costs[heavy] > 4 * costs[light]


def test_cost_ordering_changes_nothing_but_the_order(engine):
    """Heavy tracks last in routing order: started first by cost, last without; the same render either way."""
    rng = np.random.default_rng(5)
    bus = engine.add_track()
    for i in range(12):
        track = engine.add_track()
        synth(engine, track, random_notes(rng, 4.0))
        if i % 3 == 0:
            engine.set_track_output(track, bus)
        if i >= 10:
            for _ in range(3):
                engine.add_builtin_processor(engine.track_chain(track), "ott")
    assert engine.cost_ordering
    by_cost = assert_same_on_any_threads(engine, 4 * SPB)
    engine.cost_ordering = False
    assert not engine.cost_ordering
    np.testing.assert_array_equal(render(engine, THREADS, 4 * SPB), by_cost)
