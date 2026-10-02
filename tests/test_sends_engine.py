"""Sends in the engine: a track's signal also going into return tracks, after
its fader or before it; delay compensation per edge (a track going to places
of different latency is delayed differently on each); cycles across outputs
and sends; and solo and mute across sends. Rendered offline, so no audio
device is needed."""

import numpy as np
import pytest

from substation import _engine as ge

from .conftest import SAMPLE_RATE, TEST_PLUGINS

SPB = SAMPLE_RATE // 2  # samples per beat at 120 BPM
PLUGINS = str(TEST_PLUGINS)
FX_GAIN, FX_LATENCY = 0, 1  # SUB Test Effect's parameters

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


def utility(engine, track, gain_db):
    processor = engine.add_builtin_processor(engine.track_chain(track), "utility")
    engine.set_processor_param(processor, engine.processor_param_index(processor, "gain"), gain_db)
    return processor


def latent_effect(engine, uids, track, latency):
    effect = engine.add_plugin_processor(engine.track_chain(track), "VST3", PLUGINS, uids["SUB Test Effect"])
    engine.set_processor_param(effect, FX_LATENCY, latency)
    engine.idle()  # the plug-in asked for a restart to change its latency
    return effect


def level(engine) -> float:
    return float(engine.render_offline(0.0, 4000)[-1, 0])  # past a Utility's ramp


@pytest.fixture
def click_wav(make_wav):
    click = np.zeros(1000)
    click[0] = 0.25
    return make_wav(click)


def clicks(out) -> list[int]:
    return np.nonzero(np.abs(out[:, 0]) > 1e-6)[0].tolist()


def test_post_and_pre_fader_sends_at_their_levels(engine, dc_wav):
    track = clip_track(engine, dc_wav)
    ret = engine.add_track()  # a return: no clips, fed by sends
    engine.set_track_gain(track, 0.5)
    engine.set_track_send(track, ret, 0.5)
    assert [(s.track_id, s.gain, s.pre_fader) for s in engine.track_sends(track)] == [(ret, 0.5, False)]
    assert level(engine) == pytest.approx(0.25 + 0.125)  # after the fader: 0.5 * 0.5 * 0.5
    engine.set_track_send(track, ret, 0.5, pre_fader=True)
    assert level(engine) == pytest.approx(0.25 + 0.25)  # before it: the track's fader doesn't count
    engine.set_track_gain(track, 0.0)
    assert level(engine) == pytest.approx(0.25)  # only the return
    engine.set_track_send(track, ret, 1.0, pre_fader=True)  # a new level alone
    assert level(engine) == pytest.approx(0.5)
    engine.set_track_gain(ret, 0.5)  # the return's own fader
    assert level(engine) == pytest.approx(0.25)
    engine.remove_track_send(track, ret)
    assert engine.track_sends(track) == []
    assert level(engine) == pytest.approx(0.0)


def test_a_return_effect_processes_the_sum_of_what_is_sent_to_it(engine, make_wav):
    a = clip_track(engine, make_wav(np.full((SAMPLE_RATE, 2), 0.5)))
    b = clip_track(engine, make_wav(np.full((SAMPLE_RATE, 2), 0.25)))
    ret = engine.add_track()
    for track in (a, b):
        engine.set_track_gain(track, 0.0)  # only what is sent (pre-fader) is heard
        engine.set_track_send(track, ret, 1.0, pre_fader=True)
    assert level(engine) == pytest.approx(0.75)
    utility(engine, ret, -6.0206)  # halves the sum
    assert level(engine) == pytest.approx(0.375, abs=1e-4)
    # A return sending on into another return, as a track does.
    other = engine.add_track()
    engine.set_track_gain(ret, 0.0)
    engine.set_track_send(ret, other, 0.5, pre_fader=True)
    assert level(engine) == pytest.approx(0.1875, abs=1e-4)


def test_a_muted_track_sends_nothing(engine, dc_wav):
    track = clip_track(engine, dc_wav)
    post, pre = engine.add_track(), engine.add_track()
    engine.set_track_send(track, post, 1.0)
    engine.set_track_send(track, pre, 1.0, pre_fader=True)
    assert level(engine) == pytest.approx(1.5)
    engine.set_track_mute(track, True)
    assert level(engine) == pytest.approx(0.0)  # pre-fader sends too


