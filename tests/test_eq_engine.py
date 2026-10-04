"""The built-in EQ: its bands' filters (as eq_response draws them), placement, output gain and gain scale."""

import numpy as np
import pytest

from substation import _engine as ge

from .conftest import SAMPLE_RATE

BELL, LOW_SHELF, LOW_CUT, HIGH_SHELF, HIGH_CUT, NOTCH, BAND_PASS, TILT_SHELF = range(8)


@pytest.fixture
def engine():
    e = ge.Engine()
    e.set_clip_fade_ms(0)
    yield e
    e.close_device()


def tone_track(engine, make_wav, freq, left=0.25, right=0.25, seconds=1.0):
    t = np.arange(int(seconds * SAMPLE_RATE)) / SAMPLE_RATE
    wave = np.sin(2 * np.pi * freq * t)
    path = make_wav(np.stack([left * wave, right * wave], axis=1).astype(np.float32))
    engine.load_source(path)
    track = engine.add_track()
    engine.set_track_clips(track, [ge.ClipDesc(path, 0.0, seconds, 0.0, 1.0)])
    return track


def eq(engine, track, *bands, **values):
    """An EQ with these bands: dicts of a band's parameters (type, freq, gain, q, slope, place, on)."""
    device = engine.add_builtin_processor(engine.track_chain(track), "eq")
    for i, band in enumerate(bands):
        values.update({f"b{i + 1}_used": 1.0, **{f"b{i + 1}_{k}": float(v) for k, v in band.items()}})
    for name, value in values.items():
        engine.set_processor_param(device, engine.processor_param_index(device, name), value)
    return device


