"""Freezing in the engine: one track's signal before its fader rendered offline
(lined up with the timeline, solo ignored, into a WAV file with its tail), and
frozen tracks playing their clips through their faders without their devices,
notes or what goes into them; what goes only into frozen tracks isn't rendered."""

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


def freeze(engine, track, path, end_beat, tail_seconds=0.0):
    """Renders the track to `path` and plays that instead, frozen (as the app does)."""
    frames = engine.render_track_to_wav(track, str(path), 0.0, end_beat, tail_seconds)
    engine.load_source(str(path))
    engine.set_track_clips(track, [ge.ClipDesc(str(path), 0.0, frames / SAMPLE_RATE)])
    engine.set_track_notes(track, [])
    engine.set_track_frozen(track, True)
    return frames


def clicks(out) -> list[int]:
    return np.nonzero(np.abs(out[:, 0]) > 1e-6)[0].tolist()


@pytest.fixture
def click_wav(make_wav):
    click = np.zeros(SAMPLE_RATE)
    click[1000] = 0.25
    return make_wav(click)


# --- Rendering one track ------------------------------------------------------------


def test_a_track_renders_after_its_devices_before_its_fader(engine, make_wav):
    track = clip_track(engine, make_wav(np.full((SAMPLE_RATE, 2), 0.5)))
    utility(engine, track, -6.0206)  # halves it
    engine.set_track_gain(track, 0.1)
    engine.set_track_pan(track, 1.0)
    engine.set_track_mute(track, True)
    out = engine.render_track_offline(track, 0.0, 4000)
    assert out.shape == (4000, 2)
    np.testing.assert_allclose(out[-100:], 0.25, atol=1e-4)  # (once the utility's gain has settled)


def test_a_group_renders_its_bus(engine, make_wav):
    bus = engine.add_track()
    clip_track(engine, make_wav(np.full((SAMPLE_RATE, 2), 0.5)), output=bus)
    quiet = clip_track(engine, make_wav(np.full((SAMPLE_RATE, 2), 0.25)), output=bus)
    engine.set_track_gain(quiet, 0.5)  # what goes into the bus: after the tracks' faders
    utility(engine, bus, -6.0206)
    np.testing.assert_allclose(engine.render_track_offline(bus, 0.0, 4000)[-100:], (0.5 + 0.125) / 2, atol=1e-4)


def test_solo_is_ignored(engine, make_wav):
    bus = engine.add_track()
    clip_track(engine, make_wav(np.full((SAMPLE_RATE, 2), 0.5)), output=bus)
    other = clip_track(engine, make_wav(np.full((SAMPLE_RATE, 2), 0.25)))
    engine.set_track_solo(other, True)  # would leave the group silent
    np.testing.assert_allclose(engine.render_track_offline(bus, 0.0, 100), 0.5, atol=1e-4)
    np.testing.assert_allclose(engine.render_offline(0.0, 100), 0.25, atol=1e-4)  # (playing, it counts)


def test_an_unknown_track_is_refused(engine, tmp_path):
    with pytest.raises(ValueError):
        engine.render_track_offline(99, 0.0, 10)
    with pytest.raises(ValueError):
        engine.render_track_to_wav(engine.add_track(), str(tmp_path / "x.wav"), 1.0, 1.0)


@needs_plugins
def test_the_render_lines_up_with_the_timeline(engine, uids, click_wav):
    track = clip_track(engine, click_wav)
    latent_effect(engine, uids, track, 300)
    assert clicks(engine.render_track_offline(track, 0.0, 4000)) == [1000]


@needs_plugins
def test_a_group_render_lines_up_with_the_timeline(engine, uids, click_wav):
    bus = engine.add_track()
    late = clip_track(engine, click_wav, output=bus)
    latent_effect(engine, uids, late, 200)  # the bus hears its tracks 200 late
    latent_effect(engine, uids, bus, 100)
    assert clicks(engine.render_track_offline(bus, 0.0, 4000)) == [1000]


