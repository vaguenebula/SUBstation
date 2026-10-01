"""Routing in the engine: tracks going into other tracks (group buses), delay
compensation at every summing point, and solo and mute across levels.
Rendered offline, so no audio device is needed."""

import numpy as np
import pytest

from gilstudio import _engine as ge

from .conftest import SAMPLE_RATE, TEST_PLUGINS

SPB = SAMPLE_RATE // 2  # samples per beat at 120 BPM
PLUGINS = str(TEST_PLUGINS)
FX_GAIN, FX_LATENCY = 0, 1  # GIL Test Effect's parameters

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


def clip_track(engine, path, start_beat=0.0, duration_sec=1.0, output=None):
    engine.load_source(path)
    track = engine.add_track()
    engine.set_track_clips(track, [ge.ClipDesc(path, start_beat, duration_sec)])
    if output is not None:
        engine.set_track_output(track, output)
    return track


def group(engine, output=None):
    track = engine.add_track()
    if output is not None:
        engine.set_track_output(track, output)
    return track


def utility(engine, track, gain_db):
    processor = engine.add_builtin_processor(engine.track_chain(track), "utility")
    engine.set_processor_param(processor, engine.processor_param_index(processor, "gain"), gain_db)
    return processor


def latent_effect(engine, uids, track, latency):
    effect = engine.add_plugin_processor(engine.track_chain(track), "VST3", PLUGINS, uids["GIL Test Effect"])
    engine.set_processor_param(effect, FX_LATENCY, latency)
    engine.idle()  # the plug-in asked for a restart to change its latency
    return effect


@pytest.fixture
def click_wav(make_wav):
    click = np.zeros(1000)
    click[0] = 0.25
    return make_wav(click)


def clicks(out) -> list[int]:
    return np.nonzero(np.abs(out[:, 0]) > 1e-6)[0].tolist()


def test_a_group_effect_processes_the_sum_of_its_tracks(engine, make_wav):
    a_wav = make_wav(np.full((SAMPLE_RATE, 2), 0.5))
    b_wav = make_wav(np.full((SAMPLE_RATE, 2), 0.25))
    bus = group(engine)
    a = clip_track(engine, a_wav, output=bus)
    clip_track(engine, b_wav, output=bus)
    assert engine.track_output(a) == bus
    assert engine.render_offline(0.0, 100)[50] == pytest.approx([0.75, 0.75])
    utility(engine, bus, -6.0206)  # halves the sum
    engine.set_track_gain(bus, 0.5)  # and the group's fader halves it again
    assert engine.render_offline(0.0, 4000)[-1] == pytest.approx([0.1875, 0.1875], abs=1e-4)

    # Nested: the group goes into another one, which goes into the master.
    outer = group(engine)
    engine.set_track_output(bus, outer)
    engine.set_track_gain(outer, 2.0)
    assert engine.render_offline(0.0, 4000)[-1] == pytest.approx([0.375, 0.375], abs=1e-4)

    # Back to the master: the outer group hears nothing.
    engine.set_track_output(bus, ge.MASTER)
    assert engine.render_offline(0.0, 4000)[-1] == pytest.approx([0.1875, 0.1875], abs=1e-4)


def test_routes_that_would_close_a_cycle_are_refused(engine, dc_wav):
    outer = group(engine)
    inner = group(engine, outer)
    track = clip_track(engine, dc_wav, output=inner)
    for frm, to in ((outer, inner), (outer, track), (inner, track), (track, track), (outer, outer)):
        with pytest.raises(ValueError):
            engine.set_track_output(frm, to)
    with pytest.raises(ValueError):
        engine.set_track_output(track, 999)
    assert engine.track_output(outer) == ge.MASTER
    assert engine.render_offline(0.0, 100)[50] == pytest.approx([0.5, 0.5])


def test_removing_a_group_sends_what_went_into_it_to_the_master(engine, dc_wav):
    bus = group(engine)
    track = clip_track(engine, dc_wav, output=bus)
    engine.set_track_mute(bus, True)
    assert engine.render_offline(0.0, 4000)[-1] == pytest.approx([0.0, 0.0])
    engine.remove_track(bus)
    assert engine.track_output(track) == ge.MASTER
    assert engine.render_offline(0.0, 4000)[-1] == pytest.approx([0.5, 0.5])


def test_the_meters_of_a_group(engine, dc_wav):
    bus = group(engine)
    clip_track(engine, dc_wav, output=bus)
    assert {m.track_id for m in engine.take_meters()} >= {ge.MASTER, bus}


