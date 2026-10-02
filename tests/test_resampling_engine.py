"""Resampling in the engine: a track taking its input from another track's
output (or a group's, or a return's), or from the master's, with the fake ASIO
driver in manual mode. A take of a track equals that track's render sample for
sample, wherever latent plug-ins are; a monitored track hears its source
without delay compensation, never the master; cycles are refused. Skipped
without ASIO, like the recording tests."""

import numpy as np
import pytest

from gilstudio import _engine as ge

from .test_asio import open_asio
from .test_recording import (  # noqa: F401 - skipped without ASIO
    BUFFER,
    RATE,
    SPB,
    driver,
    latent_effect,
    output,
    pytestmark,
    read_take,
)


@pytest.fixture
def engine(driver):
    e = ge.Engine()
    e.set_clip_fade_ms(0)
    yield e
    e.stop_recording()
    e.close_device()
    assert driver.get("instances") == 0, "the driver was not released"


def heard(driver, buffers=1) -> np.ndarray:
    """The left output of the next buffers."""
    driver.ClearOutput()
    driver.process(buffers)
    return output(driver, 0)


RAMP = int(0.02 * RATE) // BUFFER + 2  # buffers for a live fader's (or solo's) 20 ms ramp, and then some


def open_asio_plain(engine):
    open_asio(engine, sample_rate=RATE, buffer_frames=BUFFER)


