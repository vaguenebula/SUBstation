"""The built-in Delay: synced and free times, offset, link, feedback, ping pong, freeze, the filter and the modes."""

import numpy as np
import pytest

from substation import _engine as ge

from .conftest import SAMPLE_RATE

CLICK = 100  # where the click is in the clip
SIXTEENTH = SAMPLE_RATE * 60 // 120 // 4  # at the engine's 120 BPM


@pytest.fixture
def engine():
    e = ge.Engine()
    e.set_clip_fade_ms(0)
    yield e
    e.close_device()


def click_track(engine, make_wav, left=1.0, right=1.0, seconds=3.0, at=CLICK):
    samples = np.zeros((int(seconds * SAMPLE_RATE), 2), np.float32)
    samples[at] = (left, right)
    path = make_wav(samples)
    engine.load_source(path)
    track = engine.add_track()
    engine.set_track_clips(track, [ge.ClipDesc(path, 0.0, seconds, 0.0, 1.0)])
    return track


def delay(engine, track, **values):
    values = {"feedback": 0.0, "filter": 0.0, "mix": 100.0, **values}
    device = engine.add_builtin_processor(engine.track_chain(track), "delay")
    for name, value in values.items():
        engine.set_processor_param(device, engine.processor_param_index(device, name), value)
    return device


def echoes(channel, threshold=0.05):
    """Where the channel's echoes are (the first sample of each)."""
    above = np.flatnonzero(np.abs(channel) > threshold)
    return [int(i) for i in above[np.insert(np.diff(above) > 8, 0, True)]]


def test_listed_with_its_parameters():
    delay_info = next(d for d in ge.builtin_devices() if d.id == "delay")
    assert delay_info.name == "Delay"
    ids = [p.id for p in delay_info.params]
    assert ids[:4] == ["l_sync", "l_division", "l_time", "l_offset"]
    assert {"link", "feedback", "freeze", "filter", "freq", "width", "mode",
            "ping_pong", "mix"} <= set(ids)


def test_synced_times_follow_the_tempo(engine, make_wav):
    track = click_track(engine, make_wav)
    delay(engine, track, l_division=2.0, r_division=3.0)  # 3 and 4 sixteenths
    out = engine.render_offline(0.0, SAMPLE_RATE)
    # Fully wet: the click itself is gone, its echo comes 3 (left) and 4 (right) sixteenths later.
    assert echoes(out[:, 0])[0] == pytest.approx(CLICK + 3 * SIXTEENTH, abs=2)
    assert echoes(out[:, 1])[0] == pytest.approx(CLICK + 4 * SIXTEENTH, abs=2)
    assert np.abs(out[:, 0]).max() == pytest.approx(1.0, abs=0.05)