def test_solo_and_mute_across_levels(engine, make_wav):
    level = iter((0.5, 0.25, 0.125, 0.0625))
    wavs = [make_wav(np.full((SAMPLE_RATE, 2), next(level))) for _ in range(4)]
    outer = group(engine)
    inner = group(engine, outer)
    a = clip_track(engine, wavs[0], output=inner)  # 0.5, in the inner group
    b = clip_track(engine, wavs[1], output=inner)  # 0.25, in the inner group
    c = clip_track(engine, wavs[2], output=outer)  # 0.125, in the outer group
    d = clip_track(engine, wavs[3])  # 0.0625, on its own

    def heard() -> float:
        return float(engine.render_offline(0.0, 4000)[-1, 0])  # past the 20 ms smoothing

    assert heard() == pytest.approx(0.9375)
    # Soloing a group solos what is in it.
    engine.set_track_solo(inner, True)
    assert heard() == pytest.approx(0.75)
    engine.set_track_solo(outer, True)
    assert heard() == pytest.approx(0.875)
    engine.set_track_solo(inner, False)
    engine.set_track_solo(outer, False)
    # Soloing a track keeps its groups heard, but not the other tracks in them.
    engine.set_track_solo(b, True)
    assert heard() == pytest.approx(0.25)
    engine.set_track_solo(d, True)
    assert heard() == pytest.approx(0.3125)
    # A muted group silences what is in it, soloed or not.
    engine.set_track_mute(inner, True)
    assert heard() == pytest.approx(0.0625)
    engine.set_track_mute(inner, False)
    engine.set_track_solo(b, False)
    engine.set_track_solo(d, False)
    engine.set_track_mute(outer, True)
    assert heard() == pytest.approx(0.0625)
    engine.set_track_mute(outer, False)
    # Muting a track in a soloed group leaves the rest of the group.
    engine.set_track_solo(outer, True)
    engine.set_track_mute(a, True)
    assert heard() == pytest.approx(0.375)
    engine.set_track_solo(c, True)  # one soloed inside a soloed group: the group still plays whole
    assert heard() == pytest.approx(0.375)


@needs_plugins
def test_compensation_in_nested_groups(engine, uids, click_wav):
    """Every click lands on beat 1, whatever latency sits where in the tree."""
    outer = group(engine)
    inner = group(engine, outer)
    deep = clip_track(engine, click_wav, start_beat=1.0, output=inner)  # in a group in a group
    shallow = clip_track(engine, click_wav, start_beat=1.0, output=outer)
    alone = clip_track(engine, click_wav, start_beat=1.0)  # straight into the master

    def check() -> None:
        out = engine.render_offline(0.0, 2 * SPB)
        assert clicks(out) == [SPB]
        assert out[SPB, 0] == pytest.approx(0.75, abs=1e-4)

    check()
    latent_effect(engine, uids, deep, 100)  # an unbalanced tree: deep in one branch
    check()
    latent_effect(engine, uids, inner, 30)  # on a group
    check()
    latent_effect(engine, uids, shallow, 200)  # more than the other branch
    check()
    latent_effect(engine, uids, outer, 50)
    latent_effect(engine, uids, alone, 70)
    check()
    engine.set_track_output(inner, ge.MASTER)  # the tree changes shape
    check()


@needs_plugins
def test_group_device_automation_plays_in_time(engine, uids, make_wav):
    """A group's device hears the timeline as late as its slowest input: its
    automation is as late."""
    bus = group(engine)
    track = clip_track(engine, make_wav(np.full((2 * SAMPLE_RATE, 2), 0.5)), duration_sec=2.0, output=bus)
    latent_effect(engine, uids, track, 100)
    latent_effect(engine, uids, bus, 50)
    gain = utility(engine, bus, 0.0)
    info = engine.processor_params(gain)[engine.processor_param_index(gain, "gain")]
    quiet, loud = info.to_normalized(-60.0), info.to_normalized(0.0)
    step = SPB + 400  # not on a block boundary of the render (which starts 150 samples early)
    engine.set_track_automation(bus, [ge.AutomationLane(gain, "gain", [
        ge.AutomationPoint(0.0, quiet), ge.AutomationPoint(step / SPB, quiet), ge.AutomationPoint(step / SPB, loud)])])
    out = engine.render_offline(0.0, step + 2000)[:, 0]
    assert np.abs(out[step - 300 : step]).max() < 0.001  # still at -60 dB right up to the step
    assert out[step + 40] > 0.01  # and rising from it at once (smoothed)


@needs_plugins
def test_group_volume_automation_plays_in_time(engine, uids, make_wav, dc_wav):
    """A group's fader comes after the latency of what feeds it and of its own devices."""
    bus = group(engine)
    track = clip_track(engine, make_wav(np.full((2 * SAMPLE_RATE, 2), 0.5)), duration_sec=2.0, output=bus)
    latent_effect(engine, uids, track, 100)
    latent_effect(engine, uids, bus, 50)
    clip_track(engine, dc_wav)  # beside it, delayed to line up with the group
    step = SPB + 400
    engine.set_track_automation(bus, [ge.AutomationLane(0, "volume", [
        ge.AutomationPoint(0.0, 0.0), ge.AutomationPoint(step / SPB, 0.0), ge.AutomationPoint(step / SPB, 1.0)])])
    out = engine.render_offline(0.0, step + 2000)[:, 0]
    assert np.abs(out[step - 300 : step] - 0.5).max() < 1e-4  # only the other track up to the step
    assert out[step + 40] > 0.6
