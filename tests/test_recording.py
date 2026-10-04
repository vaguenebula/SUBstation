"""Audio input and recording in the engine, with the fake ASIO driver
(tests/asio_driver) in manual mode: track inputs, input monitoring, recorded
takes and where they land on the timeline, count-in, and what ends a
recording.

The loopback tests patch the driver's first output back into an input, as a
cable would, delayed by the latencies the driver reports: what the engine plays
comes back as the input it records, so a take can be held sample by sample to
the arrangement it was recorded against."""

import numpy as np
import pytest

from substation import _engine as ge

from .conftest import TEST_PLUGINS
from .test_asio import (  # noqa: F401 - skipped without ASIO
    FLOAT32_LSB,
    Driver,
    decode,
    open_asio,
    pytestmark,
)

RATE = 48000
SPB = RATE // 2  # samples per beat at 120 BPM
BUFFER = 256
FX_LATENCY = 1  # SUB Test Effect's latency parameter


@pytest.fixture
def driver():
    d = Driver()
    d.SetManual(1)
    d.SetSampleType(FLOAT32_LSB)  # the cable carries floats exactly
    return d


@pytest.fixture
def engine(driver):
    e = ge.Engine()
    e.set_clip_fade_ms(0)
    yield e
    e.stop_recording()
    e.close_device()
    assert driver.get("instances") == 0, "the driver was not released"


def output(driver, channel=0) -> np.ndarray:
    return decode(driver.output(channel), FLOAT32_LSB)


def read_take(engine, take) -> np.ndarray:
    """A take's samples as (channels, frames), decoded by the engine."""
    source = engine.load_source(take.path)
    assert (source.frames, source.channels, source.sample_rate) == (take.frames, take.channels, RATE)
    return np.array(source.samples(0, source.frames))


def latent_effect(engine, track, latency):
    if not TEST_PLUGINS.exists():
        pytest.skip("test plug-ins not built")
    uid = next(d.uid for d in ge.scan_vst3(str(TEST_PLUGINS)) if d.name == "SUB Test Effect")
    effect = engine.add_plugin_processor(engine.track_chain(track), "VST3", str(TEST_PLUGINS), uid)
    engine.set_processor_param(effect, FX_LATENCY, latency)
    engine.idle()  # the plug-in asked for a restart to change its latency
    assert engine.processor_info(effect).latency == latency
    return effect


# --- Monitoring ---------------------------------------------------------------------


def test_monitoring_modes(engine, driver):
    driver.SetInputLevel(1, 0.25)
    driver.SetInputLevel(2, -0.5)
    open_asio(engine, sample_rate=RATE, buffer_frames=BUFFER, input_channels=[1, 2])
    track = engine.add_track()

    def heard(buffers=2):
        driver.ClearOutput()
        driver.process(buffers)
        return output(driver, 0)[-BUFFER:], output(driver, 1)[-BUFFER:]

    assert np.all(heard()[0] == 0.0)  # no input
    engine.set_track_input(track, [1, 2])  # a stereo pair
    engine.set_track_monitor(track, ge.MonitorMode.IN)
    left, right = heard()
    np.testing.assert_allclose(left, 0.25, atol=1e-6)
    np.testing.assert_allclose(right, -0.5, atol=1e-6)
    engine.set_track_input(track, [2])  # mono: both sides
    left, right = heard()
    np.testing.assert_allclose([left, right], -0.5, atol=1e-6)

    engine.set_track_monitor(track, ge.MonitorMode.OFF)
    assert np.all(heard()[0] == 0.0)
    engine.set_track_monitor(track, ge.MonitorMode.AUTO)
    assert np.all(heard()[0] == 0.0)  # not armed
    engine.set_track_armed(track, True)
    np.testing.assert_allclose(heard()[0], -0.5, atol=1e-6)  # armed, stopped
    engine.play()
    assert np.all(heard()[0] == 0.0)  # playing back, not recording: its clips
    engine.stop()
    np.testing.assert_allclose(heard()[0], -0.5, atol=1e-6)

    engine.set_track_input(track, [3])  # not open: silent
    assert np.all(heard()[0] == 0.0)
    engine.set_track_input(track, [])
    assert np.all(heard()[0] == 0.0)
    with pytest.raises(ValueError):
        engine.set_track_input(track, [0, 1, 2])
    with pytest.raises(ValueError):
        engine.set_track_input(ge.MASTER, [0])


