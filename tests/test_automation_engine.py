"""Automation in the engine: envelopes of mixer controls (tracks and master) and
of device parameters, rendered offline."""

import math

import numpy as np
import pytest

from gilstudio import _engine as ge

from .conftest import SAMPLE_RATE

SPB = SAMPLE_RATE * 60 / 120  # samples per beat at 120 BPM


@pytest.fixture
def engine():
    e = ge.Engine()
    e.set_clip_fade_ms(0)
    yield e
    e.close_device()


def dc_track(engine, path, seconds=4.0):
    engine.load_source(path)
    track = engine.add_track()
    engine.set_track_clips(track, [ge.ClipDesc(path, 0.0, seconds, 0.0, 1.0)])
    return track


@pytest.fixture
def long_dc_wav(make_wav):
    return make_wav(np.full((4 * SAMPLE_RATE, 2), 0.5))


def points(*pairs):
    return [ge.AutomationPoint(*p) for p in pairs]


def volume_gain(value: float) -> float:
    return value ** 3 * ge.MAX_VOLUME_GAIN


def test_volume_ramp_follows_the_envelope_sample_by_sample(engine, long_dc_wav):
    track = dc_track(engine, long_dc_wav)
    engine.set_track_automation(track, [ge.AutomationLane(0, "volume", points((0.0, 0.0), (2.0, 1.0)))])
    out = engine.render_offline(0.0, int(3 * SPB))
    for beat in (0.0, 0.25, 1.0, 1.5):
        i = int(beat * SPB)
        assert out[i, 0] == pytest.approx(0.5 * volume_gain(beat / 2.0), rel=1e-4, abs=1e-6)
    assert out[int(2.5 * SPB), 1] == pytest.approx(0.5 * ge.MAX_VOLUME_GAIN, rel=1e-5)  # holds the last value


def test_volume_lane_replaces_the_fader_until_it_is_removed(engine, long_dc_wav):
    track = dc_track(engine, long_dc_wav)
    engine.set_track_gain(track, 0.25)
    engine.set_track_automation(track, [ge.AutomationLane(0, "volume", points((0.0, 1.0)))])
    out = engine.render_offline(0.0, 1000)
    assert out[500, 0] == pytest.approx(0.5 * ge.MAX_VOLUME_GAIN, rel=1e-5)
    engine.set_track_automation(track, [])
    out = engine.render_offline(0.0, 1000)
    assert out[500, 0] == pytest.approx(0.125)


def test_mute_silences_an_automated_track(engine, long_dc_wav):
    track = dc_track(engine, long_dc_wav)
    engine.set_track_automation(track, [ge.AutomationLane(0, "volume", points((0.0, 1.0)))])
    engine.set_track_mute(track, True)
    assert np.all(engine.render_offline(0.0, 1000) == 0.0)


def test_pan_automation(engine, long_dc_wav):
    track = dc_track(engine, long_dc_wav)
    # Hard left at beat 0, centre from beat 1, hard right at beat 2: a step, then a ramp.
    engine.set_track_automation(track, [ge.AutomationLane(0, "pan", points((0.0, 0.0), (1.0, 0.0), (1.0, 0.5),
                                                                          (2.0, 1.0)))])
    out = engine.render_offline(0.0, int(3 * SPB))
    left = int(0.5 * SPB)
    assert out[left, 0] == pytest.approx(0.5) and out[left, 1] == pytest.approx(0.0, abs=1e-7)
    centre = int(SPB)
    assert out[centre, 0] == pytest.approx(0.5) and out[centre, 1] == pytest.approx(0.5)
    right = int(2.5 * SPB)
    assert out[right, 0] == pytest.approx(0.0, abs=1e-7) and out[right, 1] == pytest.approx(0.5)


def test_master_automation(engine, long_dc_wav):
    dc_track(engine, long_dc_wav)
    engine.set_master_gain(0.5)
    engine.set_track_automation(0, [ge.AutomationLane(0, "volume", points((0.0, 0.0), (1.0, 0.0), (1.0, 1.0)))])
    out = engine.render_offline(0.0, int(2 * SPB))
    assert np.all(out[: int(SPB)] == 0.0)
    assert out[int(SPB) + 10, 0] == pytest.approx(0.5 * ge.MAX_VOLUME_GAIN, rel=1e-5)


def test_master_pan(engine, long_dc_wav):
    dc_track(engine, long_dc_wav)
    engine.set_master_pan(-1.0)
    out = engine.render_offline(0.0, 1000)
    assert out[500, 0] == pytest.approx(0.5) and out[500, 1] == pytest.approx(0.0, abs=1e-7)


def test_curved_segment(engine, long_dc_wav):
    """A bent segment follows automationShape: bulging upward when the curve is positive."""
    track = dc_track(engine, long_dc_wav)
    curve = 0.5
    engine.set_track_automation(track, [ge.AutomationLane(0, "pan", points((0.0, 0.5, curve), (2.0, 1.0)))])
    out = engine.render_offline(0.0, int(2 * SPB))
    i = int(SPB)  # halfway
    a = -curve * ge.AUTOMATION_CURVATURE
    shaped = 0.5 + 0.5 * math.expm1(a * 0.5) / math.expm1(a)
    assert shaped > 0.75  # above the straight line
    pan = shaped * 2 - 1
    assert out[i, 0] == pytest.approx(0.5 * math.cos(pan * math.pi / 2), rel=1e-4)


