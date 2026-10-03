"""Racks (device groups) in the engine: chains side by side, summed; their
faders, mute and solo; delay compensation inside a rack (a latent device in one
chain doesn't smear the others) and around it; automation of nested devices
and of chain faders in time; instrument racks layering synths; sidechains into
devices in racks; moving and removing racks; nesting limits; chain meters; and
renders bit-identical with and without workers. Offline, unless a test needs
the fake ASIO driver."""

import numpy as np
import pytest

from substation import _engine as ge

from .conftest import SAMPLE_RATE, TEST_PLUGINS

SPB = SAMPLE_RATE // 2  # samples per beat at 120 BPM
PLUGINS = str(TEST_PLUGINS)
FX_GAIN, FX_LATENCY = 0, 1  # SUB Test Effect's parameters
CLICK = 0.25

needs_plugins = pytest.mark.skipif(not TEST_PLUGINS.exists(), reason="test plug-ins not built")


@pytest.fixture
def engine():
    e = ge.Engine()
    e.set_clip_fade_ms(0)
    yield e
    e.close_device()


@pytest.fixture(scope="module")
def uids():
    return {d.name: d.uid for d in ge.scan_vst3(PLUGINS)} if TEST_PLUGINS.exists() else {}


@pytest.fixture
def click_wav(make_wav):
    click = np.zeros(1000)
    click[0] = CLICK
    return make_wav(click)


def clip_track(engine, path, start_beat=1.0, seconds=1000 / SAMPLE_RATE):
    engine.load_source(path)
    track = engine.add_track()
    engine.set_track_clips(track, [ge.ClipDesc(path, start_beat, seconds)])
    return track


def rack(engine, chain, chains=2, index=-1):
    """A rack in a chain, with `chains` empty chains: (rack id, [chain ids])."""
    rid = engine.add_rack(chain, index)
    return rid, [engine.add_rack_chain(rid) for _ in range(chains)]


def effect(engine, uids, chain, latency=0, gain=1.0):
    """SUB Test Effect in a chain: `gain` times its input, `latency` samples late."""
    pid = engine.add_plugin_processor(chain, "VST3", PLUGINS, uids["SUB Test Effect"])
    engine.set_processor_param(pid, FX_GAIN, gain / 2)  # (0..1 is 0..2 times)
    engine.set_processor_param(pid, FX_LATENCY, latency)
    engine.idle()  # the plug-in asked for a restart to change its latency
    assert engine.processor_info(pid).latency == latency
    return pid


def utility(engine, chain, gain_db=0.0):
    pid = engine.add_builtin_processor(chain, "utility")
    engine.set_processor_param(pid, engine.processor_param_index(pid, "gain"), gain_db)
    return pid


def render(engine, beats=2.0):
    return engine.render_offline(0.0, int(beats * SPB))


def clicks(out) -> dict[int, float]:
    out = out[:, 0] if out.ndim == 2 else out
    return {int(i): round(float(out[i]), 6) for i in np.nonzero(np.abs(out) > 1e-7)[0]}


def test_an_empty_rack_passes_its_input_on(engine, click_wav):
    track = clip_track(engine, click_wav)
    rid = engine.add_rack(engine.track_chain(track))
    info = engine.processor_info(rid)
    assert (info.type_id, info.name, info.latency, info.has_sidechain) == ("rack", "Rack", 0, False)
    assert engine.rack_chains(rid) == []
    assert clicks(render(engine)) == {SPB: CLICK}
    chain = engine.add_rack_chain(rid)  # an empty chain: its input
    assert engine.chain_rack(chain) == rid and engine.chain_rack(engine.track_chain(track)) == 0
    assert clicks(render(engine)) == {SPB: CLICK}


@needs_plugins
def test_chains_sum_their_devices_outputs(engine, uids, click_wav):
    track = clip_track(engine, click_wav)
    rid, (a, b) = rack(engine, engine.track_chain(track))
    effect(engine, uids, a, gain=0.5)
    effect(engine, uids, b, gain=0.25)
    assert clicks(render(engine)) == {SPB: pytest.approx(CLICK * 0.75)}
    third = engine.add_rack_chain(rid, 0)  # an empty one, first
    assert engine.rack_chains(rid) == [third, a, b]
    assert clicks(render(engine)) == {SPB: pytest.approx(CLICK * 1.75)}
    engine.set_rack_chain_order(rid, [b, a, third])
    assert engine.rack_chains(rid) == [b, a, third]
    with pytest.raises(ValueError):
        engine.set_rack_chain_order(rid, [a, b])
    engine.remove_rack_chain(third)
    assert clicks(render(engine)) == {SPB: pytest.approx(CLICK * 0.75)}
    engine.set_processor_enabled(rid, False)  # switched off: its input as it is
    assert clicks(render(engine)) == {SPB: CLICK}
    engine.set_processor_enabled(rid, True)
    # A device after the rack hears the sum.
    effect(engine, uids, engine.track_chain(track), gain=2.0)
    assert clicks(render(engine)) == {SPB: pytest.approx(CLICK * 1.5)}


