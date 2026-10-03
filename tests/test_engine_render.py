"""Audio engine tests. They render offline, so no audio device is needed."""

import math
import wave

import numpy as np
import pytest

from substation import _engine as ge

from .conftest import SAMPLE_RATE, write_wav

SPB = SAMPLE_RATE * 60 / 120  # samples per beat at 120 BPM


@pytest.fixture
def engine():
    e = ge.Engine()
    e.set_clip_fade_ms(0)  # exact sample checks; fades have their own test
    yield e
    e.close_device()


def add_clip_track(engine, path, start_beat=0.0, duration_sec=1.0, offset_sec=0.0, gain=1.0):
    engine.load_source(path)
    track = engine.add_track()
    engine.set_track_clips(track, [ge.ClipDesc(path, start_beat, duration_sec, offset_sec, gain)])
    return track


def test_clip_lands_on_exact_sample(engine, dc_wav):
    add_clip_track(engine, dc_wav, start_beat=2.0)
    out = engine.render_offline(0.0, int(5 * SPB))
    start = int(2 * SPB)
    assert out.shape == (int(5 * SPB), 2)
    assert np.all(out[:start] == 0.0)
    assert out[start] == pytest.approx([0.5, 0.5])
    assert out[start + SAMPLE_RATE - 1] == pytest.approx([0.5, 0.5])
    assert np.all(out[start + SAMPLE_RATE :] == 0.0)


def test_clip_follows_tempo(engine, dc_wav):
    engine.tempo = 60.0  # one beat per second
    add_clip_track(engine, dc_wav, start_beat=1.0)
    out = engine.render_offline(0.0, 2 * SAMPLE_RATE + 10)
    assert out[SAMPLE_RATE - 1, 0] == 0.0
    assert out[SAMPLE_RATE, 0] == pytest.approx(0.5)