def test_cycles_are_refused_across_sends_outputs_and_returns(engine, dc_wav):
    group = engine.add_track()
    track = clip_track(engine, dc_wav, output=group)
    a, b = engine.add_track(), engine.add_track()  # returns
    engine.set_track_send(group, a, 1.0)
    engine.set_track_send(a, b, 1.0)
    for frm, to in ((b, a), (a, a), (b, group), (a, track), (b, track), (track, track)):
        with pytest.raises(ValueError):
            engine.set_track_send(frm, to, 1.0)
    with pytest.raises(ValueError):
        engine.set_track_output(b, a)  # a sends to b
    with pytest.raises(ValueError):
        engine.set_track_output(a, track)  # the track goes (through its group) into a
    with pytest.raises(ValueError):
        engine.set_track_send(track, ge.MASTER, 1.0)
    with pytest.raises(ValueError):
        engine.set_track_send(track, 999, 1.0)
    engine.set_track_send(track, b, 0.5)  # a second way into b is no cycle
    assert level(engine) == pytest.approx(0.5 + 0.5 + 0.5 + 0.25)  # group, a, b (from a and the track)


def test_removing_a_return_removes_the_sends_to_it(engine, dc_wav):
    track = clip_track(engine, dc_wav)
    ret, other = engine.add_track(), engine.add_track()
    engine.set_track_send(track, ret, 1.0)
    engine.set_track_send(track, other, 0.5)
    engine.remove_track(ret)
    assert [s.track_id for s in engine.track_sends(track)] == [other]
    assert level(engine) == pytest.approx(0.75)


def test_solo_and_mute_across_sends(engine, make_wav):
    wavs = [make_wav(np.full((SAMPLE_RATE, 2), value)) for value in (0.5, 0.25, 0.0625)]
    group = engine.add_track()
    a = clip_track(engine, wavs[0], output=group)  # 0.5, in the group
    b = clip_track(engine, wavs[1])  # 0.25
    d = clip_track(engine, wavs[2])  # 0.0625, sends nothing
    ret, other = engine.add_track(), engine.add_track()
    engine.set_track_send(group, ret, 1.0)
    engine.set_track_send(b, ret, 1.0)
    engine.set_track_send(b, other, 0.5)
    engine.set_track_send(ret, other, 0.5)
    # group 0.5, b 0.25, d 0.0625, ret 0.75, other 0.125 (from b) + 0.375 (from ret)
    assert level(engine) == pytest.approx(2.0625)

    def solo(*tracks) -> float:
        for track in (group, a, b, d, ret, other):
            engine.set_track_solo(track, track in tracks)
        return level(engine)

    # Solo in place: a soloed track is heard with everything it goes into.
    assert solo(a) == pytest.approx(0.5 + 0.5 + 0.25)  # its group, ret (from the group), other (from ret)
    assert solo(b) == pytest.approx(0.25 + 0.25 + 0.125 + 0.125)  # itself, ret, other (from it and from ret)
    assert solo(d) == pytest.approx(0.0625)
    # A soloed return keeps what sends to it sending, but nothing else of theirs.
    assert solo(ret) == pytest.approx(0.75 + 0.375)  # ret, and other (from ret)
    assert solo(other) == pytest.approx(0.5)  # from b and from ret (which hears the group and b)
    assert solo(ret, d) == pytest.approx(0.75 + 0.375 + 0.0625)
    # Mute: a muted track sends nothing, even into a soloed return.
    engine.set_track_mute(a, True)
    assert solo(ret) == pytest.approx(0.25 + 0.125)
    engine.set_track_mute(a, False)
    engine.set_track_mute(ret, True)
    assert solo(a) == pytest.approx(0.5)  # into the group; the return it reaches is muted
    assert solo() == pytest.approx(0.5 + 0.25 + 0.0625 + 0.125)


