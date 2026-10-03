"""Live playback with workers, through the fake ASIO driver in manual mode: the
MIDI input, recording and resampling tests again, with silent tracks beside theirs (each
with a device) so that every buffer's tracks are shared out among threads.
Live, held, recorded and preview notes and recorded audio behave as on one
thread. Skipped without ASIO, like the tests they repeat."""

import numpy as np
import pytest

from substation import _engine as ge

from .conftest import TEST_PLUGINS
from .test_asio import open_asio
from .test_midi_input import (  # noqa: F401 - the fixtures and the tests, again (with the engine below)
    NOTE_ON,
    heard,
    held,
    keys,
    send,
    synth,
    test_a_note_released_as_it_is_played_still_sounds,
    test_held_keys_are_released,
    test_live_notes_reach_the_instrument_at_their_offsets,
    test_midi_and_audio_record_together,
    test_monitoring,
    test_recorded_notes_land_on_the_beats_they_were_played,
    test_which_tracks_hear_which_input,
)
from .test_recording import (  # noqa: F401
    BUFFER,
    RATE,
    driver,
    pytestmark,
    test_auto_monitoring_while_recording,
    test_count_in,
    test_loopback_take_lines_up_with_the_timeline,
    test_monitoring_through_a_latent_plugin_is_not_delayed_by_compensation,
    test_what_ends_a_recording,
)
from .test_resampling_engine import (  # noqa: F401
    test_a_resampled_take_equals_its_sources_render,
    test_monitoring_a_track_is_not_delayed,
    test_solo_across_a_monitored_input,
)

THREADS = 4
BESIDE = 7  # silent tracks with a device: enough work to share at this buffer size


@pytest.fixture
def engine(driver):
    e = ge.Engine()
    e.set_clip_fade_ms(0)
    e.audio_threads = THREADS
    for _ in range(BESIDE):
        track = e.add_track()
        e.add_builtin_processor(e.track_chain(track), "utility")
    yield e
    e.stop_recording()
    e.close_device()
    assert driver.get("instances") == 0, "the driver was not released"


def test_the_workers_render_live(engine, driver, keys):
    send(engine, [NOTE_ON, 60, 127], 0)
    np.testing.assert_allclose(heard(driver, 50)[-BUFFER:], held((0, BUFFER, 127)), atol=1e-6)
    assert engine.nodes_on_workers > 0


def test_preview_notes(engine, driver):
    """Preview notes (the piano roll's keys) reach the instrument at the start of
    the next buffer, and every one of them ends, also several in one buffer."""
    open_asio(engine, sample_rate=RATE, buffer_frames=BUFFER)
    track = engine.add_track()
    synth(engine, track)  # DC: each note adds its velocity
    driver.process(2)
    engine.preview_note(track, 60, 127)
    np.testing.assert_allclose(heard(driver), held((0, BUFFER, 127)), atol=1e-6)
    for key in range(61, 73):  # a note dragged across keys: release one, play the next
        engine.preview_note(track, key - 1, 0)
        engine.preview_note(track, key, 64)
    np.testing.assert_allclose(heard(driver), held((0, BUFFER, 64)), atol=1e-6)
    engine.preview_note(track, 72, 0)
    assert np.all(heard(driver, 2) == 0.0)
    engine.preview_note(track, 50, 127)
    engine.play()  # notes started by hand go on when the transport starts
    np.testing.assert_allclose(heard(driver), held((0, BUFFER, 127)), atol=1e-6)
    engine.stop()
    engine.preview_note(track, 50, 0)
    assert np.all(heard(driver) == 0.0)


def test_sends_ramp_live(engine, driver, dc_wav):
    """Live, a send's level and solo change in a ramp (as a fader's), whichever
    threads render the tracks."""
    open_asio(engine, sample_rate=RATE, buffer_frames=BUFFER)
    engine.load_source(dc_wav)
    track = engine.add_track()
    engine.set_track_clips(track, [ge.ClipDesc(dc_wav, 0.0, 1.0)])
    ret = engine.add_track()
    engine.set_track_send(track, ret, 0.5)
    engine.play()
    ramp = int(0.02 * RATE) // BUFFER + 2  # the 20 ms ramp, and then some
    assert heard(driver, ramp)[-1] == pytest.approx(0.75)

    def glides_to(value: float) -> None:
        out = heard(driver, ramp)
        assert out[-1] == pytest.approx(value)
        assert np.abs(np.diff(out)).max() < 0.01  # no jump
    engine.set_track_send(track, ret, 1.0)
    glides_to(1.0)
    engine.set_track_solo(ret, True)  # the track goes on sending, but not into the master
    glides_to(0.5)
    engine.set_track_mute(track, True)
    glides_to(0.0)
    assert engine.nodes_on_workers > 0


@pytest.mark.skipif(not TEST_PLUGINS.exists(), reason="test plug-ins not built")
def test_a_sidechain_lines_up_live(engine, driver, make_wav):
    """Live, with the source on one thread and the device's track perhaps on
    another, a sidechain still lines up at its device: the source's click and
    the track's (and its device's copy of the source's) fall on one sample."""
    plugins = str(TEST_PLUGINS)
    uids = {d.name: d.uid for d in ge.scan_vst3(plugins)}
    open_asio(engine, sample_rate=RATE, buffer_frames=BUFFER)
    click = np.zeros(1000)
    click[0] = 0.25
    path = make_wav(click)
    engine.load_source(path)
    source, track = engine.add_track(), engine.add_track()
    for t in (source, track):
        engine.set_track_clips(t, [ge.ClipDesc(path, 0.25, 1000 / RATE)])
    latent = engine.add_plugin_processor(engine.track_chain(source), "VST3", plugins, uids["SUB Test Effect"])
    engine.set_processor_param(latent, 1, 300)  # Latency
    engine.idle()
    keyed = engine.add_plugin_processor(engine.track_chain(track), "VST3", plugins, uids["SUB Test Sidechain"])
    engine.set_processor_sidechain(keyed, source)  # it comes 300 samples late: the track waits for it
    engine.play()
    out = heard(driver, (RATE // 8 + 300) // BUFFER + 8)
    loud = np.nonzero(np.abs(out) > 1e-7)[0]
    assert len(loud) == 1 and out[loud[0]] == pytest.approx(0.75)
    assert engine.nodes_on_workers > 0