def test_chain_faders_mute_and_solo(engine, click_wav):
    track = clip_track(engine, click_wav)
    _, (a, b) = rack(engine, engine.track_chain(track))

    def heard():
        out = render(engine)
        return round(float(out[SPB, 0]), 6), round(float(out[SPB, 1]), 6)

    assert heard() == (2 * CLICK, 2 * CLICK)
    engine.set_chain_gain(a, 0.5)
    assert heard() == pytest.approx((1.5 * CLICK, 1.5 * CLICK))
    engine.set_chain_pan(b, 1.0)  # hard right
    assert heard() == pytest.approx((0.5 * CLICK, 1.5 * CLICK))
    engine.set_chain_pan(b, 0.0)
    engine.set_chain_mute(a, True)
    assert heard() == pytest.approx((CLICK, CLICK))
    engine.set_chain_mute(a, False)
    engine.set_chain_solo(a, True)  # only the soloed chains of the rack
    assert heard() == pytest.approx((0.5 * CLICK, 0.5 * CLICK))
    engine.set_chain_solo(b, True)
    assert heard() == pytest.approx((1.5 * CLICK, 1.5 * CLICK))
    engine.set_chain_mute(b, True)  # muted wins
    assert heard() == pytest.approx((0.5 * CLICK, 0.5 * CLICK))
    with pytest.raises(ValueError):  # a track's own chain has no fader of its own
        engine.set_chain_gain(engine.track_chain(track), 0.5)


@needs_plugins
def test_a_latent_device_in_one_chain_doesnt_smear_the_others(engine, uids, click_wav):
    """Every click lands on beat 1 as one: the other chains are delayed to line
    up with the slowest, and the rack's latency is compensated like any device's."""
    track = clip_track(engine, click_wav)
    rid, (a, b) = rack(engine, engine.track_chain(track))
    clip_track(engine, click_wav)  # beside it, delayed to line up with it
    effect(engine, uids, a, latency=300)
    assert engine.processor_info(rid).latency == 300
    assert clicks(render(engine)) == {SPB: pytest.approx(3 * CLICK)}
    effect(engine, uids, b, latency=120)
    effect(engine, uids, b, latency=250)  # now the slowest
    assert engine.processor_info(rid).latency == 370
    assert clicks(render(engine)) == {SPB: pytest.approx(3 * CLICK)}
    # Before and after the rack on its track, and a rack in a rack.
    effect(engine, uids, engine.track_chain(track), latency=50)
    engine.move_processor(engine.chain_processors(engine.track_chain(track))[-1], engine.track_chain(track), 0)
    inner, (c, _d) = rack(engine, a)
    effect(engine, uids, c, latency=500)
    assert engine.processor_info(inner).latency == 500
    assert engine.processor_info(rid).latency == 800
    effect(engine, uids, engine.track_chain(track), latency=30)
    assert clicks(render(engine)) == {SPB: pytest.approx(4 * CLICK)}  # (the inner rack doubles chain a)


@needs_plugins
def test_nested_device_automation_plays_in_time(engine, uids, make_wav):
    """A device in a rack hears the timeline as late as the devices before the
    rack and before it in its chain: its automation is as late."""
    track = clip_track(engine, make_wav(np.full((2 * SAMPLE_RATE, 2), 0.5)), start_beat=0.0, seconds=2.0)
    effect(engine, uids, engine.track_chain(track), latency=100)
    _rid, (a, b) = rack(engine, engine.track_chain(track))
    engine.set_chain_mute(b, True)
    effect(engine, uids, a, latency=50)
    effect(engine, uids, b, latency=400)  # the slowest chain: chain a is delayed after it
    gain = utility(engine, a)
    info = engine.processor_params(gain)[engine.processor_param_index(gain, "gain")]
    quiet, loud = info.to_normalized(-60.0), info.to_normalized(0.0)
    step = SPB + 400
    engine.set_track_automation(track, [ge.AutomationLane(gain, "gain", [
        ge.AutomationPoint(0.0, quiet), ge.AutomationPoint(step / SPB, quiet), ge.AutomationPoint(step / SPB, loud)])])
    out = render(engine, 3.0)[:, 0]
    assert np.abs(out[step - 300 : step]).max() < 0.001  # still at -60 dB right up to the step
    assert out[step + 40] > 0.01  # and rising from it at once (smoothed)