def test_the_wav_has_the_range_and_its_tail(engine, make_wav, tmp_path):
    tone = np.zeros((SAMPLE_RATE, 2))
    tone[: SAMPLE_RATE // 2] = 0.5  # half a second of sound in a one-second clip
    track = clip_track(engine, make_wav(tone))
    path = tmp_path / "frozen.wav"
    frames = engine.render_track_to_wav(track, str(path), 0.0, 0.5, tail_seconds=2.0)  # a quarter second, then on
    assert frames == SAMPLE_RATE // 2  # the tail, until it fell silent
    source = engine.load_source(str(path))
    assert source.frames == frames and source.channels == 2
    samples = source.samples(0, frames)
    np.testing.assert_allclose(samples, 0.5, atol=1e-4)


def test_the_wav_keeps_what_is_louder_than_full_scale(engine, make_wav, tmp_path):
    track = clip_track(engine, make_wav(np.full((SAMPLE_RATE, 2), 0.5)))
    utility(engine, track, 12.0)  # about 2.0
    path = tmp_path / "loud.wav"
    engine.render_track_to_wav(track, str(path), 0.0, 1.0)
    assert engine.load_source(str(path)).samples(4000, 100).max() > 1.5  # 32-bit float: not clipped


# --- Frozen tracks -----------------------------------------------------------------


def test_a_frozen_track_plays_its_render_through_its_fader(engine, make_wav, tmp_path):
    track = clip_track(engine, make_wav(np.full((SAMPLE_RATE, 2), 0.5)))
    effect = utility(engine, track, -6.0206)
    engine.set_track_gain(track, 0.5)
    engine.render_offline(0.0, 4000)  # (a new utility glides to its gain once)
    before = engine.render_offline(0.0, SAMPLE_RATE // 2)
    freeze(engine, track, tmp_path / "f.wav", 1.0)
    assert engine.track_frozen(track)
    np.testing.assert_allclose(engine.render_offline(0.0, SAMPLE_RATE // 2), before, atol=1e-6)
    # Its devices are left out (it plays what they made); its fader stays live.
    engine.set_processor_param(effect, engine.processor_param_index(effect, "gain"), 0.0)
    np.testing.assert_allclose(engine.render_offline(0.0, 4000)[-100:], 0.125, atol=1e-4)
    engine.set_track_gain(track, 1.0)
    np.testing.assert_allclose(engine.render_offline(0.0, 4000)[-100:], 0.25, atol=1e-4)
    engine.set_track_frozen(track, False)  # its devices again (on what it plays now)
    np.testing.assert_allclose(engine.render_offline(0.0, 4000)[-100:], 0.25, atol=1e-4)


def test_a_frozen_midi_track_plays_no_notes(engine):
    track = engine.add_track()
    engine.add_builtin_processor(engine.track_chain(track), "synth")
    engine.set_track_notes(track, [ge.NoteDesc(0.0, 1.0, 60, 100)])
    assert np.abs(engine.render_offline(0.0, SPB)).max() > 0.01
    engine.set_track_clips(track, [])
    engine.set_track_frozen(track, True)  # without a render: nothing at all
    assert np.abs(engine.render_offline(0.0, SPB)).max() == 0.0


def test_a_frozen_group_doesnt_hear_its_tracks(engine, make_wav, tmp_path):
    bus = engine.add_track()
    child = clip_track(engine, make_wav(np.full((SAMPLE_RATE, 2), 0.5)), output=bus)
    utility(engine, bus, -6.0206)
    freeze(engine, bus, tmp_path / "bus.wav", 1.0)
    np.testing.assert_allclose(engine.render_offline(0.0, 4000)[-100:], 0.25, atol=1e-4)
    engine.set_track_gain(child, 0.0)  # inside the frozen audio
    np.testing.assert_allclose(engine.render_offline(0.0, 4000)[-100:], 0.25, atol=1e-4)
    engine.set_track_gain(bus, 0.5)  # live
    np.testing.assert_allclose(engine.render_offline(0.0, 4000)[-100:], 0.125, atol=1e-4)


def test_what_goes_only_into_frozen_tracks_isnt_rendered(engine, make_wav, tmp_path):
    bus = engine.add_track()
    inner = engine.add_track()
    engine.set_track_output(inner, bus)
    child = clip_track(engine, make_wav(np.full((SAMPLE_RATE, 2), 0.5)), output=inner)
    utility(engine, child, 0.0)
    ret = engine.add_track()
    sender = clip_track(engine, make_wav(np.full((SAMPLE_RATE, 2), 0.25)), output=bus)
    engine.set_track_send(sender, ret, 1.0)  # also heard elsewhere: rendered
    freeze(engine, bus, tmp_path / "bus.wav", 1.0)
    # The bus (both tracks, frozen), and the send into the return, live.
    np.testing.assert_allclose(engine.render_offline(0.0, 100), 0.75 + 0.25, atol=1e-4)
    engine.set_track_gain(sender, 0.0)
    np.testing.assert_allclose(engine.render_offline(0.0, 100), 0.75, atol=1e-4)
    engine.set_track_frozen(bus, False)  # all of it again
    np.testing.assert_allclose(engine.render_offline(0.0, 100), 0.5 + 0.75, atol=1e-4)


def test_sends_tap_the_frozen_audio(engine, make_wav, tmp_path):
    track = clip_track(engine, make_wav(np.full((SAMPLE_RATE, 2), 0.5)))
    utility(engine, track, -6.0206)
    ret = engine.add_track()
    engine.set_track_output(track, ret)  # (all of it through the return)
    pre = engine.add_track()
    engine.set_track_send(track, pre, 1.0, pre_fader=True)
    freeze(engine, track, tmp_path / "t.wav", 1.0)
    engine.set_track_gain(track, 0.5)
    # Into the return after the fader (0.125), the pre-fader send before it (0.25).
    np.testing.assert_allclose(engine.render_offline(0.0, 4000)[-100:], 0.125 + 0.25, atol=1e-4)


@needs_plugins
def test_a_frozen_track_adds_no_latency(engine, uids, click_wav, tmp_path):
    # Were its devices' latency still counted, everything else would wait for
    # it, and its frozen audio (on time) would come that much early.
    track = clip_track(engine, click_wav)
    latent_effect(engine, uids, track, 300)
    clip_track(engine, click_wav, start_beat=1.0)
    assert clicks(engine.render_offline(0.0, SPB + 4000)) == [1000, SPB + 1000]
    freeze(engine, track, tmp_path / "t.wav", 1.0)
    assert clicks(engine.render_offline(0.0, SPB + 4000)) == [1000, SPB + 1000]


@needs_plugins
def test_a_frozen_group_lines_up_with_nothing_inside_it(engine, uids, click_wav, tmp_path):
    # Its bus doesn't hear its tracks, so it isn't as late as they are.
    bus = engine.add_track()
    child = clip_track(engine, click_wav, output=bus)
    latent_effect(engine, uids, child, 500)  # would make everything else 500 late
    clip_track(engine, click_wav, start_beat=1.0)
    freeze(engine, bus, tmp_path / "bus.wav", 1.0)
    out = engine.render_offline(0.0, SPB + 4000)
    assert clicks(out) == [1000, SPB + 1000]