def utility_gain_index(engine, pid):
    return engine.processor_param_index(pid, "gain")


def test_device_parameter_automation(engine, long_dc_wav):
    track = dc_track(engine, long_dc_wav)
    pid = engine.add_builtin_processor(track, "utility")
    info = engine.processor_params(pid)[utility_gain_index(engine, pid)]
    minus_12 = info.to_normalized(-12.0)
    assert info.from_normalized(minus_12) == pytest.approx(-12.0, abs=1e-4)
    # 0 dB for the first beat, then -12 dB.
    zero = info.to_normalized(0.0)
    engine.set_track_automation(track, [ge.AutomationLane(pid, "gain", points((0.0, zero), (1.0, zero),
                                                                              (1.0, minus_12)))])
    out = engine.render_offline(0.0, int(2 * SPB))
    assert out[int(0.5 * SPB), 0] == pytest.approx(0.5, rel=1e-4)
    assert out[int(1.5 * SPB), 0] == pytest.approx(0.5 * 10 ** (-12 / 20), rel=1e-3)  # after the device's smoothing
    # The device holds the automated value, which the UI shows.
    assert engine.processor_param(pid, utility_gain_index(engine, pid)) == pytest.approx(-12.0, abs=1e-3)


def test_automation_splits_a_builtin_device_block_where_values_change(engine, long_dc_wav):
    """A step in the middle of a block takes effect there, not at the block's start."""
    track = dc_track(engine, long_dc_wav)
    pid = engine.add_builtin_processor(track, "utility")
    info = engine.processor_params(pid)[utility_gain_index(engine, pid)]
    step = int(SPB) + 100  # well inside a block (blocks are MAX_BLOCK long from 0)
    assert step % ge.MAX_BLOCK > 200
    engine.set_track_automation(track, [ge.AutomationLane(pid, "gain", points(
        (0.0, info.to_normalized(-60.0)), (step / SPB, info.to_normalized(-60.0)),
        (step / SPB, info.to_normalized(0.0))))])
    out = engine.render_offline(0.0, step + 2000)
    assert np.abs(out[step - 200 : step, 0]).max() < 0.001  # settled at -60 dB, right up to the step
    assert out[step - 1, 0] < out[step + 10, 0] < out[step + 1500, 0]  # rising from it (smoothed)


def test_discrete_parameters_take_whole_steps(engine):
    track = engine.add_track()
    pid = engine.add_builtin_processor(track, "synth")
    wave = engine.processor_param_index(pid, "wave")
    info = engine.processor_params(pid)[wave]
    assert info.step_count == 3
    engine.set_track_automation(track, [ge.AutomationLane(pid, "wave", points((0.0, 0.55)))])
    engine.render_offline(0.0, 256)
    assert engine.processor_param(pid, wave) == 2.0  # Saw
    engine.set_track_automation(track, [ge.AutomationLane(pid, "wave", points((0.0, 0.0)))])
    engine.render_offline(0.0, 256)
    assert engine.processor_param(pid, wave) == 0.0  # Sine


def test_envelopes_of_missing_devices_or_parameters_are_ignored(engine, long_dc_wav):
    track = dc_track(engine, long_dc_wav)
    pid = engine.add_builtin_processor(track, "utility")
    engine.set_track_automation(track, [ge.AutomationLane(pid, "nonsense", points((0.0, 0.0))),
                                        ge.AutomationLane(pid + 100, "gain", points((0.0, 0.0))),
                                        ge.AutomationLane(0, "nonsense", points((0.0, 0.0)))])
    out = engine.render_offline(0.0, 1000)
    assert out[500, 0] == pytest.approx(0.5)
    engine.remove_processor(pid)  # its envelope stays, unplayed
    assert engine.render_offline(0.0, 1000)[500, 0] == pytest.approx(0.5)


def test_automation_follows_tempo(engine, long_dc_wav):
    track = dc_track(engine, long_dc_wav)
    engine.set_track_automation(track, [ge.AutomationLane(0, "volume", points((0.0, 0.0), (1.0, 0.0), (1.0, 1.0)))])
    engine.tempo = 60.0  # beat 1 is now at one second
    out = engine.render_offline(0.0, int(1.5 * SAMPLE_RATE))
    assert out[SAMPLE_RATE - 1, 0] == 0.0
    assert out[SAMPLE_RATE, 0] > 0.5


def test_normalized_mapping(engine):
    track = engine.add_track()
    synth = engine.add_builtin_processor(track, "synth")
    cutoff = engine.processor_params(synth)[engine.processor_param_index(synth, "cutoff")]
    assert cutoff.log_scale
    middle = math.sqrt(20.0 * 20000.0)  # log scale: the geometric mean is halfway
    assert cutoff.to_normalized(middle) == pytest.approx(0.5, abs=1e-5)
    assert cutoff.from_normalized(0.5) == pytest.approx(middle, rel=1e-4)
    assert cutoff.from_normalized(1.5) == pytest.approx(20000.0)