@pytest.mark.parametrize("case", ["track", "latent track", "group", "master"])
def test_a_resampled_take_equals_its_sources_render(engine, driver, make_wav, tmp_path, case):
    """Recorded from a track (after its fader), a group or the master, a take
    holds what that source renders, placed where it was heard: as late as it
    leaves its source, not by any device latency, nor delayed with the edges
    that line up at the master."""
    rng = np.random.default_rng(11)
    signal = np.round(rng.uniform(-0.5, 0.5, (RATE, 2)) * 32768) / 32768
    wav = make_wav(signal)
    open_asio_plain(engine)
    engine.load_source(wav)
    source = engine.add_track()
    engine.set_track_clips(source, [ge.ClipDesc(wav, 0.5, 1.0)])
    engine.set_track_gain(source, 0.5)  # after the fader
    slow = engine.add_track()  # everything else is delayed to line up with it
    latent_effect(engine, slow, 100)
    recorder = engine.add_track()
    arrival = 0
    if case == "latent track":
        latent_effect(engine, source, 333)
        arrival = 333
    elif case == "group":
        group = engine.add_track()
        engine.set_track_output(source, group)
        latent_effect(engine, source, 50)
        latent_effect(engine, group, 333)
        engine.set_track_gain(group, 0.8)
        source, arrival = group, 50 + 333
    elif case == "master":
        latent_effect(engine, ge.MASTER, 333)
        engine.set_track_gain(ge.MASTER, 0.8)
        source, arrival = ge.MASTER, 100 + 333  # the master hears the tracks 100 late
    engine.set_track_input_track(recorder, source)
    # The master can't be heard (it would feed back); a track could, but would go into the master too.
    engine.set_track_monitor(recorder, ge.MonitorMode.IN if case == "master" else ge.MonitorMode.OFF)

    engine.start_recording([(recorder, str(tmp_path / "take.wav"))])
    driver.process(8 * RATE // BUFFER // 4)  # two seconds
    progress = engine.recording_progress()
    take = engine.stop_recording()[0]
    assert (take.track_id, take.channels, take.dropped_frames, take.error) == (recorder, 2, 0, "")
    assert take.start_sample == -arrival == progress[0].start_sample
    recorded = read_take(engine, take)

    engine.stop()
    if source != ge.MASTER:
        engine.set_track_solo(source, True)  # the source alone (a group: with what is in it)
    rendered = engine.render_offline(0.0, take.frames + take.start_sample).T
    expected = np.zeros_like(recorded)
    expected[:, -take.start_sample:] = rendered
    np.testing.assert_allclose(recorded, expected, atol=1e-6)
    assert np.abs(recorded).max() > 0.15


def test_monitoring_a_track_is_not_delayed(engine, driver, dc_wav):
    """A monitored track hears its source's output instead of its clips, as soon
    as the source has it: not delayed to line up with a slower track."""
    open_asio_plain(engine)
    engine.load_source(dc_wav)
    source = engine.add_track()
    engine.set_track_clips(source, [ge.ClipDesc(dc_wav, 0.0, 1.0)])
    slow = engine.add_track()
    latent_effect(engine, slow, 300)  # the source reaches the master 300 late...
    listener = engine.add_track()
    engine.set_track_clips(listener, [ge.ClipDesc(dc_wav, 0.0, 1.0, 0.0, 0.5)])  # (0.25)
    engine.set_track_input_track(listener, source)
    engine.set_track_monitor(listener, ge.MonitorMode.OFF)
    engine.play()
    out = heard(driver, 4)
    np.testing.assert_allclose(out[:300], 0.0, atol=1e-6)
    np.testing.assert_allclose(out[300:], 0.75, atol=1e-6)  # its clip, lined up

    engine.stop()
    engine.position_beats = 0.0
    engine.set_track_monitor(listener, ge.MonitorMode.IN)
    driver.process(2)
    engine.play()
    out = heard(driver, 4)  # ...but the listener hears it at once
    np.testing.assert_allclose(out[:300], 0.5, atol=1e-6)
    np.testing.assert_allclose(out[300:], 1.0, atol=1e-6)

    engine.set_track_monitor(listener, ge.MonitorMode.AUTO)
    engine.set_track_armed(listener, True)
    assert heard(driver, 3)[-1] == pytest.approx(0.75)  # playing back without recording: its clip
    engine.stop()
    assert heard(driver, 3)[-1] == pytest.approx(0.0)  # stopped: hears the source (silent now)


def test_monitoring_the_master_is_impossible(engine, driver, dc_wav):
    open_asio_plain(engine)
    engine.load_source(dc_wav)
    source = engine.add_track()
    engine.set_track_clips(source, [ge.ClipDesc(dc_wav, 0.0, 1.0)])
    resampler = engine.add_track()
    engine.set_track_input_track(resampler, ge.MASTER)
    engine.set_track_monitor(resampler, ge.MonitorMode.IN)
    engine.play()
    np.testing.assert_allclose(heard(driver, 4), 0.5, atol=1e-6)  # no feedback


def test_solo_across_a_monitored_input(engine, driver, dc_wav):
    """Monitored, an input edge carries solo like any edge: soloing the source
    keeps the track hearing it; soloing the track keeps its source going into
    it (but not into the master). Unmonitored, it carries nothing."""
    open_asio_plain(engine)
    engine.load_source(dc_wav)
    source = engine.add_track()
    engine.set_track_clips(source, [ge.ClipDesc(dc_wav, 0.0, 1.0)])  # 0.5
    listener = engine.add_track()
    engine.set_track_clips(listener, [ge.ClipDesc(dc_wav, 0.0, 1.0, 0.0, 0.5)])  # 0.25
    engine.set_track_input_track(listener, source)
    engine.play()

    def level() -> float:
        return float(heard(driver, RAMP)[-1])
    engine.set_track_monitor(listener, ge.MonitorMode.OFF)
    assert level() == pytest.approx(0.75)
    engine.set_track_solo(source, True)
    assert level() == pytest.approx(0.5)
    engine.set_track_monitor(listener, ge.MonitorMode.IN)
    assert level() == pytest.approx(1.0)  # it hears the soloed source
    engine.set_track_solo(source, False)
    engine.set_track_solo(listener, True)
    assert level() == pytest.approx(0.5)  # only through the listener
    engine.set_track_monitor(listener, ge.MonitorMode.OFF)
    assert level() == pytest.approx(0.25)  # its clip; the source is silenced


def test_cycles_are_refused():
    engine = ge.Engine()
    a, b, group, ret = (engine.add_track() for _ in range(4))
    with pytest.raises(ValueError):
        engine.set_track_input_track(a, a)
    engine.set_track_output(a, group)
    with pytest.raises(ValueError, match="feeds"):
        engine.set_track_input_track(a, group)  # its own group
    engine.set_track_input_track(b, a)
    with pytest.raises(ValueError):
        engine.set_track_input_track(a, b)
    with pytest.raises(ValueError):
        engine.set_track_output(b, a)  # b takes its input from a
    engine.set_track_input_track(b, ret)
    with pytest.raises(ValueError):
        engine.set_track_send(b, ret, 1.0)
    engine.set_track_send(a, ret, 1.0)  # b takes ret's output; a feeding ret is fine
    engine.set_track_input_track(b, ge.MASTER)  # the master: always (it renders after every track)
    assert engine.track_input_track(b) == ge.MASTER
    engine.set_track_input_track(group, ge.MASTER)
    with pytest.raises(ValueError):
        engine.set_track_input_track(ge.MASTER, a)
    with pytest.raises(ValueError):
        engine.set_track_input_track(a, 999)
    engine.set_track_input(b, [0])  # back to the device: the edge from the master goes (it was none)
    engine.set_track_input_track(b, ret)
    engine.set_track_input(b, [0])  # and the one from ret
    assert engine.track_input_track(b) is None
    engine.set_track_send(b, ret, 1.0)


def test_the_input_goes_with_its_source(engine, driver, dc_wav, tmp_path):
    open_asio_plain(engine)
    source, listener = engine.add_track(), engine.add_track()
    engine.set_track_input_track(listener, source)
    assert engine.track_input_track(listener) == source
    engine.remove_track(source)
    assert engine.track_input_track(listener) is None
    with pytest.raises(ValueError, match="no input"):
        engine.start_recording([(listener, str(tmp_path / "take.wav"))])
    engine.set_track_input_track(listener, ge.MASTER)
    engine.start_recording([(listener, str(tmp_path / "take.wav"))])
    driver.process(2)
    take = engine.stop_recording()[0]
    assert take.frames == 2 * BUFFER and take.channels == 2


def test_a_source_removed_while_recording_leaves_silence(engine, driver, dc_wav, tmp_path):
    open_asio_plain(engine)
    engine.load_source(dc_wav)
    source, listener = engine.add_track(), engine.add_track()
    engine.set_track_clips(source, [ge.ClipDesc(dc_wav, 0.0, 1.0)])
    engine.set_track_input_track(listener, source)
    engine.start_recording([(listener, str(tmp_path / "take.wav"))])
    driver.process(2)
    engine.remove_track(source)
    driver.process(2)
    take = engine.stop_recording()[0]
    samples = read_take(engine, take)
    assert take.frames == 4 * BUFFER
    np.testing.assert_allclose(samples[:, :2 * BUFFER], 0.5, atol=1e-6)
    np.testing.assert_allclose(samples[:, 2 * BUFFER:], 0.0, atol=1e-6)


def test_device_and_resampled_takes_together(engine, driver, dc_wav, tmp_path):
    """Each take is placed by its own input: device input by the device's
    latencies and the output's lag, a track's output by its arrival."""
    driver.SetInputLevel(0, 0.25)
    open_asio(engine, sample_rate=RATE, buffer_frames=BUFFER, input_channels=[0])
    engine.load_source(dc_wav)
    source = engine.add_track()
    engine.set_track_clips(source, [ge.ClipDesc(dc_wav, 0.0, 1.0)])
    latent_effect(engine, source, 200)
    mic, resampler = engine.add_track(), engine.add_track()
    engine.set_track_input(mic, [0])
    engine.set_track_input_track(resampler, source)
    for track in (mic, resampler):
        engine.set_track_monitor(track, ge.MonitorMode.OFF)
    engine.position_beats = 1.0
    engine.start_recording([(mic, str(tmp_path / "mic.wav")), (resampler, str(tmp_path / "res.wav"))])
    driver.process(4)
    takes = {t.track_id: t for t in engine.stop_recording()}
    assert takes[mic].start_sample == SPB - (2 * BUFFER + 32 + 64 + 200)  # the driver's latencies, the lag
    assert takes[resampler].start_sample == SPB - 200
    assert takes[mic].channels == 1 and takes[resampler].channels == 2
    assert takes[mic].frames == takes[resampler].frames == 4 * BUFFER
