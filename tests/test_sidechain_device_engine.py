"""The built-in Sidechain device: hits found sample-accurately in the key, the curve played from each,
depth, lookahead, ducking only the lows, hits on the beat, and its displays."""

import numpy as np
import pytest

from substation import _engine as ge

from .conftest import SAMPLE_RATE

HIT = 12000  # where the kick starts, in samples
LEVEL = 0.5  # dc_wav's


@pytest.fixture
def engine():
    e = ge.Engine()
    e.set_clip_fade_ms(0)
    yield e
    e.close_device()


def clip_track(engine, path, seconds=1.0):
    engine.load_source(path)
    track = engine.add_track()
    engine.set_track_clips(track, [ge.ClipDesc(path, 0.0, seconds, 0.0, 1.0)])
    return track


def kick_wav(make_wav, hits=(HIT,), seconds=1.0, length=2400):
    """Short bursts of a 60 Hz sine (a kick, as far as the key goes) starting at each of `hits`."""
    data = np.zeros((int(seconds * SAMPLE_RATE), 2), np.float32)
    burst = 0.8 * np.sin(2 * np.pi * 60.0 * np.arange(length) / SAMPLE_RATE) * np.exp(-np.arange(length) / 600.0)
    burst[0] = 0.8  # starts at full level: the hit is its first sample
    for at in hits:
        data[at:at + length] = burst[:, None]
    return make_wav(data)


def curve_values(points):
    """Parameters for a curve of (x, y, curve) points; the other slots unused."""
    values = {}
    for i in range(16):
        used = i < len(points)
        x, y, c = points[i] if used else (1.0, 1.0, 0.0)
        values.update({f"p{i + 1}_used": float(used), f"p{i + 1}_x": x, f"p{i + 1}_y": y, f"p{i + 1}_curve": c})
    return values


LINEAR = [(0.0, 0.0, 0.0), (1.0, 1.0, 0.0)]  # ducked at the hit, straight back up over the length


def hits(engine, device):
    """Where the hits came (since the render started), from the phase display: its 0s."""
    phase, position = engine.read_processor_display(device, 2)
    return [int(i) + position - len(phase) for i in np.flatnonzero(phase == 0)]


def sidechain(engine, track, key=None, points=LINEAR, **values):
    device = engine.add_builtin_processor(engine.track_chain(track), "sidechain")
    values = {"smooth": 0.0, "length": 100.0, **curve_values(points), **values}
    for name, value in values.items():
        engine.set_processor_param(device, engine.processor_param_index(device, name), float(value))
    if key is not None:
        engine.set_track_gain(key, 0.0)  # heard only through the sidechain
        engine.set_processor_sidechain(device, key, ge.SidechainTap.PRE_FADER)
    return device


def test_listed_with_its_parameters():
    info = next(d for d in ge.builtin_devices() if d.id == "sidechain")
    assert info.name == "Sidechain"
    ids = [p.id for p in info.params]
    assert ids[:12] == ["trigger", "threshold", "depth", "sync", "length", "rate", "smooth", "lookahead", "range",
                        "crossover", "autofit", "character"]
    assert len(ids) == 12 + 16 * 4 and ids[-1] == "p16_curve"
    points = [p for p in info.params if p.id.startswith("p")]
    assert all(p.hidden and not p.automatable for p in points)
    defaults = {p.id: p.default_value for p in info.params}
    assert [defaults[f"p{i}_used"] for i in (1, 2, 3, 4)] == [1.0, 1.0, 1.0, 0.0]  # the default curve: 3 points