def test_offset_free_time_and_link(engine, make_wav):
    track = click_track(engine, make_wav)
    device = delay(engine, track, l_division=3.0, l_offset=25.0, r_sync=0.0, r_time=100.0)
    out = engine.render_offline(0.0, SAMPLE_RATE)
    assert echoes(out[:, 0])[0] == pytest.approx(CLICK + 1.25 * 4 * SIXTEENTH, abs=2)
    assert echoes(out[:, 1])[0] == pytest.approx(CLICK + SAMPLE_RATE // 10, abs=2)

    engine.set_processor_param(device, engine.processor_param_index(device, "link"), 1.0)
    engine.set_processor_param(device, engine.processor_param_index(device, "mode"), 2.0)  # Jump: no glide
    out = engine.render_offline(0.0, SAMPLE_RATE)
    assert echoes(out[:, 1])[0] == echoes(out[:, 0])[0]  # the right side follows the left


def test_feedback_repeats(engine, make_wav):
    track = click_track(engine, make_wav)
    delay(engine, track, l_sync=0.0, l_time=100.0, r_sync=0.0, r_time=100.0, feedback=50.0)
    out = engine.render_offline(0.0, SAMPLE_RATE)
    step = SAMPLE_RATE // 10
    for n in range(1, 4):
        at = CLICK + n * step
        assert out[at - 2:at + 3, 0].max() == pytest.approx(0.5 ** (n - 1), rel=0.02)


def test_ping_pong_bounces(engine, make_wav):
    track = click_track(engine, make_wav, left=1.0, right=0.0)
    delay(engine, track, l_sync=0.0, l_time=100.0, r_sync=0.0, r_time=50.0, feedback=50.0, ping_pong=1.0)
    out = engine.render_offline(0.0, SAMPLE_RATE // 2)
    left, right = echoes(out[:, 0], 0.01), echoes(out[:, 1], 0.01)
    step_l, step_r = SAMPLE_RATE // 10, SAMPLE_RATE // 20
    # The input, summed to mono, goes left first, then bounces right, then left again.
    assert left[0] == pytest.approx(CLICK + step_l, abs=2)
    assert right[0] == pytest.approx(CLICK + step_l + step_r, abs=2)
    assert left[1] == pytest.approx(CLICK + 2 * step_l + step_r, abs=2)
    assert np.abs(out[:, 0]).max() == pytest.approx(0.5, rel=0.02)  # half of the click, on each side


def test_freeze_holds_the_loop_and_ignores_new_input(engine, make_wav):
    track = click_track(engine, make_wav)
    device = delay(engine, track, l_sync=0.0, l_time=100.0, r_sync=0.0, r_time=100.0)
    engine.set_processor_param(device, engine.processor_param_index(device, "freeze"), 1.0)
    out = engine.render_offline(0.0, SAMPLE_RATE)
    assert not np.any(np.abs(out) > 0.01)  # frozen before the click came: it never went in

    engine.set_processor_param(device, engine.processor_param_index(device, "freeze"), 0.0)
    engine.set_processor_param(device, engine.processor_param_index(device, "feedback"), 10.0)
    first = engine.render_offline(0.0, SAMPLE_RATE // 4)  # the click goes in, and its first echo comes
    assert np.abs(first[:, 0]).max() == pytest.approx(1.0, abs=0.05)


def test_filter_takes_out_what_is_outside_its_band(engine, make_wav):
    t = np.arange(2 * SAMPLE_RATE) / SAMPLE_RATE
    low = 0.5 * np.sin(2 * np.pi * 60 * t)
    path = make_wav(np.stack([low, low], axis=1))
    engine.load_source(path)
    track = engine.add_track()
    engine.set_track_clips(track, [ge.ClipDesc(path, 0.0, 2.0, 0.0, 1.0)])
    device = delay(engine, track, l_sync=0.0, l_time=10.0, r_sync=0.0, r_time=10.0)
    unfiltered = np.abs(engine.render_offline(0.0, SAMPLE_RATE)[SAMPLE_RATE // 2:, 0]).max()
    for name, value in (("filter", 1.0), ("freq", 4000.0), ("width", 2.0)):  # 2 to 8 kHz
        engine.set_processor_param(device, engine.processor_param_index(device, name), value)
    filtered = np.abs(engine.render_offline(0.0, SAMPLE_RATE)[SAMPLE_RATE // 2:, 0]).max()
    assert unfiltered == pytest.approx(0.5, rel=0.02)
    assert filtered < 0.01


@pytest.mark.parametrize("mode, exact", [(0.0, False), (1.0, True), (2.0, True)])  # Repitch, Fade, Jump
def test_time_changes_glide_only_in_repitch(engine, make_wav, mode, exact):
    """The time jumps from 10 to 200 ms at beat 0.5; a click comes 0.375 s later. Fade (done fading) and Jump
    echo it at 200 ms; Repitch is still gliding there."""
    click = 30000
    track = click_track(engine, make_wav, seconds=2.0, at=click)
    device = delay(engine, track, l_sync=0.0, l_time=10.0, r_sync=0.0, r_time=10.0, mode=mode)
    info = engine.processor_params(device)[engine.processor_param_index(device, "l_time")]
    before, after = info.to_normalized(10.0), info.to_normalized(200.0)
    engine.set_track_automation(track, [ge.AutomationLane(device, "l_time", [
        ge.AutomationPoint(0.0, before), ge.AutomationPoint(0.5, before), ge.AutomationPoint(0.5, after)])])
    out = engine.render_offline(0.0, SAMPLE_RATE)
    at = echoes(out[:, 0])[0]
    assert (abs(at - (click + SAMPLE_RATE // 5)) <= 2) == exact


def test_mono_and_silence_stay_finite(engine, make_wav):
    track = click_track(engine, make_wav)
    delay(engine, track, feedback=95.0, filter=1.0, width=0.5)
    out = engine.render_offline(0.0, 2 * SAMPLE_RATE)
    assert np.all(np.isfinite(out)) and np.abs(out).max() < 2.0


def test_input_display_is_the_input_in_mono(engine, make_wav):
    track = click_track(engine, make_wav, left=1.0, right=0.5)
    device = delay(engine, track)
    assert [(d.id, d.samples_per_value) for d in engine.processor_displays(device)] == [("input", 1)]
    engine.render_offline(0.0, 4096)
    values, position = engine.read_processor_display(device, 0)
    assert position == 4096 and values[CLICK] == pytest.approx(0.75, abs=1e-3) and np.count_nonzero(values) == 1
