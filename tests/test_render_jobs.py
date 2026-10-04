"""Renders in the background (RenderJob): an export or a track's render on a
thread of its own, with its progress, cancelled (its file gone), one at a
time, rendering the project as it was when it started."""

import time

import numpy as np
import pytest

from substation import _engine as ge

from .conftest import SAMPLE_RATE

SPB = SAMPLE_RATE // 2  # samples per beat at 120 BPM


@pytest.fixture
def engine():
    e = ge.Engine()
    e.set_clip_fade_ms(0)
    yield e
    e.close_device()


def clip_track(engine, path, duration_sec=1.0) -> int:
    engine.load_source(path)
    track = engine.add_track()
    engine.set_track_clips(track, [ge.ClipDesc(path, 0.0, duration_sec)])
    return track


def wait_done(job, timeout=20.0) -> list[float]:
    """The progress seen until the job is done."""
    seen = []
    deadline = time.monotonic() + timeout
    while not job.done:
        assert time.monotonic() < deadline, "the render never ended"
        seen.append(job.progress)
        time.sleep(0.001)
    return seen


def test_an_export_in_the_background_is_the_same_export(engine, ramp_wav, tmp_path):
    clip_track(engine, ramp_wav)
    engine.export_wav(str(tmp_path / "now.wav"), 0.0, 2.0, bit_depth=24)

    job = engine.start_export(str(tmp_path / "later.wav"), 0.0, 2.0, bit_depth=24)
    assert job.path == str(tmp_path / "later.wav") and engine.is_rendering
    seen = wait_done(job)
    assert seen == sorted(seen) and job.progress == 1.0
    assert engine.is_rendering  # until it is finished
    assert job.finish() == 2 * SPB
    assert not engine.is_rendering
    assert (tmp_path / "later.wav").read_bytes() == (tmp_path / "now.wav").read_bytes()
    assert job.finish() == 2 * SPB  # (again: the same)


def test_a_track_rendered_in_the_background_is_the_same_render(engine, dc_wav, tmp_path):
    track = clip_track(engine, dc_wav, duration_sec=0.5)
    frames = engine.render_track_to_wav(track, str(tmp_path / "now.wav"), 0.0, 2.0, 1.0)
    job = engine.start_track_render(track, str(tmp_path / "later.wav"), 0.0, 2.0, tail_seconds=1.0)
    wait_done(job)
    assert job.finish() == frames == 2 * SPB  # (no tail: silent after the clip)
    assert (tmp_path / "later.wav").read_bytes() == (tmp_path / "now.wav").read_bytes()


def test_a_cancelled_render_leaves_no_file(engine, dc_wav, tmp_path):
    clip_track(engine, dc_wav)
    target = tmp_path / "long.wav"
    job = engine.start_export(str(target), 0.0, 2 * 60 * 60 * 2.0, bit_depth=16)  # two hours
    assert target.exists()  # created at once
    job.cancel()
    assert job.cancelled
    assert job.finish() is None
    assert not target.exists() and not engine.is_rendering
    # The engine renders again as before.
    assert engine.render_offline(0.0, SPB)[100, 0] == pytest.approx(0.5)

    track = engine.add_track()
    job = engine.start_track_render(track, str(tmp_path / "track.wav"), 0.0, 2 * 60 * 60 * 2.0)
    job.cancel()
    assert job.finish() is None and not (tmp_path / "track.wav").exists()


def test_one_render_at_a_time_and_what_waits_for_it(engine, dc_wav, tmp_path):
    clip_track(engine, dc_wav)
    job = engine.start_export(str(tmp_path / "a.wav"), 0.0, 2.0)
    with pytest.raises(RuntimeError, match="Wait for the render"):
        engine.start_export(str(tmp_path / "b.wav"), 0.0, 2.0)
    assert not (tmp_path / "b.wav").exists()
    with pytest.raises(RuntimeError, match="Wait for the render"):
        engine.render_offline(0.0, 100)
    with pytest.raises(RuntimeError, match="Wait for the render"):
        engine.audio_threads = 1 if engine.audio_threads != 1 else 2
    engine.take_meters()  # what the UI polls doesn't wait for it
    engine.idle()
    assert job.finish() == 2 * SPB
    engine.audio_threads = 1
    assert engine.render_offline(0.0, 100)[50, 0] == pytest.approx(0.5)


def test_a_render_hears_the_project_as_it_was_when_it_started(engine, dc_wav, tmp_path):
    track = clip_track(engine, dc_wav)
    utility = engine.add_builtin_processor(engine.track_chain(track), "utility")
    job = engine.start_export(str(tmp_path / "mix.wav"), 0.0, 2.0, bit_depth=32)
    engine.set_track_clips(track, [])  # changes meanwhile are taken...
    engine.remove_processor(utility)
    engine.remove_track(track)
    engine.idle()
    assert job.finish() == 2 * SPB  # ...but not heard in it
    exported = np.array(ge.Engine().load_source(str(tmp_path / "mix.wav")).samples(0, 2 * SPB))
    assert exported.shape == (2, 2 * SPB) and np.all(exported == pytest.approx(0.5))
    assert engine.render_offline(0.0, 100)[50, 0] == 0.0


def test_a_render_that_cant_start_says_why(engine, tmp_path):
    with pytest.raises(RuntimeError, match="Could not create"):
        engine.start_export(str(tmp_path / "missing folder" / "mix.wav"), 0.0, 1.0)
    with pytest.raises(ValueError):
        engine.start_export(str(tmp_path / "mix.wav"), 1.0, 1.0)
    with pytest.raises(ValueError):
        engine.start_track_render(12345, str(tmp_path / "track.wav"), 0.0, 1.0)
    assert not engine.is_rendering and not list(tmp_path.iterdir())


def test_a_render_let_go_of_unfinished_gives_the_engine_back(engine, tmp_path):
    job = engine.start_export(str(tmp_path / "mix.wav"), 0.0, 2 * 60 * 60 * 2.0)
    del job
    assert not engine.is_rendering and not (tmp_path / "mix.wav").exists()
    # And an engine going while its render runs stops it first.
    other = ge.Engine()
    job = other.start_export(str(tmp_path / "other.wav"), 0.0, 2 * 60 * 60 * 2.0)
    del other
    assert job.finish() is None and not (tmp_path / "other.wav").exists()
