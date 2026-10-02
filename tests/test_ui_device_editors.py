"""Built-in devices' own editors in the device view (ui/device_editors), shown with the Compressor's."""

import time

import numpy as np
import pytest
from PySide6.QtTest import QTest

from gilstudio.ui.device_editors import editor_for
from gilstudio.ui.device_editors.compressor import CompressorWidget
from gilstudio.ui.device_panel import DeviceWidget, device_height

from .conftest import SAMPLE_RATE, write_wav


def test_registry():
    assert editor_for("compressor") is CompressorWidget
    assert editor_for("utility") is None  # the generic knobs


def test_compressor_editor(window, tmp_path):
    path = str(write_wav(tmp_path / "loud.wav", np.full((SAMPLE_RATE, 2), 0.5)))
    window.editor.add_clips(None, 0.0, [(path, 1.0)], track_index=0)
    track = window.project.tracks[0]
    window.selection.select_track(track.id)
    device = window.editor.add_device(track.id, "compressor")
    widget = window.devices.widgets[device.id]
    assert isinstance(widget, CompressorWidget) and isinstance(widget, DeviceWidget)
    assert widget.pages == 1 and len(widget.knobs) == 7  # every knob at once
    assert widget.minimumSizeHint().height() <= device_height(window.editor)  # fits the view

    # Its knobs edit the model as the generic ones do (undoably).
    window.editor.set_device_param(track.id, device.id, "threshold", -30.0)
    assert widget.knobs["threshold"][0].value() == pytest.approx(-30.0)
    window.undo_stack.undo()
    assert widget.knobs["threshold"][0].value() == pytest.approx(-18.0)

    # What the engine reports as it renders reaches the graph with the meters.
    QTest.qWait(50)
    window.engine.render_offline(0.0, SAMPLE_RATE // 2)
    window.devices._refresh_displays()
    reduction = (20 * np.log10(0.5) + 18.0) * 0.75  # the default threshold and ratio, at -6 dB
    assert widget.graph.history[-1] == pytest.approx(reduction, abs=0.1)
    assert widget.graph.level_in == pytest.approx(20 * np.log10(0.5), abs=0.1)
    widget.graph.grab()  # paints


def _sampler(window):
    """A MIDI track whose instrument is a Sampler, shown in the device view."""
    window.insert_midi_track()
    track = window.project.tracks[0]
    device_id = window.editor.add_device(track.id, "sampler").id
    return track, window.project.device(track.id, device_id), window.devices.widgets[device_id]


def _wait_for(condition, timeout_ms=2000):
    deadline = time.monotonic() + timeout_ms / 1000
    while not condition():
        assert time.monotonic() < deadline, "timed out"
        QTest.qWait(10)


def _press(widget, x, y=40):
    from PySide6.QtCore import QPoint, Qt

    return Qt.MouseButton.LeftButton, Qt.KeyboardModifier.NoModifier, QPoint(round(x), y)


def test_sampler_editor_loads_a_sample_undoably(window, tmp_path):
    from gilstudio.model import device_state
    from gilstudio.model.project import Note
    from gilstudio.ui.device_editors.sampler import SamplerWidget

    t = np.arange(SAMPLE_RATE) / SAMPLE_RATE
    path = str(write_wav(tmp_path / "tone.wav", 0.5 * np.sin(2 * np.pi * 440 * t)))
    track, device, widget = _sampler(window)
    def current():
        return window.project.device(track.id, device.id)

    assert isinstance(widget, SamplerWidget) and [d.kind for d in track.devices] == ["sampler"]
    assert widget.pages == 2 and set(widget.knobs) == {"root", "tune", "fine", "start", "end"}
    assert set(widget.choices) == {"loop"} and widget.knobs["root"][1].text() == "C3"
    assert widget.minimumSizeHint().height() <= device_height(window.editor)  # fits the view
    widget.view.grab()  # paints the hint to drop a sample

    widget.load_sample(path)
    assert device_state.from_model(current().state) == {"sample": path}
    assert window.undo_stack.undoText() == "Load Sample"
    window.bridge.wait_for_device_states()
    engine, engine_id = window.engine, widget.engine_id
    assert device_state.decode(engine.processor_state(engine_id)) == {"sample": path}

    window.undo_stack.undo()  # no sample again
    assert current().state is None and widget.sample_path() == ""
    window.bridge.wait_for_device_states()
    assert engine.processor_state(engine_id) == b""
    window.undo_stack.redo()
    window.bridge.wait_for_device_states()
    assert device_state.decode(engine.processor_state(engine_id)) == {"sample": path}

    # It plays what was loaded; the playhead follows the note.
    clip = window.editor.add_midi_clip(track.id, 0.0, 4.0)
    window.editor.set_clip_notes(clip, [Note(60, 0.0, 1.0, 127)], "Add Note")
    QTest.qWait(50)
    out = engine.render_offline(0.0, SAMPLE_RATE // 4)
    assert np.abs(out[1000:, 0]).max() > 0.2
    window.devices._refresh_displays()
    assert 0.2 < widget.view.playhead < 0.3  # a quarter of a second into a second

    # Its waveform comes from the engine's cache: quick, and drawn.
    _wait_for(lambda: widget.sample_source() is not None)
    widget.view.grab()


def test_sampler_markers_drop_and_saving(window, tmp_path):
    from PySide6.QtCore import QMimeData, QPointF, Qt, QUrl
    from PySide6.QtGui import QDropEvent

    from gilstudio.model import device_state
    from gilstudio.model.serialization import load_project, save_project

    path = str(write_wav(tmp_path / "dc.wav", np.full((SAMPLE_RATE, 2), 0.5)))
    track, device, widget = _sampler(window)
    def current():
        return window.project.device(track.id, device.id)


    # Dropping an audio file on the waveform loads it (other files are refused).
    mime = QMimeData()
    mime.setUrls([QUrl.fromLocalFile(str(tmp_path / "notes.txt")), QUrl.fromLocalFile(path)])
    view = widget.view
    view.dropEvent(QDropEvent(QPointF(40, 40), Qt.DropAction.CopyAction, mime, Qt.MouseButton.LeftButton,
                              Qt.KeyboardModifier.NoModifier))
    assert widget.sample_path() == path
    _wait_for(lambda: widget.sample_source() is not None)

    # Dragging the Start marker is one undoable edit; it can't pass End.
    plot = view._plot()
    QTest.mousePress(view, *_press(view, plot.left()))
    QTest.mouseMove(view, _press(view, plot.left() + plot.width() / 4)[2])
    QTest.mouseMove(view, _press(view, plot.left() + plot.width() / 2)[2])
    QTest.mouseRelease(view, *_press(view, plot.left() + plot.width() / 2))
    assert current().params["start"] == pytest.approx(50.0, abs=1.0)
    assert widget.knobs["start"][0].value() == pytest.approx(current().params["start"])
    window.editor.set_device_param(track.id, device.id, "end", 30.0)
    QTest.mousePress(view, *_press(view, view._x(30.0)))  # grabs End (Start is under it)
    QTest.mouseMove(view, _press(view, plot.left() + 5)[2])
    QTest.mouseRelease(view, *_press(view, plot.left() + 5))
    assert current().params["end"] == pytest.approx(current().params["start"])
    window.undo_stack.undo()
    window.undo_stack.undo()
    assert current().params["end"] == pytest.approx(100.0) or "end" not in current().params
    window.undo_stack.undo()
    assert current().params.get("start", 0.0) == pytest.approx(0.0)
    view.grab()

    # Its sample is saved with the project, and comes back with it.
    project_file = tmp_path / "song.gil"
    save_project(window.project, project_file)
    load_project(window.project, project_file)
    [loaded] = window.project.track(track.id).devices
    assert loaded.kind == "sampler" and device_state.from_model(loaded.state) == {"sample": path}
    window.bridge.wait_for_device_states()
    engine_id = window.bridge.engine_device_id(track.id, loaded.id)
    assert device_state.decode(window.engine.processor_state(engine_id)) == {"sample": path}