def gain_db(out, channel=0, reference=0.25):
    """The level of the second half of a channel against the tone's, in dB."""
    rms = np.sqrt(np.mean(out[len(out) // 2:, channel] ** 2))
    return 20 * np.log10(max(rms, 1e-9) / (reference / np.sqrt(2)))


def response(kind, freq, gain, q, slope, at):
    return float(ge.eq_response(kind, freq, gain, q, slope, float(SAMPLE_RATE), np.array([float(at)]))[0])


def test_listed_with_its_parameters():
    info = next(d for d in ge.builtin_devices() if d.id == "eq")
    assert info.name == "EQ"
    ids = [p.id for p in info.params]
    assert len(ids) == 24 * 8 + 2
    assert ids[:8] == ["b1_used", "b1_on", "b1_type", "b1_freq", "b1_gain", "b1_q", "b1_slope", "b1_place"]
    assert ids[-2:] == ["output", "scale"] and "b24_place" in ids
    used = info.params[0]
    assert used.hidden and not used.automatable


def test_without_bands_it_passes_through(engine, make_wav):
    track = tone_track(engine, make_wav, 1000.0)
    eq(engine, track, {"type": BELL, "freq": 1000, "gain": 12, "on": 0})  # a band switched off does nothing
    assert gain_db(engine.render_offline(0.0, SAMPLE_RATE)) == pytest.approx(0.0, abs=0.05)


@pytest.mark.parametrize("band, tone", [
    ({"type": BELL, "freq": 1000, "gain": 12, "q": 1}, 1000),
    ({"type": BELL, "freq": 1000, "gain": 12, "q": 1}, 3000),
    ({"type": BELL, "freq": 12000, "gain": -9, "q": 4}, 12000),
    ({"type": LOW_SHELF, "freq": 200, "gain": 6, "q": 0.71, "slope": 1}, 60),
    ({"type": HIGH_SHELF, "freq": 4000, "gain": -6, "q": 0.71, "slope": 3}, 10000),
    ({"type": LOW_CUT, "freq": 1000, "q": 0.71, "slope": 3}, 500),
    ({"type": HIGH_CUT, "freq": 1000, "q": 2, "slope": 1}, 1000),
    ({"type": NOTCH, "freq": 2000, "q": 4}, 1500),
    ({"type": BAND_PASS, "freq": 2000, "q": 2}, 1000),
    ({"type": TILT_SHELF, "freq": 1000, "gain": 6, "slope": 0}, 10000),
])
def test_bands_play_as_their_curves_show(engine, make_wav, band, tone):
    track = tone_track(engine, make_wav, tone)
    eq(engine, track, band)
    expected = response(band["type"], band["freq"], band.get("gain", 0.0), band.get("q", 1.0), band.get("slope", 1),
                        tone)
    assert gain_db(engine.render_offline(0.0, SAMPLE_RATE)) == pytest.approx(expected, abs=0.15)


def test_curves_match_the_analog_filters():
    freqs = np.geomspace(20.0, 16000.0, 400)
    s = 1j * freqs / 1000.0
    analog = 20 * np.log10(np.abs((s * s + s * 10 ** (12 / 40) + 1) / (s * s + s / 10 ** (12 / 40) + 1)))
    assert np.max(np.abs(ge.eq_response(BELL, 1000.0, 12.0, 1.0, 1, 48000.0, freqs) - analog)) < 0.1
    # Butterworth: 3 dB down at the corner, and 6 dB an octave per order below it (4 octaves here).
    for slope, order in ((0, 1), (1, 2), (3, 4), (6, 8)):
        assert response(LOW_CUT, 1000.0, 0.0, 0.71, slope, 1000.0) == pytest.approx(-3.0, abs=0.1)
        assert response(LOW_CUT, 1000.0, 0.0, 0.71, slope, 62.5) == pytest.approx(-24.1 * order, abs=0.5 * order)
    assert response(BELL, 15000.0, 6.0, 1.0, 1, 15000.0) == pytest.approx(6.0, abs=0.05)  # not squeezed up high
    assert response(NOTCH, 15000.0, 0.0, 4.0, 1, 15000.0) < -40


def test_placement(engine, make_wav):
    track = tone_track(engine, make_wav, 1000.0, left=0.25, right=0.25)
    device = eq(engine, track, {"type": BELL, "freq": 1000, "gain": -12, "place": 1})  # left only
    out = engine.render_offline(0.0, SAMPLE_RATE)
    assert gain_db(out, 0) == pytest.approx(-12.0, abs=0.15) and gain_db(out, 1) == pytest.approx(0.0, abs=0.05)

    index = engine.processor_param_index(device, "b1_place")
    engine.set_processor_param(device, index, 4.0)  # side: the same on both channels has none
    out = engine.render_offline(0.0, SAMPLE_RATE)
    assert gain_db(out, 0) == pytest.approx(0.0, abs=0.05) and gain_db(out, 1) == pytest.approx(0.0, abs=0.05)
    engine.set_processor_param(device, index, 3.0)  # mid: all of it
    out = engine.render_offline(0.0, SAMPLE_RATE)
    assert gain_db(out, 0) == pytest.approx(-12.0, abs=0.15) and gain_db(out, 1) == pytest.approx(-12.0, abs=0.15)


def test_side_only_takes_the_difference(engine, make_wav):
    track = tone_track(engine, make_wav, 1000.0, left=0.25, right=-0.25)  # all side
    eq(engine, track, {"type": BELL, "freq": 1000, "gain": -12, "place": 3})  # mid: none of it
    out = engine.render_offline(0.0, SAMPLE_RATE)
    assert gain_db(out, 0) == pytest.approx(0.0, abs=0.05)


def test_output_gain_and_gain_scale(engine, make_wav):
    track = tone_track(engine, make_wav, 1000.0)
    device = eq(engine, track, {"type": BELL, "freq": 1000, "gain": 12}, output=-6.0)
    assert gain_db(engine.render_offline(0.0, SAMPLE_RATE)) == pytest.approx(6.0, abs=0.15)
    engine.set_processor_param(device, engine.processor_param_index(device, "scale"), 50.0)  # half the gain
    assert gain_db(engine.render_offline(0.0, SAMPLE_RATE)) == pytest.approx(0.0, abs=0.15)


def test_extremes_stay_finite(engine, make_wav):
    track = tone_track(engine, make_wav, 30.0)
    eq(engine, track,
       {"type": BELL, "freq": 10, "gain": 30, "q": 40},
       {"type": LOW_CUT, "freq": 22000, "q": 40, "slope": 8},
       {"type": HIGH_SHELF, "freq": 22000, "gain": 30, "q": 40, "slope": 8},
       {"type": NOTCH, "freq": 10, "q": 0.025})
    out = engine.render_offline(0.0, SAMPLE_RATE)
    assert np.all(np.isfinite(out))


def test_displays_are_the_input_and_output(engine, make_wav):
    track = tone_track(engine, make_wav, 1000.0, left=0.5, right=0.25)
    device = eq(engine, track, {"type": BELL, "freq": 1000, "gain": -6})
    assert [d.id for d in engine.processor_displays(device)] == ["input", "output"]
    engine.render_offline(0.0, 4096)
    before, position = engine.read_processor_display(device, 0)
    after, _ = engine.read_processor_display(device, 1)
    assert position == 4096
    assert np.abs(before).max() == pytest.approx(0.375, abs=0.01)  # in mono
    assert np.abs(after[2048:]).max() == pytest.approx(0.375 * 10 ** (-6 / 20), abs=0.01)
