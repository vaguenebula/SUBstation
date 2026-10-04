"""Built-in devices' own editors in the device view (ui/device_editors), shown with the Compressor's."""

import time

import numpy as np
import pytest
from PySide6.QtCore import QPoint, Qt
from PySide6.QtTest import QTest

from substation.ui.device_editors import editor_for
from substation.ui.device_editors.compressor import CompressorWidget
from substation.ui.device_panel import DeviceWidget, device_height

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
    from substation.model import device_state
    from substation.model.project import Note
    from substation.ui.device_editors.sampler import SamplerWidget

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

    from substation.model import device_state
    from substation.model.serialization import load_project, save_project

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


def test_delay_editor(window):
    from substation.ui.device_editors.delay import DelayWidget, filter_response

    assert editor_for("delay") is DelayWidget
    window.insert_midi_track()
    track = window.project.tracks[0]
    window.selection.select_track(track.id)
    device = window.editor.add_device(track.id, "delay")
    widget = window.devices.widgets[device.id]
    assert isinstance(widget, DelayWidget) and widget.pages == 1
    assert widget.minimumSizeHint().height() <= device_height(window.editor)  # fits the view

    def param(pid):
        return window.project.device(track.id, device.id).params.get(pid)

    # The sixteenths are exclusive buttons; a click sets the parameter, undoably.
    buttons = widget._views["l_division"]
    assert len(buttons) == 8
    grid = widget._times["l"].widget(0)
    from substation.ui.widgets import ToggleButton
    five = next(b for b in grid.findChildren(ToggleButton) if b.text() == "5")
    five.click()
    assert param("l_division") == 4.0 and five.isChecked()
    window.undo_stack.undo()
    assert not five.isChecked()

    # Sync off shows the time knob instead of the grid; Link turns the right side off.
    window.editor.set_device_param(track.id, device.id, "l_sync", 0.0)
    assert widget._times["l"].currentIndex() == 1
    widget.link.click()
    assert param("link") == 1.0 and not any(w.isEnabled() for w in widget._right)

    # Mode buttons choose one; the graph drags the filter's frequency and width.
    fade = next(b for b in widget.findChildren(ToggleButton) if b.text() == "Fade")
    fade.click()
    assert param("mode") == 1.0
    graph = widget.graph
    graph.resize(graph.width(), 60)
    QTest.mouseClick(graph, Qt.MouseButton.LeftButton, pos=QPoint(graph.width() // 2, 30))
    assert 400 < param("freq") < 1000 and 4.0 < param("width") < 6.0
    graph.grab()  # paints
    widget.grab()
    assert filter_response(1000.0, 1000.0, 8.0) == pytest.approx(0.0, abs=0.01)
    assert filter_response(1000.0 * 2 ** 4, 1000.0, 8.0) == pytest.approx(-3.0, abs=0.1)  # at a corner

    # The spectrum of the input shows behind the filter's curve: a 1 kHz tone peaks at 1 kHz.
    t = np.arange(8192) / 48000.0
    graph.add_samples(0.5 * np.sin(2 * np.pi * 1000.0 * t), 48000.0)
    columns = graph.spectrum_columns(200)
    peak = 20.0 * 1000.0 ** ((int(np.argmax(columns)) + 0.5) / 200)  # the graph spans 20 Hz to 20 kHz
    assert 900 < peak < 1100 and columns.max() == pytest.approx(20 * np.log10(0.5), abs=1.0)
    graph.add_samples(np.zeros(0, np.float32), 48000.0)  # nothing new: it falls back slowly
    assert graph.spectrum_columns(200).max() == pytest.approx(columns.max() - 1.0, abs=0.01)
    graph.grab()


def test_eq_editor(window):
    from PySide6.QtCore import QPointF
    from PySide6.QtWidgets import QApplication

    from substation.ui.device_editors import eq

    assert editor_for("eq") is eq.EqWidget
    window.insert_midi_track()
    track = window.project.tracks[0]
    window.selection.select_track(track.id)
    device = window.editor.add_device(track.id, "eq")
    widget = window.devices.widgets[device.id]
    assert isinstance(widget, eq.EqWidget) and widget.pages == 1
    assert widget.minimumSizeHint().height() <= device_height(window.editor)  # fits the view
    view, graph = widget.eq, widget.eq.graph
    QApplication.processEvents()  # laid out: the graph at its size

    def value(pid):
        return window.project.device(track.id, device.id).params.get(pid)

    def point(freq, db):
        return QPoint(round(graph._x(freq)), round(graph._y(db)))

    # Hovering the (flat) curve shows a ghost band; a click adds it, of the type for where it is.
    QTest.mouseMove(graph, point(1000.0, 0.0))
    assert graph.ghost is not None and graph.ghost[1] == eq.BELL
    QTest.mouseMove(graph, point(1000.0, 9.0))  # off the curve: nothing to add
    assert graph.ghost is None
    QTest.mouseMove(graph, point(1000.0, 0.0))
    QTest.mousePress(graph, Qt.MouseButton.LeftButton, pos=point(1000.0, 0.0))
    QTest.mouseMove(graph, point(2000.0, 6.0))  # and dragging it on is the same step
    QTest.mouseRelease(graph, Qt.MouseButton.LeftButton, pos=point(2000.0, 6.0))
    assert value("b1_used") == 1.0 and value("b1_type") == eq.BELL and view.selected == 0
    assert value("b1_freq") == pytest.approx(2000.0, rel=0.03) and value("b1_gain") == pytest.approx(6.0, abs=0.3)
    window.undo_stack.undo()
    assert value("b1_used") == 0.0 and view.bands[0] is None
    window.undo_stack.redo()

    for x_fraction, kind in ((0.03, eq.LOW_CUT), (0.15, eq.LOW_SHELF), (0.85, eq.HIGH_SHELF), (0.97, eq.HIGH_CUT)):
        x = round(graph._plot().left() + x_fraction * graph._plot().width())
        y = round(graph._curve_y(x))
        QTest.mouseMove(graph, QPoint(x, y))
        assert graph.ghost is not None and graph.ghost[1] == kind
        QTest.mouseClick(graph, Qt.MouseButton.LeftButton, pos=QPoint(x, y))
    assert [b.type for b in view.bands[:5]] == [eq.BELL, eq.LOW_CUT, eq.LOW_SHELF, eq.HIGH_SHELF, eq.HIGH_CUT]
    assert value("b2_slope") == 3.0  # cuts start at 24 dB/octave

    # Dragging a band moves it (one undo step); Ctrl-drag and the wheel set its Q.
    dot = graph.dot(view.bands[0]).toPoint()
    QTest.mousePress(graph, Qt.MouseButton.LeftButton, pos=dot)
    for step in range(1, 6):
        QTest.mouseMove(graph, dot + QPoint(0, 4 * step))
    QTest.mouseRelease(graph, Qt.MouseButton.LeftButton, pos=dot + QPoint(0, 20))
    lowered = value("b1_gain")
    assert lowered < 6.0 - 2.0 and value("b1_freq") == pytest.approx(2000.0, rel=0.03)
    window.undo_stack.undo()
    assert value("b1_gain") == pytest.approx(6.0, abs=0.3)
    q = value("b1_q")
    dot = graph.dot(view.bands[0])
    graph.wheelEvent(_wheel(dot, 120))
    assert value("b1_q") == pytest.approx(q * 1.15, rel=0.01)

    # The wheel while dragging a cut sets its slope instead.
    cut = graph.dot(view.bands[1]).toPoint()
    QTest.mousePress(graph, Qt.MouseButton.LeftButton, pos=cut)
    q = value("b2_q")
    graph.wheelEvent(_wheel(cut, 120))
    QTest.mouseRelease(graph, Qt.MouseButton.LeftButton, pos=cut)
    assert value("b2_slope") == 4.0 and value("b2_q") == q  # 24 dB/octave to 30

    # Double-click switches a band off and on; Alt-click deletes it; so does Delete.
    dot = graph.dot(view.bands[0]).toPoint()
    QTest.mouseDClick(graph, Qt.MouseButton.LeftButton, pos=dot)
    assert value("b1_on") == 0.0 and not view.bands[0].on
    QTest.mouseClick(graph, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.AltModifier, dot)
    assert view.bands[0] is None
    view.select(1)
    QTest.keyClick(graph, Qt.Key.Key_Delete)
    assert view.bands[1] is None and view.selected is None

    # The wheel's Q holds as the drag goes on: for a band the drag added (whose every move sets all
    # its parameters), and for a notch (whose drag sets its Q). Adding and changing it is still one step.
    for kind, freq in ((eq.BELL, 500.0), (eq.NOTCH, 300.0)):
        x = round(graph._x(freq))
        y = round(graph._curve_y(x))
        QTest.mouseMove(graph, QPoint(x, y))
        QTest.mousePress(graph, Qt.MouseButton.LeftButton, pos=QPoint(x, y))
        index = view.selected
        if kind == eq.NOTCH:
            view.set_type(index, eq.NOTCH)
        start = value(eq.param(index, "q"))
        graph.wheelEvent(_wheel(QPointF(x, y), 240))
        for step in range(1, 4):
            QTest.mouseMove(graph, QPoint(x + 5 * step, y))
        QTest.mouseRelease(graph, Qt.MouseButton.LeftButton, pos=QPoint(x + 15, y))
        assert value(eq.param(index, "q")) == pytest.approx(start * 1.15 ** 2, rel=0.01)
        if kind == eq.BELL:
            window.undo_stack.undo()
            assert view.bands[index] is None
        else:
            view.remove(index)

    # The panel edits the selected band: its type buttons, knobs and slope.
    view.select(2)
    assert view.panel.pages.currentIndex() == 1 and view.panel.types[eq.LOW_SHELF].isChecked()
    view.panel.types[eq.TILT_SHELF].click()
    assert value("b3_type") == eq.TILT_SHELF
    view.panel.knobs["freq"][0].valueChanged.emit(150.0, object())
    assert value("b3_freq") == 150.0
    view.panel.slope.activated.emit(5)
    assert value("b3_slope") == 5.0
    view.select(None)
    assert view.panel.pages.currentIndex() == 0

    # With 24 bands there is no ghost to add another.
    view.set_params({eq.param(i, "used"): 1.0 for i in range(eq.BANDS)})
    assert view.free_band() is None
    QTest.mouseMove(graph, QPoint(5, 5))
    assert graph.ghost is None

    # The analyzer: a 1 kHz tone peaks at 1 kHz.
    t = np.arange(eq.FFT_SIZE) / 48000.0
    for _ in range(20):
        view.analyzer.feed("output", (0.5 * np.sin(2 * np.pi * 1000.0 * t)).astype(np.float32), 48000.0)
    freqs = graph.column_freqs()
    peak = freqs[int(np.argmax(view.analyzer.columns("output", freqs)))]
    assert 900 < peak < 1100
    widget.grab()  # paints

    # The window shows the same, and goes with the device.
    window_ = eq.open_window(window.editor, window.bridge, track.id, device.id)
    assert window_.eq.bands[3] is not None and window_.isVisible()
    assert eq.open_window(window.editor, window.bridge, track.id, device.id) is window_
    window_.eq.set_params({"output": 3.0})
    assert value("output") == 3.0 and widget.eq.globals["output"][0].value() == 3.0
    window_.grab()
    window.editor.remove_device(track.id, device.id)
    assert (track.id, device.id) not in eq._WINDOWS


def _wheel(pos, delta):
    from PySide6.QtCore import QPointF
    from PySide6.QtGui import QWheelEvent

    return QWheelEvent(QPointF(pos), QPointF(pos), QPoint(0, 0), QPoint(0, delta), Qt.MouseButton.NoButton,
                       Qt.KeyboardModifier.NoModifier, Qt.ScrollPhase.NoScrollPhase, False)


def test_sidechain_editor(window, tmp_path):
    from PySide6.QtCore import QPointF
    from PySide6.QtWidgets import QApplication

    from substation.model.project import PRE_FADER, Sidechain
    from substation.ui.device_editors import sidechain as sc

    assert editor_for("sidechain") is sc.SidechainWidget
    # A kick (short, every 4096 samples) and a bass at 55 Hz, each on a track of its own.
    period, seconds = 4096, 2.0
    t = np.arange(int(seconds * SAMPLE_RATE)) / SAMPLE_RATE
    burst = np.arange(period) / SAMPLE_RATE
    one = 0.9 * np.sin(2 * np.pi * np.cumsum(50 + 100 * np.exp(-burst / 0.01)) / SAMPLE_RATE) * np.exp(-burst / 0.02)
    kick = np.tile(one, len(t) // period + 1)[:len(t)]
    bass = 0.4 * np.sin(2 * np.pi * 55 * t)
    kick_path = str(write_wav(tmp_path / "kick.wav", np.stack([kick, kick], axis=1)))
    bass_path = str(write_wav(tmp_path / "bass.wav", np.stack([bass, bass], axis=1)))
    window.editor.add_clips(None, 0.0, [(kick_path, seconds)], track_index=0)
    window.editor.add_clips(None, 0.0, [(bass_path, seconds)], track_index=1)
    kick_track, bass_track = window.project.tracks[0], window.project.tracks[1]
    window.selection.select_track(bass_track.id)
    device = window.editor.add_device(bass_track.id, "sidechain")
    widget = window.devices.widgets[device.id]
    assert isinstance(widget, sc.SidechainWidget) and widget.pages == 1
    assert widget.minimumSizeHint().height() <= device_height(window.editor)  # fits the view
    graph = widget.graph
    QApplication.processEvents()

    def value(pid):
        return window.project.device(bass_track.id, device.id).params.get(pid)

    def screen(x, y):
        at = graph.to_screen(x, y)
        return QPoint(round(at.x()), round(at.y()))

    # The default curve: ducked at once, held, then back up.
    assert [(p.x, p.y) for p in widget.points()] == [(0.0, 0.0), (pytest.approx(0.1), 0.0), (1.0, 1.0)]
    assert widget.hint()  # no sidechain yet: it says to choose one

    # A click off the curve adds a point, and dragging it on is the same step.
    QTest.mousePress(graph, Qt.MouseButton.LeftButton, pos=screen(0.5, 0.95))
    QTest.mouseMove(graph, screen(0.6, 0.9))
    QTest.mouseRelease(graph, Qt.MouseButton.LeftButton, pos=screen(0.6, 0.9))
    points = widget.points()
    assert len(points) == 4 and points[2].x == pytest.approx(0.6, abs=0.01) and points[2].y == pytest.approx(0.9, abs=0.02)
    assert value("p4_used") == 1.0
    window.undo_stack.undo()
    assert len(widget.points()) == 3
    window.undo_stack.redo()

    # The first point only moves up and down; a point between moves between its neighbours.
    QTest.mousePress(graph, Qt.MouseButton.LeftButton, pos=screen(0.0, 0.0))
    QTest.mouseMove(graph, screen(0.3, 0.4))
    QTest.mouseRelease(graph, Qt.MouseButton.LeftButton, pos=screen(0.3, 0.4))
    first = widget.points()[0]
    assert first.x == 0.0 and first.y == pytest.approx(0.4, abs=0.02)
    QTest.mousePress(graph, Qt.MouseButton.LeftButton, pos=screen(0.6, 0.9))
    QTest.mouseMove(graph, screen(0.95, 0.9))
    QTest.mouseRelease(graph, Qt.MouseButton.LeftButton, pos=screen(0.95, 0.9))
    assert widget.points()[2].x == pytest.approx(0.95, abs=0.01)

    # Dragging between points bends the curve there; the wheel too; a double-click straightens it.
    points = widget.points()
    middle = (points[1].x + points[2].x) / 2
    at = screen(middle, sc.curve_value(points, middle))
    assert graph.hit(QPointF(at)) == ("segment", 1)
    QTest.mousePress(graph, Qt.MouseButton.LeftButton, pos=at)
    QTest.mouseMove(graph, at - QPoint(0, 30))
    QTest.mouseRelease(graph, Qt.MouseButton.LeftButton, pos=at - QPoint(0, 30))
    bent = widget.points()[1].curve
    assert bent > points[1].curve + 0.4  # up: bulging up
    at = screen(middle, sc.curve_value(widget.points(), middle))
    QApplication.sendEvent(graph, _wheel(at, -120))
    assert widget.points()[1].curve == pytest.approx(bent - 0.08)
    at = screen(middle, sc.curve_value(widget.points(), middle))
    QTest.mouseDClick(graph, Qt.MouseButton.LeftButton, pos=at)
    assert widget.points()[1].curve == 0.0

    # Double-click (or Alt-click) a point to remove it; the ends stay.
    count = len(widget.points())
    p2 = widget.points()[2]
    QTest.mouseDClick(graph, Qt.MouseButton.LeftButton, pos=screen(p2.x, p2.y))
    assert len(widget.points()) == count - 1
    QTest.mouseClick(graph, Qt.MouseButton.LeftButton, Qt.KeyboardModifier.AltModifier, screen(1.0, 1.0))
    assert len(widget.points()) == count - 1
    graph._flip()
    assert [p.y for p in widget.points()][-1] == 0.0  # flipped: the end swells instead

    # The controls: the knobs set their parameters; Sync shows the synced length.
    widget.controls["threshold"][0].valueChanged.emit(-30.0, object())
    assert value("threshold") == -30.0 and widget.controls["threshold"][1].text() == "-30.0 dB"
    widget.sync_button.click()
    assert value("sync") == 1.0 and widget.length_stack.currentIndex() == 1
    assert widget.length_ms() == pytest.approx(500.0)  # 1/4 at 120
    widget.sync_button.click()
    widget.lows.click()
    assert value("range") == 1.0 and widget.controls["crossover"][0].isEnabled()

    # With the kick as its sidechain, hits come (one each 4096 samples), and the fit follows the kick.
    window.editor.set_device_sidechain(bass_track.id, device.id, Sidechain(kick_track.id, PRE_FADER))
    assert not widget.hint()
    QTest.qWait(50)
    widget._positions.clear()
    beats_per_chunk = period / (SAMPLE_RATE * 60 / window.project.tempo)
    for chunk in range(12):  # (each render is reset at its start: on a hit)
        window.engine.render_offline(chunk * beats_per_chunk, period)
        widget.refresh_displays()
    assert widget.capture.hit_count == 12
    fit = widget.fit
    assert fit is not None and fit.spectra.bass_heard and fit.spectra.low < 55 < fit.spectra.high
    assert graph.trail and graph.flash > 0  # the playhead rode the curve
    graph.grab()  # paints (the kick, the playhead)
    widget.clash.grab()

    widget.fit_button.click()
    assert [(p.x, p.y) for p in widget.points()] == [pytest.approx(p[:2], abs=1e-4) for p in fit.points]
    assert value("length") == pytest.approx(fit.length) and value("sync") == 0.0
    window.undo_stack.undo()  # one step
    assert len(widget.points()) != len(fit.points) or value("length") != pytest.approx(fit.length)
    window.undo_stack.redo()

    # Auto fits again at every hit, all in one step.
    widget.character.setCurrentIndex(2)
    widget.character.activated.emit(2)
    assert value("character") == 2.0
    loose = widget.fit
    assert loose.length > fit.length  # Loose: out of the way for longer
    steps = window.undo_stack.count()
    widget.auto.click()
    assert value("autofit") == 1.0 and value("length") == pytest.approx(loose.length)
    for chunk in range(12, 16):
        window.engine.render_offline(chunk * beats_per_chunk, period)
        widget.refresh_displays()
    assert window.undo_stack.count() == steps + 2  # switching it on, then the fits (merged)
    widget.auto.click()
    assert value("autofit") == 0.0