@needs_plugins
def test_chain_volume_automation_plays_in_time(engine, uids, make_wav):
    """A chain's fader comes after the devices before the rack and in its chain."""
    track = clip_track(engine, make_wav(np.full((2 * SAMPLE_RATE, 2), 0.5)), start_beat=0.0, seconds=2.0)
    effect(engine, uids, engine.track_chain(track), latency=100)
    rid, (a, b) = rack(engine, engine.track_chain(track))
    effect(engine, uids, a, latency=50)
    effect(engine, uids, b, latency=400)
    engine.set_chain_mute(b, True)
    step = SPB + 400
    engine.set_track_automation(track, [ge.AutomationLane(rid, f"chain:{a}:volume", [
        ge.AutomationPoint(0.0, 0.0), ge.AutomationPoint(step / SPB, 0.0), ge.AutomationPoint(step / SPB, 1.0)])])
    out = render(engine, 3.0)[:, 0]
    assert np.abs(out[step - 300 : step]).max() < 1e-6  # silent right up to the step
    assert out[step + 1] == pytest.approx(0.5 * ge.MAX_VOLUME_GAIN, rel=1e-4)  # a fader isn't smoothed offline
    engine.set_track_automation(track, [ge.AutomationLane(rid, f"chain:{a}:pan", [ge.AutomationPoint(0.0, 1.0)])])
    out = render(engine, 3.0)
    assert out[step, 0] == pytest.approx(0.0, abs=1e-7) and out[step, 1] == pytest.approx(0.5, rel=1e-4)


def synth_track(engine, chains):
    """A MIDI track playing one note, with a synth in each of `chains` (chain ids made by `chains(track)`)."""
    track = engine.add_track()
    engine.set_track_notes(track, [ge.NoteDesc(0.5, 1.0, 60, 100)])
    for chain in chains(track):
        engine.add_builtin_processor(chain, "synth")
    return track


def test_an_instrument_rack_layers_two_synths(engine):
    layered = synth_track(engine, lambda t: rack(engine, engine.track_chain(t))[1])
    both = render(engine)
    assert np.abs(both).max() > 0.01
    engine.remove_track(layered)
    synth_track(engine, lambda t: [engine.track_chain(t)])
    synth_track(engine, lambda t: [engine.track_chain(t)])
    np.testing.assert_allclose(both, render(engine), atol=1e-6)  # two tracks of one synth each


@needs_plugins
@pytest.mark.parametrize("before_rack, before_device, other_chain", [
    (0, 0, 0),
    (200, 0, 0),    # the sidechain comes early: it waits
    (0, 300, 0),
    (0, 0, 500),    # the device's chain is delayed after it, not before
    (100, 50, 400),
])
def test_a_sidechain_into_a_device_in_a_rack_lines_up(engine, uids, click_wav, before_rack, before_device,
                                                      other_chain):
    """SUB Test Sidechain puts out its input plus its sidechain: both clicks land
    on beat 1 together, whatever the latency before the rack and in it."""
    source = clip_track(engine, click_wav)
    track = clip_track(engine, click_wav)
    if before_rack:
        effect(engine, uids, engine.track_chain(track), latency=before_rack)
    _rid, (a, b) = rack(engine, engine.track_chain(track))
    engine.set_chain_mute(b, True)
    if before_device:
        effect(engine, uids, a, latency=before_device)
    keyed = engine.add_plugin_processor(a, "VST3", PLUGINS, uids["SUB Test Sidechain"])
    if other_chain:
        effect(engine, uids, b, latency=other_chain)
    engine.set_track_gain(source, 0.0)  # heard only through the sidechain (taken before the fader)
    engine.set_processor_sidechain(keyed, source, ge.SidechainTap.PRE_FADER)
    assert clicks(render(engine)) == {SPB: pytest.approx(2 * CLICK)}
    with pytest.raises(ValueError):  # its own track: a cycle
        engine.set_processor_sidechain(keyed, track)