def test_without_a_sidechain_nothing_ducks(engine, dc_wav):
    track = clip_track(engine, dc_wav)
    sidechain(engine, track)
    out = engine.render_offline(0.0, SAMPLE_RATE // 2)
    assert np.allclose(out[:, 0], LEVEL)


def test_the_curve_starts_at_the_hits_first_sample(engine, dc_wav, make_wav):
    track = clip_track(engine, dc_wav)
    key = clip_track(engine, kick_wav(make_wav))
    sidechain(engine, track, key)
    out = engine.render_offline(0.0, SAMPLE_RATE // 2)[:, 0]
    length = int(0.1 * SAMPLE_RATE)
    assert out[HIT - 1] == pytest.approx(LEVEL)
    assert out[HIT] == pytest.approx(0.0, abs=1e-6)
    # Straight back up over its length, sample by sample, then untouched.
    expected = LEVEL * np.arange(length) / length
    assert np.allclose(out[HIT:HIT + length], expected, atol=2e-5)
    assert np.allclose(out[HIT + length + 1:], LEVEL)


def test_curve_shape_depth_and_smoothing(engine, dc_wav, make_wav):
    track = clip_track(engine, dc_wav)
    key = clip_track(engine, kick_wav(make_wav))
    hold = [(0.0, 0.2, 0.0), (0.5, 0.2, 0.0), (0.5001, 1.0, 0.0), (1.0, 1.0, 0.0)]
    sidechain(engine, track, key, hold, depth=50.0)
    out = engine.render_offline(0.0, SAMPLE_RATE // 2)[:, 0]
    gain = 1 - 0.5 * (1 - 0.2)  # depth 50 %: halfway to the curve
    assert out[HIT + 100] == pytest.approx(LEVEL * gain, rel=1e-5)
    assert out[HIT + 2600] == pytest.approx(LEVEL)  # past half its length: back up at once


def test_smoothing_softens_the_jump(engine, dc_wav, make_wav):
    track = clip_track(engine, dc_wav)
    key = clip_track(engine, kick_wav(make_wav))
    sidechain(engine, track, key, smooth=5.0)
    out = engine.render_offline(0.0, SAMPLE_RATE // 2)[:, 0]
    assert 0.3 < out[HIT + 10] / LEVEL < 1.0  # on its way down, not there yet
    assert out[HIT + 48 * 30] < 0.5 * LEVEL  # (and still well ducked a few ms on)


def test_bends_as_automation_does(engine, dc_wav, make_wav):
    track = clip_track(engine, dc_wav)
    key = clip_track(engine, kick_wav(make_wav))
    sidechain(engine, track, key, [(0.0, 0.0, 0.5), (1.0, 1.0, 0.0)])
    out = engine.render_offline(0.0, SAMPLE_RATE // 2)[:, 0]
    a = -0.5 * 6.0
    middle = np.expm1(a * 0.5) / np.expm1(a)  # bent up: past halfway at half the length
    assert out[HIT + 2400] / LEVEL == pytest.approx(middle, abs=1e-3) and middle > 0.6


def test_threshold_and_rearming(engine, dc_wav, make_wav):
    track = clip_track(engine, dc_wav)
    times = (HIT, HIT + 4800, HIT + 4800 + 600)  # the third comes 12.5 ms after the second: too soon
    key = clip_track(engine, kick_wav(make_wav, times, length=480))
    device = sidechain(engine, track, key, length=20.0)
    engine.render_offline(0.0, 20000)
    assert hits(engine, device) == [HIT, HIT + 4800]

    # Above the key's level: no hits at all.
    track2 = clip_track(engine, dc_wav)
    sidechain(engine, track2, key, threshold=-1.0)
    engine.remove_track(track)
    out = engine.render_offline(0.0, SAMPLE_RATE // 2)
    assert np.allclose(out[:, 0], LEVEL)


def test_lookahead_ducks_before_the_kick(engine, dc_wav, make_wav):
    track = clip_track(engine, dc_wav)
    key = clip_track(engine, kick_wav(make_wav))
    device = sidechain(engine, track, key, lookahead=5.0)
    assert engine.processor_info(device).latency == 240
    out = engine.render_offline(0.0, SAMPLE_RATE // 2)[:, 0]
    # The mix is delayed to match: the duck starts 5 ms before the kick.
    assert out[HIT - 241] == pytest.approx(LEVEL)
    assert out[HIT - 240] == pytest.approx(0.0, abs=1e-6)


def test_lows_only_keeps_the_highs(engine, make_wav):
    t = np.arange(SAMPLE_RATE) / SAMPLE_RATE
    low, high = 0.3 * np.sin(2 * np.pi * 50 * t), 0.3 * np.sin(2 * np.pi * 5000 * t)
    track = clip_track(engine, make_wav(np.stack([low + high] * 2, axis=1).astype(np.float32)))
    key = clip_track(engine, kick_wav(make_wav))
    hold = [(0.0, 0.0, 0.0), (0.99, 0.0, 0.0), (1.0, 1.0, 0.0)]  # ducked all the way through
    sidechain(engine, track, key, hold, length=500.0, range=1.0, crossover=300.0)
    out = engine.render_offline(0.0, SAMPLE_RATE)[:, 0]

    def level(segment, freq):
        window = np.hanning(len(segment))
        spectrum = np.abs(np.fft.rfft(segment * window)) / window.sum() * 2
        return spectrum[round(freq * len(segment) / SAMPLE_RATE)]

    ducked = out[HIT + 4800:HIT + 4800 + 9600]
    assert level(ducked, 50) < 0.3 * 0.02  # the lows gone
    assert level(ducked, 5000) == pytest.approx(0.3, rel=0.05)  # the highs kept
    before = out[HIT - 9600:HIT]  # while the curve is at 1: both as they were (the bands add up flat)
    assert level(before, 50) == pytest.approx(0.3, rel=0.02) and level(before, 5000) == pytest.approx(0.3, rel=0.02)


def test_hits_on_the_beat(engine, make_wav):
    track = clip_track(engine, make_wav(np.full((2 * SAMPLE_RATE, 2), LEVEL)), seconds=2.0)
    engine.tempo = 120.0
    device = sidechain(engine, track, trigger=3.0, length=50.0)  # every 1/4: every 24000 samples
    out = engine.render_offline(0.0, 80000)[:, 0]
    for beat in range(4):
        at = beat * SAMPLE_RATE // 2
        assert out[at] == pytest.approx(0.0, abs=1e-6)
        if at:
            assert out[at - 1] == pytest.approx(LEVEL)
    assert hits(engine, device) == [72000]  # (the display keeps the latest 8192 samples)


def test_synced_length(engine, dc_wav, make_wav):
    track = clip_track(engine, dc_wav)
    key = clip_track(engine, kick_wav(make_wav))
    engine.tempo = 120.0
    sidechain(engine, track, key, sync=1.0, rate=2.0)  # 1/8 at 120: 250 ms
    out = engine.render_offline(0.0, SAMPLE_RATE // 2)[:, 0]
    assert out[HIT + 6000] / LEVEL == pytest.approx(0.5, abs=1e-3)


def test_displays(engine, dc_wav, make_wav):
    track = clip_track(engine, dc_wav)
    key = clip_track(engine, kick_wav(make_wav))
    device = sidechain(engine, track, key)
    assert [(d.id, d.samples_per_value) for d in engine.processor_displays(device)] == [
        ("key", 1), ("input", 1), ("phase", 1)]
    engine.render_offline(0.0, 16384)
    key_values, _ = engine.read_processor_display(device, 0)
    inputs, _ = engine.read_processor_display(device, 1)
    phase, _ = engine.read_processor_display(device, 2)
    assert len(key_values) == len(inputs) == len(phase) == 8192  # (the latest)
    start = 16384 - 8192
    assert key_values[HIT - start] == pytest.approx(0.8, abs=1e-3) and key_values[HIT - start - 1] == 0.0
    assert np.allclose(inputs, LEVEL)
    assert phase[HIT - start - 1] == -1 and phase[HIT - start] == 0 and phase[HIT - start + 10] == 10


def test_extremes_stay_finite(engine, make_wav):
    noise = np.random.default_rng(1).uniform(-1, 1, (SAMPLE_RATE, 2)).astype(np.float32)
    track = clip_track(engine, make_wav(noise))
    key = clip_track(engine, make_wav(noise))
    sidechain(engine, track, key, threshold=-60.0, length=10.0, range=1.0, crossover=30.0, lookahead=20.0,
              smooth=30.0)
    out = engine.render_offline(0.0, SAMPLE_RATE)
    assert np.all(np.isfinite(out)) and np.abs(out).max() < 2.0