@needs_plugins
def test_compensation_across_sends(engine, uids, click_wav):
    """Every click lands on beat 1 at the master, whatever latency sits on the
    returns, the tracks sending, or what they send through."""
    track = clip_track(engine, click_wav, start_beat=1.0)
    clip_track(engine, click_wav, start_beat=1.0)  # beside it, straight into the master
    near, far = engine.add_track(), engine.add_track()  # returns
    engine.set_track_send(track, near, 1.0)
    engine.set_track_send(track, far, 1.0)

    def check(expected: float) -> None:
        out = engine.render_offline(0.0, 2 * SPB)
        assert clicks(out) == [SPB]
        assert out[SPB, 0] == pytest.approx(expected, abs=1e-4)

    check(1.0)
    latent_effect(engine, uids, far, 300)  # a latent return: the rest wait for it
    check(1.0)
    latent_effect(engine, uids, near, 50)  # one track into two returns of different latency
    check(1.0)
    latent_effect(engine, uids, track, 120)  # a latent track sending
    check(1.0)
    engine.set_track_send(near, far, 1.0)  # a return into another (with more latency)
    check(1.25)
    latent_effect(engine, uids, ge.MASTER, 40)
    check(1.25)
    group = engine.add_track()  # the track goes on through a group of its own
    latent_effect(engine, uids, group, 70)
    engine.set_track_output(track, group)
    check(1.25)


@needs_plugins
def test_a_pre_fader_tap_after_a_latent_device(engine, uids, click_wav):
    track = clip_track(engine, click_wav, start_beat=1.0)
    ret = engine.add_track()
    latent_effect(engine, uids, track, 200)
    engine.set_track_send(track, ret, 1.0, pre_fader=True)
    engine.set_track_gain(track, 0.5)
    clip_track(engine, click_wav, start_beat=1.0)  # beside them, delayed to line up
    out = engine.render_offline(0.0, 2 * SPB)
    assert clicks(out) == [SPB]
    assert out[SPB, 0] == pytest.approx(0.125 + 0.25 + 0.25, abs=1e-4)


@needs_plugins
def test_send_automation_plays_in_time(engine, uids, make_wav, click_wav):
    """A send's level is applied where the return hears it: as late as the
    return hears its inputs."""
    track = clip_track(engine, make_wav(np.full((2 * SAMPLE_RATE, 2), 0.5)), duration_sec=2.0)
    ret = engine.add_track()
    latent_effect(engine, uids, track, 100)
    latent_effect(engine, uids, ret, 50)
    late = clip_track(engine, click_wav)
    latent_effect(engine, uids, late, 400)  # so that the return's edge to the master is delayed too
    engine.set_track_gain(track, 0.0)  # only the send is heard
    engine.set_track_send(track, ret, 1.0, pre_fader=True)
    step = SPB + 400
    engine.set_track_automation(track, [ge.AutomationLane(0, f"send:{ret}", [
        ge.AutomationPoint(0.0, 0.0), ge.AutomationPoint(step / SPB, 0.0), ge.AutomationPoint(step / SPB, 1.0)])])
    out = engine.render_offline(0.0, step + 2000)[:, 0]
    assert np.abs(out[step - 300 : step]).max() < 1e-6  # silent right up to the step
    assert out[step + 40] > 0.3  # and loud from it (+6 dB at the top of the lane)
    # Without its envelope, the send's own level counts again.
    engine.set_track_automation(track, [])
    assert engine.render_offline(0.0, 4000)[-1, 0] == pytest.approx(0.5)


def test_the_meters_of_a_return(engine, dc_wav):
    track = clip_track(engine, dc_wav)
    ret = engine.add_track()
    engine.set_track_send(track, ret, 1.0)
    assert {m.track_id for m in engine.take_meters()} >= {ge.MASTER, ret}


def test_ranks_follow_the_longest_path_through_sends():
    """A node going to several places ranks by the heaviest of them."""
    # 0 -> 2 (a group) and 3 (a return); 1 -> 3; 2 -> master; 3 -> 4 (a return) -> master
    destinations = [[2, 3], [3], [], [4], []]
    costs = [1.0, 1.0, 1.0, 2.0, 5.0]
    order, ranks = ge.task_graph_order(destinations, costs)
    assert ranks == pytest.approx([8.0, 8.0, 1.0, 7.0, 5.0])
    assert order == [0, 1]
    with pytest.raises(ValueError):
        ge.task_graph_order([[1], [0]], [1.0, 1.0])