def test_offset_and_duration(engine, ramp_wav):
    add_clip_track(engine, ramp_wav, start_beat=0.0, duration_sec=0.25, offset_sec=0.5)
    out = engine.render_offline(0.0, SAMPLE_RATE)
    first = SAMPLE_RATE // 2  # the clip starts playing the source at 0.5 s
    assert out[0, 0] == pytest.approx((first % 32768) / 32768.0)
    assert out[100, 1] == pytest.approx(((first + 100) % 32768) / 32768.0)  # mono -> both channels
    assert out[SAMPLE_RATE // 4 - 1, 0] != 0.0
    assert np.all(out[SAMPLE_RATE // 4 :] == 0.0)


def test_render_offline_start_position(engine, dc_wav):
    add_clip_track(engine, dc_wav, start_beat=4.0)
    out = engine.render_offline(4.0, 100)
    assert out[0] == pytest.approx([0.5, 0.5])


def test_clip_gain_track_gain_and_master(engine, dc_wav):
    track = add_clip_track(engine, dc_wav, gain=0.5)
    engine.set_track_gain(track, 0.5)
    engine.set_master_gain(0.5)
    out = engine.render_offline(0.0, 1000)
    assert out[500] == pytest.approx([0.0625, 0.0625])


def test_pan_is_balance_with_unity_centre(engine, dc_wav):
    track = add_clip_track(engine, dc_wav)
    engine.set_track_pan(track, 1.0)
    out = engine.render_offline(0.0, 1000)
    assert out[500, 0] == pytest.approx(0.0, abs=1e-6)
    assert out[500, 1] == pytest.approx(0.5)

    engine.set_track_pan(track, -0.5)
    out = engine.render_offline(0.0, 4000)  # past the 20 ms smoothing ramp
    assert out[-1, 0] == pytest.approx(0.5)
    assert out[-1, 1] == pytest.approx(0.5 * math.cos(0.5 * math.pi / 2), rel=1e-4)


def test_mute_and_solo(engine, dc_wav, make_wav):
    other = make_wav(np.full((SAMPLE_RATE, 2), 0.25))
    a = add_clip_track(engine, dc_wav)
    b = add_clip_track(engine, other)
    assert engine.render_offline(0.0, 100)[50, 0] == pytest.approx(0.75)

    engine.set_track_mute(a, True)
    assert engine.render_offline(0.0, 4000)[-1, 0] == pytest.approx(0.25)

    engine.set_track_mute(a, False)
    engine.set_track_solo(a, True)
    assert engine.render_offline(0.0, 4000)[-1, 0] == pytest.approx(0.5)

    engine.set_track_solo(b, True)
    assert engine.render_offline(0.0, 4000)[-1, 0] == pytest.approx(0.75)


def test_loop_wraps(engine, ramp_wav):
    add_clip_track(engine, ramp_wav)
    engine.set_loop(True, 0.0, 1.0)  # one beat = 24000 samples
    loop_len = int(SPB)
    out = engine.render_offline(0.0, loop_len * 2 + 10, loop=True)
    assert out[loop_len - 1, 0] == pytest.approx(((loop_len - 1) % 32768) / 32768.0)
    assert out[loop_len + 5, 0] == pytest.approx(5 / 32768.0)  # jumped back to the loop start
    np.testing.assert_array_equal(out[:loop_len], out[loop_len : 2 * loop_len])
    # Without the loop flag (as for exports) the timeline plays straight through.
    straight = engine.render_offline(0.0, loop_len * 2)
    assert not np.array_equal(straight[:loop_len], straight[loop_len:])


def test_tempo_change_keeps_playhead_on_beat(engine):
    engine.position_beats = 8.0
    engine.tempo = 60.0
    assert engine.position_beats == pytest.approx(8.0)
    engine.tempo = 174.0
    assert engine.position_beats == pytest.approx(8.0, abs=1e-4)


def test_transport_state_without_device(engine):
    assert not engine.is_playing
    engine.play()
    assert engine.is_playing
    engine.stop()
    assert not engine.is_playing


def test_metronome_clicks_on_beats(engine):
    out = engine.render_offline(0.0, int(4 * SPB), metronome=True)
    level = np.abs(out[:, 0])
    for beat in range(4):
        onset = int(beat * SPB)
        assert level[onset : onset + 200].max() > 0.1, f"no click at beat {beat}"
        assert level[onset + 4000 : onset + int(SPB) - 10].max() < 1e-3, f"click spills after beat {beat}"
    # The downbeat is accented (higher pitch and level).
    assert level[:2000].max() > level[int(SPB) : int(SPB) + 2000].max()


def test_clip_fades(dc_wav):
    engine = ge.Engine()  # default 4 ms fades
    add_clip_track(engine, dc_wav)
    out = engine.render_offline(0.0, SAMPLE_RATE)
    fade = round(0.004 * SAMPLE_RATE)
    assert out[0, 0] == 0.0
    assert 0.0 < out[fade // 2, 0] < 0.5
    assert out[fade, 0] == pytest.approx(0.5)
    assert 0.0 < out[SAMPLE_RATE - 1, 0] < 0.01


def test_clip_waits_for_its_source(engine, dc_wav):
    track = engine.add_track()
    engine.set_track_clips(track, [ge.ClipDesc(dc_wav, 0.0, 1.0)])
    assert np.all(engine.render_offline(0.0, 1000) == 0.0)
    engine.load_source(dc_wav)
    assert engine.render_offline(0.0, 1000)[500, 0] == pytest.approx(0.5)


def test_remove_track(engine, dc_wav):
    track = add_clip_track(engine, dc_wav)
    engine.remove_track(track)
    assert np.all(engine.render_offline(0.0, 1000) == 0.0)
    with pytest.raises(ValueError):
        engine.set_track_gain(track, 1.0)


def test_utility_device(engine, dc_wav):
    track = add_clip_track(engine, dc_wav)
    device = engine.add_builtin_processor(engine.track_chain(track), "utility")
    names = [p.id for p in engine.processor_params(device)]
    assert names == ["gain", "pan", "width"]
    engine.set_processor_param(device, 0, 20 * math.log10(0.5))
    out = engine.render_offline(0.0, 4000)
    assert out[-1, 0] == pytest.approx(0.25, rel=1e-4)

    engine.set_processor_enabled(device, False)
    assert engine.render_offline(0.0, 100)[50, 0] == pytest.approx(0.5)

    engine.remove_processor(device)
    with pytest.raises(ValueError):
        engine.processor_params(device)


def test_over_the_top(engine, tmp_path):
    """Soundgoodize at 100 % brings loud and quiet audio close together; at 0 % it only passes it through."""
    rng = np.random.default_rng(7)
    noise = rng.standard_normal(SAMPLE_RATE)
    noise /= np.sqrt(np.mean(noise**2))

    def rms_db(x):
        return 20 * np.log10(np.sqrt(np.mean(x**2)))

    def render(level_db, depth):
        path = write_wav(tmp_path / f"n{level_db}.wav", noise * 10 ** (level_db / 20))
        engine.load_source(str(path))
        track = engine.add_track()
        engine.set_track_clips(track, [ge.ClipDesc(str(path), 0.0, 1.0, 0.0, 1.0)])
        device = engine.add_builtin_processor(engine.track_chain(track), "ott")
        assert [p.id for p in engine.processor_params(device)] == ["depth", "output"]
        engine.set_processor_param(device, 0, depth)
        out = engine.render_offline(0.0, SAMPLE_RATE)[SAMPLE_RATE // 4 :, 0]
        engine.remove_track(track)
        return rms_db(out)

    loud, quiet = -8.0, -45.0
    assert render(loud, 0.0) == pytest.approx(loud, abs=0.5)  # the crossovers sum flat
    assert render(quiet, 0.0) == pytest.approx(quiet, abs=0.5)
    squashed = render(loud, 100.0), render(quiet, 100.0)
    assert squashed[0] < loud - 3 and squashed[1] > quiet + 15  # down from above, up from below
    assert squashed[0] - squashed[1] < 12  # 37 dB apart went in


def test_source_peaks_and_samples(engine, ramp_wav):
    source = engine.load_source(ramp_wav)
    assert source.channels == 1
    assert source.frames == SAMPLE_RATE
    assert source.duration == pytest.approx(1.0)
    peaks = source.peaks(0)
    spp = ge.AudioSource.samples_per_peak(0)
    assert peaks.shape == (math.ceil(SAMPLE_RATE / spp), 1, 2)
    assert peaks[1, 0, 0] == pytest.approx(spp / 32768.0)
    assert peaks[1, 0, 1] == pytest.approx((2 * spp - 1) / 32768.0)
    coarse = source.peaks(source.peak_levels - 1)
    assert coarse[0, 0, 1] >= peaks[:, 0, 1][: coarse.shape[0]].min()
    raw = source.samples(10, 5)
    assert raw.shape == (1, 5)
    assert raw[0, 0] == pytest.approx(10 / 32768.0)
    assert engine.load_source(ramp_wav) is not None  # cached
    assert engine.cached_source(ramp_wav).frames == SAMPLE_RATE


def test_resamples_to_engine_rate(engine, make_wav):
    path = make_wav(np.full(22050, 0.5), sample_rate=22050)
    source = engine.load_source(path)
    assert source.sample_rate == SAMPLE_RATE
    assert source.file_sample_rate == 22050
    assert source.frames == pytest.approx(SAMPLE_RATE, abs=4)


def test_probe_file(make_wav):
    path = make_wav(np.zeros((12345, 2)), sample_rate=44100)
    info = ge.probe_file(path)
    assert (info.frames, info.channels, info.sample_rate) == (12345, 2, 44100)
    assert info.duration == pytest.approx(12345 / 44100)
    with pytest.raises(RuntimeError):
        ge.probe_file(path + ".missing")


def test_export_wav_matches_render(engine, ramp_wav, tmp_path):
    add_clip_track(engine, ramp_wav, gain=0.9)
    target = tmp_path / "mix.wav"
    engine.export_wav(str(target), 0.0, 1.0, bit_depth=24)
    with wave.open(str(target), "rb") as f:
        assert (f.getnchannels(), f.getsampwidth(), f.getframerate()) == (2, 3, SAMPLE_RATE)
        raw = np.frombuffer(f.readframes(f.getnframes()), dtype=np.uint8).reshape(-1, 3)
    ints = (raw[:, 0].astype(np.int32) | (raw[:, 1].astype(np.int32) << 8) | (raw[:, 2].astype(np.int32) << 16))
    ints = np.where(ints >= 1 << 23, ints - (1 << 24), ints).reshape(-1, 2)
    exported = ints / float(1 << 23)
    rendered = engine.render_offline(0.0, int(SPB))
    assert exported.shape == rendered.shape
    np.testing.assert_allclose(exported, rendered, atol=2.0 / (1 << 23))


def test_meters_are_empty_offline(engine, dc_wav):
    track = add_clip_track(engine, dc_wav)
    engine.render_offline(0.0, 1000)
    meters = {m.track_id: (m.left, m.right) for m in engine.take_meters()}
    assert set(meters) == {0, track}
    assert meters[track] == (0.0, 0.0)


def read_wav24(path) -> np.ndarray:
    with wave.open(str(path), "rb") as f:
        raw = np.frombuffer(f.readframes(f.getnframes()), dtype=np.uint8).reshape(-1, 3)
    ints = raw[:, 0].astype(np.int32) | (raw[:, 1].astype(np.int32) << 8) | (raw[:, 2].astype(np.int32) << 16)
    return np.where(ints >= 1 << 23, ints - (1 << 24), ints).reshape(-1, 2) / float(1 << 23)


def test_every_strip_has_its_own_chain(engine):
    a, b = engine.add_track(), engine.add_track()
    chains = [engine.track_chain(t) for t in (ge.MASTER, a, b)]
    assert len(set(chains)) == 3
    device = engine.add_builtin_processor(chains[1], "utility")
    assert engine.processor_chain(device) == chains[1]
    with pytest.raises(ValueError):
        engine.add_builtin_processor(max(chains) + 1, "utility")
    engine.remove_track(a)
    with pytest.raises(ValueError):
        engine.add_builtin_processor(chains[1], "utility")  # its chain went with it
    with pytest.raises(ValueError):
        engine.processor_chain(device)


def test_move_processor_between_chains(engine, dc_wav):
    a = add_clip_track(engine, dc_wav)
    b = add_clip_track(engine, dc_wav)
    chain_a, chain_b = engine.track_chain(a), engine.track_chain(b)
    half = engine.add_builtin_processor(chain_a, "utility")
    engine.set_processor_param(half, 0, 20 * math.log10(0.5))
    assert engine.render_offline(0.0, 4000)[-1, 0] == pytest.approx(0.75, rel=1e-4)

    # To the other track: its gain goes along.
    engine.move_processor(half, chain_b)
    assert engine.processor_chain(half) == chain_b
    engine.set_track_clips(a, [])
    assert engine.render_offline(0.0, 4000)[-1, 0] == pytest.approx(0.25, rel=1e-4)

    # To the master, and to a position in a chain.
    engine.move_processor(half, engine.track_chain(ge.MASTER))
    assert engine.render_offline(0.0, 4000)[-1, 0] == pytest.approx(0.25, rel=1e-4)
    other = engine.add_builtin_processor(chain_b, "utility")
    engine.move_processor(half, chain_b, 0)
    engine.set_chain_order(chain_b, [half, other])  # the order it has
    with pytest.raises(ValueError):
        engine.set_chain_order(chain_b, [other])
    engine.move_processor(half, chain_b, 5)  # past the end: last
    engine.set_chain_order(chain_b, [other, half])
    with pytest.raises(ValueError):
        engine.move_processor(half, chain_b + 99)
    with pytest.raises(ValueError):
        engine.move_processor(999, chain_b)


def test_an_effect_on_the_master_changes_the_export(engine, dc_wav, tmp_path):
    add_clip_track(engine, dc_wav, duration_sec=0.5)
    add_clip_track(engine, dc_wav, duration_sec=0.5)
    utility = engine.add_builtin_processor(engine.track_chain(ge.MASTER), "utility")
    engine.set_processor_param(utility, engine.processor_param_index(utility, "gain"), 20 * math.log10(0.25))
    target = tmp_path / "mix.wav"
    engine.export_wav(str(target), 0.0, 1.0, bit_depth=24)
    exported = read_wav24(target)
    # The master's effect works on the tracks' sum (1.0), after which its fader applies.
    np.testing.assert_allclose(exported[4000:], 0.25, atol=1e-5)
    rendered = engine.render_offline(0.0, int(SPB))
    np.testing.assert_allclose(exported[4000:], rendered[4000:], atol=2.0 / (1 << 23))  # past the gain's smoothing
    engine.set_master_gain(0.5)
    np.testing.assert_allclose(engine.render_offline(0.0, 8000)[4000:], 0.125, rtol=1e-4)
    engine.set_processor_enabled(utility, False)
    np.testing.assert_allclose(engine.render_offline(0.0, 100), 0.5)
    engine.remove_processor(utility)
    np.testing.assert_allclose(engine.render_offline(0.0, 100), 0.5)


def test_the_master_is_a_track_without_clips(engine, dc_wav):
    with pytest.raises(ValueError):
        engine.set_track_clips(ge.MASTER, [])
    with pytest.raises(ValueError):
        engine.set_track_notes(ge.MASTER, [])
    with pytest.raises(ValueError):
        engine.remove_track(ge.MASTER)
    add_clip_track(engine, dc_wav)
    engine.set_track_gain(ge.MASTER, 0.5)  # its mixer, as the tracks'
    engine.set_track_pan(ge.MASTER, 1.0)
    out = engine.render_offline(0.0, 100)
    assert out[50, 0] == pytest.approx(0.0, abs=1e-7) and out[50, 1] == pytest.approx(0.25)
    assert engine.take_meters()[0].track_id == ge.MASTER