def test_monitoring_through_a_latent_plugin_is_not_delayed_by_compensation(engine, driver):
    driver.SetInputLevel(0, 0.5)
    open_asio(engine, sample_rate=RATE, buffer_frames=BUFFER, input_channels=[0])
    slow = engine.add_track()
    latent_effect(engine, slow, 300)  # every other track waits for this one...
    monitored = engine.add_track()
    latent_effect(engine, monitored, 100)
    engine.set_track_input(monitored, [0])
    engine.set_track_monitor(monitored, ge.MonitorMode.IN)
    driver.ClearOutput()
    driver.process(4)
    out = output(driver, 0)
    # ...but a monitored track only waits for its own devices.
    assert np.all(out[:100] == 0.0)
    np.testing.assert_allclose(out[100:], 0.5, atol=1e-6)


# --- Recording ----------------------------------------------------------------------


@pytest.mark.parametrize("latency", [None, "track", "master"])
def test_loopback_take_lines_up_with_the_timeline(engine, driver, make_wav, latency):
    """The engine plays a clip; the cable brings it back; the take, placed where
    the engine says, holds exactly what the timeline played there."""
    rng = np.random.default_rng(7)
    signal = np.round(rng.uniform(-0.5, 0.5, RATE) * 32768) / 32768  # exact in 16 bits
    wav = make_wav(signal)
    driver.SetLoopback(0, 0)
    driver.SetLatencies(40, 90)
    open_asio(engine, sample_rate=RATE, buffer_frames=BUFFER, input_channels=[0])
    player = engine.add_track()
    engine.load_source(wav)
    engine.set_track_clips(player, [ge.ClipDesc(wav, 0.5, 1.0)])
    other = engine.add_track()  # delayed to line up with a latent player
    engine.set_track_clips(other, [ge.ClipDesc(wav, 0.5, 1.0, 0.0, 0.0)])
    if latency == "track":
        latent_effect(engine, player, 333)
    elif latency == "master":
        latent_effect(engine, ge.MASTER, 333)
    recorder = engine.add_track()
    engine.set_track_input(recorder, [0])
    engine.set_track_monitor(recorder, ge.MonitorMode.OFF)  # or it would play its own input into the cable
    engine.set_track_armed(recorder, True)

    path = str(make_wav(np.zeros(1))).replace(".wav", "-take.wav")
    engine.start_recording([(recorder, path)])
    assert engine.is_recording and engine.is_playing
    driver.process(8 * RATE // BUFFER // 4)  # two seconds
    progress = engine.recording_progress()
    assert [p.track_id for p in progress] == [recorder] and progress[0].started
    takes = engine.stop_recording()
    assert not engine.is_recording and engine.is_playing  # recording ends; playing goes on
    assert len(takes) == 1
    take = takes[0]
    assert (take.track_id, take.path, take.channels, take.sample_rate) == (recorder, path, 1, RATE)
    assert take.dropped_frames == 0 and take.error == ""
    assert take.frames >= 2 * RATE - BUFFER
    assert progress[0].start_sample == take.start_sample
    lag = (2 * BUFFER + 40 + 90) + (333 if latency else 0)
    assert take.start_sample == -lag  # it began with the playhead at 0, heard that much later

    recorded = read_take(engine, take)[0]
    timeline = np.zeros(take.frames + lag + RATE)
    timeline[SPB // 2:SPB // 2 + RATE] = signal  # what the master played (the other track is silent)
    positions = np.arange(take.frames) + take.start_sample
    expected = np.where(positions >= 0, timeline[np.clip(positions, 0, None)], 0.0)
    np.testing.assert_allclose(recorded, expected, atol=1e-6)
    assert np.abs(recorded).max() > 0.4


def test_stereo_take_and_its_live_peaks(engine, driver, tmp_path):
    driver.SetInputLevel(2, 0.25)
    driver.SetInputLevel(3, -0.75)
    open_asio(engine, sample_rate=RATE, buffer_frames=BUFFER, input_channels=[2, 3])
    track = engine.add_track()
    engine.set_track_input(track, [2, 3])
    engine.start_recording([(track, str(tmp_path / "rec" / "stereo.wav"))])  # makes its folder
    driver.process(10)
    progress = engine.recording_progress()[0]
    assert progress.frames == 10 * BUFFER
    assert progress.peaks.shape == (10 * BUFFER // ge.RECORD_PEAK_FRAMES, 2)
    np.testing.assert_allclose(progress.peaks, [[-0.75, 0.25]] * len(progress.peaks), atol=1e-6)
    assert engine.recording_progress()[0].peaks.shape == (0, 2)  # since the last call
    take = engine.stop_recording()[0]
    samples = read_take(engine, take)
    assert take.channels == 2 and take.frames == 10 * BUFFER
    np.testing.assert_allclose(samples[0], 0.25, atol=1e-6)
    np.testing.assert_allclose(samples[1], -0.75, atol=1e-6)


def test_auto_monitoring_while_recording(engine, driver, tmp_path):
    driver.SetInputLevel(0, 0.5)
    open_asio(engine, sample_rate=RATE, buffer_frames=BUFFER, input_channels=[0])
    track = engine.add_track()
    engine.set_track_input(track, [0])
    engine.set_track_armed(track, True)  # Auto
    engine.play()
    driver.ClearOutput()
    driver.process(2)
    assert np.all(output(driver)[-BUFFER:] == 0.0)  # playing back
    engine.start_recording([(track, str(tmp_path / "take.wav"))])  # punch in
    driver.ClearOutput()
    driver.process(2)
    np.testing.assert_allclose(output(driver)[-BUFFER:], 0.5, atol=1e-6)
    take = engine.stop_recording()[0]
    assert take.start_sample == 2 * BUFFER - (2 * BUFFER + 32 + 64)  # where the playhead was, heard


def test_a_track_being_recorded_plays_none_of_its_clips(engine, driver, tmp_path, make_wav):
    """Its take replaces them: unmonitored, the track is silent while it records."""
    driver.SetInputLevel(0, 0.25)
    open_asio(engine, sample_rate=RATE, buffer_frames=BUFFER, input_channels=[0])
    wav = make_wav(np.full(4 * RATE, 0.5))
    engine.load_source(wav)
    track = engine.add_track()
    engine.set_track_clips(track, [ge.ClipDesc(wav, 0.0, 4.0)])
    engine.set_track_input(track, [0])
    engine.set_track_monitor(track, ge.MonitorMode.OFF)
    engine.set_track_armed(track, True)

    def heard():
        driver.ClearOutput()
        driver.process(3)
        return output(driver)[-BUFFER:]

    engine.play()
    np.testing.assert_allclose(heard(), 0.5, atol=1e-6)  # playing back: its clip
    engine.start_recording([(track, str(tmp_path / "take.wav"))])  # punch in
    assert np.all(heard() == 0.0)  # neither its clip nor its input
    engine.stop_recording()
    np.testing.assert_allclose(heard(), 0.5, atol=1e-6)  # punched out: its clip again

    engine.set_track_monitor(track, ge.MonitorMode.IN)  # monitored: its input, as before
    engine.start_recording([(track, str(tmp_path / "take2.wav"))])
    np.testing.assert_allclose(heard(), 0.25, atol=1e-6)
    engine.stop_recording()


def test_count_in(engine, driver, tmp_path):
    driver.SetInputLevel(0, 0.5)
    open_asio(engine, sample_rate=RATE, buffer_frames=BUFFER, input_channels=[0])
    track = engine.add_track()
    engine.set_track_input(track, [0])
    engine.set_track_monitor(track, ge.MonitorMode.OFF)
    engine.position_beats = 2.0
    engine.start_recording([(track, str(tmp_path / "take.wav"))], count_in_beats=4.0)
    driver.ClearOutput()
    driver.process(1)
    assert engine.is_counting_in and engine.position_beats == 2.0
    assert engine.recording_progress()[0].frames == 0
    count_in = 4 * SPB
    driver.process(count_in // BUFFER + 4)
    assert not engine.is_counting_in
    moved = (count_in // BUFFER + 5) * BUFFER - count_in  # samples played since it ended
    assert engine.position_beats == pytest.approx(2.0 + moved / SPB)
    take = engine.stop_recording()[0]
    assert take.frames == moved
    assert take.start_sample == 2 * SPB - (2 * BUFFER + 32 + 64)
    # It clicks on every beat of the count-in (the metronome is off), then stops.
    out = np.abs(output(driver))
    for beat in range(4):
        assert out[beat * SPB:beat * SPB + 200].max() > 0.05, beat
        assert out[beat * SPB - 2000:beat * SPB].max() < 1e-3 if beat else True
    driver.ClearOutput()
    driver.process(20)
    assert np.abs(output(driver))[3000:].max() < 1e-3  # no fifth click


def test_what_ends_a_recording(engine, driver, tmp_path):
    open_asio(engine, sample_rate=RATE, buffer_frames=BUFFER, input_channels=[0, 1])
    a, b = engine.add_track(), engine.add_track()
    engine.set_track_input(a, [0])
    engine.set_track_input(b, [1])

    # A locate: the takes end where the playhead jumped.
    engine.start_recording([(a, str(tmp_path / "a1.wav")), (b, str(tmp_path / "b1.wav"))])
    driver.process(4)
    engine.position_beats = 8.0
    driver.process(4)
    assert not engine.is_recording
    takes = engine.stop_recording()
    assert [(t.track_id, t.frames) for t in takes] == [(a, 4 * BUFFER), (b, 4 * BUFFER)]

    # Opening the device again (another buffer size or sample rate): the takes so far are kept.
    engine.start_recording([(a, str(tmp_path / "a2.wav"))])
    driver.process(3)
    open_asio(engine, sample_rate=96000, buffer_frames=BUFFER, input_channels=[0, 1])
    assert not engine.is_recording
    take = engine.stop_recording()[0]
    assert take.frames == 3 * BUFFER and take.sample_rate == RATE
    assert engine.stop_recording() == []

    # Stopped before anything came in: no take, no file.
    engine.stop()
    engine.start_recording([(a, str(tmp_path / "a3.wav"))], count_in_beats=4.0)
    engine.stop()
    take = engine.stop_recording()[0]
    assert take.frames == 0 and not (tmp_path / "a3.wav").exists()


def test_recording_needs_an_open_input(engine, driver, tmp_path):
    track = engine.add_track()
    engine.set_track_input(track, [1])
    with pytest.raises(RuntimeError, match="No audio device"):
        engine.start_recording([(track, str(tmp_path / "x.wav"))])
    open_asio(engine, sample_rate=RATE, buffer_frames=BUFFER, input_channels=[0])
    with pytest.raises(RuntimeError, match="not open"):
        engine.start_recording([(track, str(tmp_path / "x.wav"))])
    other = engine.add_track()
    with pytest.raises(ValueError, match="no input"):
        engine.start_recording([(other, str(tmp_path / "x.wav"))])
    engine.set_track_input(track, [0])
    with pytest.raises(RuntimeError, match="Could not create"):
        engine.start_recording([(track, str(tmp_path))])  # a folder
    assert not engine.is_recording and not engine.is_playing