@needs_plugins
def test_moving_devices_into_and_out_of_racks(engine, uids, click_wav):
    track = clip_track(engine, click_wav)
    other = clip_track(engine, click_wav, start_beat=0.5)
    fx = effect(engine, uids, engine.track_chain(track), gain=0.5)
    rid, (a, b) = rack(engine, engine.track_chain(track))
    engine.move_processor(fx, a)  # in: one chain halves, the other passes
    assert engine.processor_chain(fx) == a and engine.chain_processors(a) == [fx]
    assert clicks(render(engine)) == {SPB // 2: CLICK, SPB: pytest.approx(1.5 * CLICK)}
    engine.move_processor(rid, engine.track_chain(other))  # the rack moves with its chains and devices
    assert engine.rack_chains(rid) == [a, b] and engine.processor_chain(fx) == a
    assert clicks(render(engine)) == {SPB // 2: pytest.approx(1.5 * CLICK), SPB: CLICK}
    with pytest.raises(ValueError):  # into itself
        engine.move_processor(rid, a)
    inner, (c, _) = rack(engine, b)
    with pytest.raises(ValueError):  # into a rack in it
        engine.move_processor(rid, c)
    engine.move_processor(fx, engine.track_chain(track))  # out again
    assert clicks(render(engine)) == {SPB // 2: pytest.approx(3 * CLICK), SPB: pytest.approx(0.5 * CLICK)}
    engine.remove_processor(rid)  # with everything in it
    with pytest.raises(ValueError):
        engine.processor_info(inner)
    with pytest.raises(ValueError):
        engine.chain_processors(c)
    assert clicks(render(engine)) == {SPB // 2: CLICK, SPB: pytest.approx(0.5 * CLICK)}
    engine.remove_track(track)
    with pytest.raises(ValueError):
        engine.processor_info(fx)


def test_racks_nest_at_most_max_rack_depth_deep(engine):
    track = engine.add_track()
    chain, racks = engine.track_chain(track), []
    for _ in range(ge.MAX_RACK_DEPTH):
        rid, (chain,) = rack(engine, chain, chains=1)
        racks.append(rid)
    with pytest.raises(ValueError):
        engine.add_rack(chain)
    engine.add_builtin_processor(chain, "utility")  # devices may go there
    with pytest.raises(ValueError):  # nor may a rack move deeper
        engine.move_processor(racks[1], engine.rack_chains(racks[-1])[0])
    np.testing.assert_array_equal(render(engine), 0.0)
    engine.remove_track(track)  # everything in it goes


def test_a_sidechain_moving_with_a_rack_cant_close_a_cycle(engine, uids, click_wav):
    if not TEST_PLUGINS.exists():
        pytest.skip("test plug-ins not built")
    source, track = clip_track(engine, click_wav), engine.add_track()
    rid, (a,) = rack(engine, engine.track_chain(track), chains=1)
    keyed = engine.add_plugin_processor(a, "VST3", PLUGINS, uids["SUB Test Sidechain"])
    engine.set_processor_sidechain(keyed, source)
    with pytest.raises(ValueError):  # onto its own source
        engine.move_processor(rid, engine.track_chain(source))
    assert engine.processor_chain(rid) == engine.track_chain(track)
    engine.remove_track(source)  # its sidechain goes with its source
    assert engine.processor_sidechain(keyed) is None


@needs_plugins
def test_racks_render_the_same_with_and_without_workers(engine, uids, make_wav):
    rng = np.random.default_rng(9)
    noise = make_wav(rng.uniform(-0.5, 0.5, (SAMPLE_RATE, 2)))
    for t in range(6):
        track = clip_track(engine, noise, start_beat=0.0, seconds=1.0)
        _rid, chains = rack(engine, engine.track_chain(track), chains=3)
        for i, chain in enumerate(chains):
            effect(engine, uids, chain, latency=37 * ((t + i) % 4), gain=0.5 + 0.1 * i)
        _inner, (c, d) = rack(engine, chains[0])
        utility(engine, c, -3.0)
        effect(engine, uids, d, latency=11 * t)
        engine.set_chain_gain(chains[1], 0.7)
        engine.add_builtin_processor(chains[2], "synth")
        engine.set_track_notes(track, [ge.NoteDesc(0.25 * t, 0.5, 60 + t, 100)])
    engine.audio_threads = 1
    render(engine, 4.0)  # (the utilities' gains glide to where they were set, once)
    serial = render(engine, 4.0)
    engine.audio_threads = 4
    engine.cost_ordering = False
    parallel = render(engine, 4.0)
    np.testing.assert_array_equal(serial, parallel)


needs_asio = pytest.mark.skipif("ASIO" not in ge.driver_types(), reason="built without the ASIO SDK")


@needs_asio
def test_chain_meters(click_wav, make_wav):
    from .test_asio import Driver, open_asio

    driver = Driver()
    driver.SetManual(1)
    engine = ge.Engine()
    try:
        track = clip_track(engine, make_wav(np.full((SAMPLE_RATE, 2), 0.5)), start_beat=0.0, seconds=1.0)
        _, (a, b) = rack(engine, engine.track_chain(track))
        engine.set_chain_gain(b, 0.5)
        open_asio(engine, sample_rate=SAMPLE_RATE, buffer_frames=256)
        engine.play()
        engine.take_meters()
        driver.process(20)
        meters = {(m.track_id, m.chain_id): m.left for m in engine.take_meters()}
        assert meters[(track, a)] == pytest.approx(0.5, rel=1e-3)
        assert meters[(track, b)] == pytest.approx(0.25, rel=1e-3)
        assert meters[(track, 0)] == pytest.approx(0.75, rel=1e-3)  # the track's own
    finally:
        engine.close_device()
