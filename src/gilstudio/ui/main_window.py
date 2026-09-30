"""Main window: transport bar on top, browser left, arrangement centre, device
view at the bottom, menus and keyboard shortcuts."""

from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import QSettings, Qt, QTimer
from PySide6.QtGui import QAction, QCloseEvent, QKeySequence, QUndoStack
from PySide6.QtWidgets import (
    QApplication,
    QFileDialog,
    QMainWindow,
    QMessageBox,
    QSplitter,
    QVBoxLayout,
    QWidget,
)

from .. import APP_NAME, __version__
from .. import _engine as ge
from ..audio.engine_bridge import EngineBridge
from ..audio.settings import AudioSettings
from ..model.editor import ProjectEditor, is_instrument
from ..model.project import PLUGIN_KIND, PluginRef, Project
from ..model.serialization import (
    EXTENSION,
    ProjectFileError,
    load_project,
    save_project,
)
from . import icons
from .arrangement.arrangement_view import ArrangementView
from .arrangement.view_state import Selection
from .browser.browser_panel import BrowserPanel
from .device_panel import DevicePanel
from .dialogs import ExportDialog, PreferencesDialog
from .transport_bar import TransportBar

PROJECT_FILTER = f"GIL Studio Project (*{EXTENSION})"


class MainWindow(QMainWindow):
    def __init__(self, engine: ge.Engine):
        super().__init__()
        self.engine = engine
        self.project = Project(self)
        self.undo_stack = QUndoStack(self)
        self.editor = ProjectEditor(self.project, self.undo_stack)
        self.selection = Selection(self)
        self.bridge = EngineBridge(engine, self.project, self)
        self._play_start = 0.0

        self.arrangement = ArrangementView(self.editor, self.selection, self.bridge)
        self.transport = TransportBar(self.editor, self.bridge, self.arrangement.view)
        self.browser = BrowserPanel(self.bridge)
        self.devices = DevicePanel(self.editor, self.selection, self.bridge)

        right = QSplitter(Qt.Orientation.Vertical)
        right.addWidget(self.arrangement)
        right.addWidget(self.devices)
        right.setStretchFactor(0, 1)
        right.setCollapsible(0, False)
        self.splitter = QSplitter(Qt.Orientation.Horizontal)
        self.splitter.addWidget(self.browser)
        self.splitter.addWidget(right)
        self.splitter.setStretchFactor(1, 1)
        self.splitter.setSizes([300, 1100])
        self.splitter.setCollapsible(1, False)

        central = QWidget()
        layout = QVBoxLayout(central)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)
        layout.addWidget(self.transport)
        layout.addWidget(self.splitter, 1)
        self.setCentralWidget(central)
        self.setWindowIcon(icons.app_icon())

        for source in (self.bridge, self.browser, self.arrangement, self.devices):
            source.status_message.connect(self.show_message)
        self.arrangement.locate_requested.connect(self.locate)
        self.browser.file_activated.connect(self.add_file_at_insert)
        self.browser.device_activated.connect(self.add_device_to_selected_track)
        self.browser.plugin_activated.connect(lambda ref: self.add_device_to_selected_track(PLUGIN_KIND, ref))
        plugins = self.browser.plugin_index
        plugins.updated.connect(lambda: self.bridge.set_known_plugins(plugins.plugins))
        self.bridge.owner_window = lambda: int(self.winId())  # plug-in editors float above this window
        self.bridge.plugin_param_edited.connect(self._plugin_param_edited)
        self.bridge.plugin_state_dirty.connect(self.undo_stack.resetClean)
        self.transport.play_requested.connect(self.toggle_play)
        self.transport.stop_requested.connect(self.stop_button)
        self.transport.preferences_requested.connect(self.show_preferences)
        self.undo_stack.cleanChanged.connect(self._update_title)
        self.project.reset.connect(self._update_title)

        self._create_actions()
        self._restore_window()
        self._update_title()
        self.statusBar().showMessage("Ready")

    # --- Menus & shortcuts ---------------------------------------------------------

    def _action(self, menu, text: str, slot, shortcut=None, checkable: bool = False, checked: bool = False) -> QAction:
        action = QAction(text, self)
        if shortcut is not None:
            if isinstance(shortcut, (list, tuple)):
                action.setShortcuts([QKeySequence(s) for s in shortcut])
            else:
                action.setShortcut(QKeySequence(shortcut))
        if checkable:
            action.setCheckable(True)
            action.setChecked(checked)
            action.toggled.connect(slot)
        else:
            action.triggered.connect(slot)
        menu.addAction(action)
        return action

    def _create_actions(self) -> None:
        bar = self.menuBar()
        file = bar.addMenu("&File")
        self._action(file, "&New Project", self.new_project, QKeySequence.StandardKey.New)
        self._action(file, "&Open…", lambda: self.open_project(), QKeySequence.StandardKey.Open)
        file.addSeparator()
        self._action(file, "&Save", self.save_project, QKeySequence.StandardKey.Save)
        self._action(file, "Save &As…", self.save_project_as, "Ctrl+Shift+S")
        file.addSeparator()
        self._action(file, "&Export Audio…", self.export_audio, "Ctrl+Shift+R")
        file.addSeparator()
        self._action(file, "&Quit", self.close, "Ctrl+Q")

        edit = bar.addMenu("&Edit")
        undo = self.undo_stack.createUndoAction(self, "&Undo")
        undo.setShortcut(QKeySequence.StandardKey.Undo)
        redo = self.undo_stack.createRedoAction(self, "&Redo")
        redo.setShortcuts([QKeySequence("Ctrl+Y"), QKeySequence("Ctrl+Shift+Z")])
        edit.addAction(undo)
        edit.addAction(redo)
        edit.addSeparator()
        self._action(edit, "D&uplicate", self.duplicate, "Ctrl+D")
        self._action(edit, "&Split", self.split, "Ctrl+E")
        self._action(edit, "&Delete", self.delete_selection, [QKeySequence.StandardKey.Delete, "Backspace"])
        self._action(edit, "Select &All", self.select_all, QKeySequence.StandardKey.SelectAll)
        edit.addSeparator()
        self._action(edit, "Play / Stop", self.toggle_play, "Space")
        self._action(edit, "Go to Start", lambda: self.locate(0.0), "Home")
        self.loop_action = self._action(edit, "Loop", self.editor.set_loop_enabled, "Ctrl+L", checkable=True)
        self.project.settings_changed.connect(lambda: self._sync_check(self.loop_action, self.project.loop_enabled))
        self.project.reset.connect(lambda: self._sync_check(self.loop_action, self.project.loop_enabled))
        self._action(edit, "Find in Browser", self._focus_search, "Ctrl+F")

        create = bar.addMenu("&Create")
        self._action(create, "Insert Audio &Track", self.insert_track, "Ctrl+T")
        self._action(create, "Insert &MIDI Track", self.insert_midi_track, "Ctrl+Shift+T")
        self._action(create, "Insert MIDI &Clip", self.insert_midi_clip, "Ctrl+Shift+M")
        create.addSeparator()
        self._action(create, "Delete Selected Track", self.delete_track)

        view = bar.addMenu("&View")
        self.browser_action = self._action(view, "&Browser", self.browser.setVisible, "Ctrl+Alt+B",
                                           checkable=True, checked=True)
        self.devices_action = self._action(view, "&Device View", self.devices.setVisible, "Ctrl+Alt+L",
                                           checkable=True, checked=True)
        self._action(view, "&Clip View", self.arrangement.toggle_clip_view, "Shift+Tab")
        view.addSeparator()
        arrangement = self.arrangement
        self._action(view, "Zoom &In", lambda: arrangement.zoom(1.4), ["+", "=", QKeySequence.StandardKey.ZoomIn])
        self._action(view, "Zoom &Out", lambda: arrangement.zoom(1 / 1.4), ["-", QKeySequence.StandardKey.ZoomOut])
        self._action(view, "Zoom to &Arrangement", arrangement.zoom_to_arrangement, "Z")
        view.addSeparator()
        grid = arrangement.view
        self._action(view, "Narrow Grid", lambda: grid.set_grid_level(grid.grid_level - 1), "Ctrl+1")
        self._action(view, "Widen Grid", lambda: grid.set_grid_level(grid.grid_level + 1), "Ctrl+2")
        self.snap_action = self._action(view, "Snap to Grid", grid.set_snap, "Ctrl+4", checkable=True, checked=True)
        grid.grid_changed.connect(lambda: self._sync_check(self.snap_action, grid.snap))

        options = bar.addMenu("&Options")
        self._action(options, "&Preferences…", self.show_preferences, "Ctrl+,")
        self._action(options, "&Rescan Plug-ins", self.browser.rescan_plugins)

        help_menu = bar.addMenu("&Help")
        self._action(help_menu, "&About GIL Studio", self.show_about)

    @staticmethod
    def _sync_check(action: QAction, checked: bool) -> None:
        if action.isChecked() != checked:
            action.blockSignals(True)
            action.setChecked(checked)
            action.blockSignals(False)

    # --- Audio device ------------------------------------------------------------

    def start_audio(self) -> None:
        settings = AudioSettings.load()
        error = self.bridge.open_device(settings.device_name, settings.sample_rate, settings.buffer_frames,
                                        settings.exclusive)
        if error and (settings.device_name or settings.exclusive or settings.sample_rate):
            # The saved device may be gone; fall back to the system default.
            fallback = self.bridge.open_device("", 0, settings.buffer_frames, False)
            if fallback is None:
                self.show_message(f"{error}. Using the system default output instead.")
                return
        if error:
            self.show_message(f"Audio is off: {error}. Choose a device in Options > Preferences.")

    def show_preferences(self) -> None:
        PreferencesDialog(self.bridge, self).exec()

    # --- Transport -----------------------------------------------------------------

    def toggle_play(self) -> None:
        if self.bridge.is_playing:
            self.bridge.stop()
            self.bridge.locate(self._play_start)  # Ableton: return to where playback started
        else:
            self._play_start = self.selection.insert_beat
            self.bridge.locate(self._play_start)
            self.bridge.play()

    def stop_button(self) -> None:
        if self.bridge.is_playing:
            self.bridge.stop()
            self.bridge.locate(self._play_start)
        else:
            self.locate(0.0)

    def locate(self, beat: float) -> None:
        self.selection.set_insert(beat)
        self._play_start = beat
        self.bridge.locate(beat)

    # --- Editing -------------------------------------------------------------------

    def _after_selected_track(self) -> int | None:
        if self.selection.track_id and self.project.has_track(self.selection.track_id):
            return self.project.track_index(self.selection.track_id) + 1
        return None

    def insert_track(self) -> None:
        track = self.editor.add_audio_track(self._after_selected_track())
        self.selection.select_track(track.id, focus_track=True)

    def insert_midi_track(self) -> None:
        track = self.editor.add_midi_track(self._after_selected_track())
        self.selection.select_track(track.id, focus_track=True)

    def insert_midi_clip(self) -> None:
        """Ctrl+Shift+M: a MIDI clip over the time selection on each MIDI track in it,
        or at the insert marker (a bar long) on the selected MIDI track."""
        time_range = self.selection.time_range
        if time_range is not None:
            start, end, track_ids = time_range
            refs = [self.editor.add_midi_clip(tid, start, end - start) for tid in track_ids
                    if self.project.track(tid).is_midi]
        else:
            track_id = self.selection.track_id
            refs = []
            if track_id and self.project.has_track(track_id) and self.project.track(track_id).is_midi:
                view = self.arrangement.view
                start, length = self.editor.midi_clip_span(
                    track_id, self.selection.insert_beat, view.grid_step() if view.snap else 0.0)
                refs = [self.editor.add_midi_clip(track_id, start, length)]
        refs = [ref for ref in refs if ref is not None]
        if refs:
            self.selection.set_clips(refs, track_id=refs[0][0])
        else:
            self.show_message("Select a MIDI track (or a time range on one) to insert a MIDI clip.")

    def add_device_to_selected_track(self, kind: str, plugin: PluginRef | None = None) -> None:
        """A built-in device `kind`, or kind 'plugin' and a `plugin`."""
        track_id = self.selection.track_id
        has_track = bool(track_id) and self.project.has_track(track_id)
        if is_instrument(kind, plugin) and not (has_track and self.project.track(track_id).is_midi):
            # As in Ableton: an instrument chosen with no MIDI track selected gets a new one.
            track = self.editor.add_midi_track(self._after_selected_track(), instrument=None if plugin else kind,
                                               plugin=plugin)
            self.selection.select_track(track.id, focus_track=True)
        elif has_track:
            self.editor.add_device(track_id, kind, plugin=plugin)
        else:
            self.show_message("Select a track to add the device to.")

    def _plugin_param_edited(self, track_id: str, device_id: str, param_id: str, value: float, old: float,
                             gesture: int) -> None:
        """A plug-in's own editor changed a parameter: an undo step (one per knob drag)."""
        if self.project.has_track(track_id):
            self.editor.set_device_param(track_id, device_id, param_id, value,
                                         merge_key=("plugin edit", device_id, param_id, gesture), old=old)

    def delete_track(self) -> None:
        if self.selection.track_id and self.project.has_track(self.selection.track_id):
            self.editor.delete_tracks([self.selection.track_id])

    def delete_selection(self) -> None:
        time_range = self.selection.time_range
        if time_range is not None:
            if self.selection.clip_range:
                # Cut out just the selected area; the selection stays, now empty.
                start, end, track_ids = time_range
                self.editor.delete_range(start, end, list(track_ids))
                self.selection.set_time_range(start, end, track_ids, clips=set())
            return  # a lane range will delete automation
        if self.selection.clips:
            self.editor.delete_clips(sorted(self.selection.clips))
        elif self.selection.focus == "track":
            self.delete_track()

    def duplicate(self) -> None:
        time_range = self.selection.time_range
        if time_range is not None:
            if self.selection.clip_range:
                # Copy the selected area to right after it, and select the copy.
                start, end, track_ids = time_range
                length = end - start
                self.editor.duplicate_range(start, end, list(track_ids))
                self.selection.set_time_range(end, end + length, track_ids,
                                              clips=self.editor.clips_in_range(end, end + length, track_ids))
                self.selection.set_insert(end)
            return  # a lane range will duplicate automation
        if self.selection.clips:
            refs = self.editor.duplicate_clips(sorted(self.selection.clips))
            self.selection.set_clips(refs)

    def split(self) -> None:
        refs = sorted(self.selection.clips)
        if not refs and self.selection.track_id and self.project.has_track(self.selection.track_id):
            refs = self.editor.clips_at([self.selection.track_id], self.selection.insert_beat)
        if refs:
            self.editor.split_clips(refs, self.selection.insert_beat)

    def select_all(self) -> None:
        self.selection.set_clips({(t.id, c.id) for t in self.project.tracks for c in t.clips})

    def add_file_at_insert(self, path: str) -> None:
        info = self.bridge.file_info(path)
        if info is None:
            return
        track_id = self.selection.track_id if self.project.has_track(self.selection.track_id or "") else None
        refs = self.editor.add_clips(track_id, self.selection.insert_beat, [(path, info.duration)],
                                     track_index=len(self.project.tracks))
        if refs:
            self.selection.set_clips(refs, track_id=refs[0][0])

    def _focus_search(self) -> None:
        if not self.browser.isVisible():
            self.browser_action.setChecked(True)
        self.browser.focus_search()

    # --- Files -----------------------------------------------------------------------

    def _confirm_discard(self) -> bool:
        if self.undo_stack.isClean():
            return True
        answer = QMessageBox.question(
            self, APP_NAME, "Save changes to the current project?",
            QMessageBox.StandardButton.Save | QMessageBox.StandardButton.Discard | QMessageBox.StandardButton.Cancel)
        if answer == QMessageBox.StandardButton.Save:
            return self.save_project()
        return answer == QMessageBox.StandardButton.Discard

    def _reset_session(self) -> None:
        self.bridge.stop()
        self.undo_stack.clear()
        self.undo_stack.setClean()
        self._play_start = 0.0
        self.selection.set_clips(set())
        self.selection.select_track(None)
        self.selection.set_insert(0.0)
        self.bridge.locate(0.0)

    def new_project(self) -> None:
        if self._confirm_discard():
            self.project.clear()
            self._reset_session()

    def open_project(self, path: str | None = None) -> None:
        if not self._confirm_discard():
            return
        if path is None:
            path, _ = QFileDialog.getOpenFileName(self, "Open Project", self._last_dir(), PROJECT_FILTER)
            if not path:
                return
        try:
            load_project(self.project, Path(path))
        except ProjectFileError as exc:
            QMessageBox.warning(self, APP_NAME, str(exc))
            return
        self._reset_session()
        QSettings().setValue("files/last_dir", str(Path(path).parent))
        self.arrangement.zoom_to_arrangement()
        self.show_message(f"Opened {Path(path).name}")

    def save_project(self) -> bool:
        if self.project.path is None:
            return self.save_project_as()
        return self._save_to(self.project.path)

    def save_project_as(self) -> bool:
        suggested = str(Path(self._last_dir()) / f"Untitled{EXTENSION}")
        path, _ = QFileDialog.getSaveFileName(self, "Save Project As", suggested, PROJECT_FILTER)
        if not path:
            return False
        return self._save_to(Path(path))

    def _save_to(self, path: Path) -> bool:
        self.bridge.store_plugin_states()
        try:
            save_project(self.project, path)
        except OSError as exc:
            QMessageBox.warning(self, APP_NAME, f"Could not save {path.name}: {exc}")
            return False
        self.undo_stack.setClean()
        QSettings().setValue("files/last_dir", str(path.parent))
        self._update_title()
        self.show_message(f"Saved {path.name}")
        return True

    def export_audio(self) -> None:
        project = self.project
        dialog = ExportDialog(project.loop_enabled and project.loop_end > project.loop_start, self)
        if not dialog.exec():
            return
        if dialog.range.currentData() == "loop":
            start, end = project.loop_start, project.loop_end
        else:
            start, end = 0.0, project.end_beat()
        if end <= start:
            QMessageBox.information(self, APP_NAME, "There is nothing to export yet.")
            return
        name = project.path.stem if project.path else "Untitled"
        path, _ = QFileDialog.getSaveFileName(self, "Export Audio", str(Path(self._last_dir()) / f"{name}.wav"),
                                              "WAV Audio (*.wav)")
        if not path:
            return
        if self.bridge.is_playing:
            self.toggle_play()
        QApplication.setOverrideCursor(Qt.CursorShape.WaitCursor)
        try:
            self.engine.export_wav(path, start, end, int(dialog.bit_depth.currentData()))
        except (RuntimeError, ValueError) as exc:
            QMessageBox.warning(self, APP_NAME, f"Export failed: {exc}")
            return
        finally:
            QApplication.restoreOverrideCursor()
        self.show_message(f"Exported {Path(path).name}")

    def _last_dir(self) -> str:
        return str(QSettings().value("files/last_dir", str(Path.home() / "Music")))

    # --- Misc ----------------------------------------------------------------------

    def show_message(self, text: str) -> None:
        self.statusBar().showMessage(text, 8000)

    def show_about(self) -> None:
        QMessageBox.about(
            self, f"About {APP_NAME}",
            f"<b>{APP_NAME}</b> {__version__}<br>A basic DAW: Python/Qt interface, C++ audio engine "
            "(miniaudio, WASAPI).<br><br>Hosts VST3 instruments and effects.<br>"
            "VST is a registered trademark of Steinberg Media Technologies GmbH.")

    def _update_title(self, *_args) -> None:
        name = self.project.path.stem if self.project.path else "Untitled"
        dirty = "" if self.undo_stack.isClean() else "*"
        self.setWindowTitle(f"{name}{dirty} - {APP_NAME}")

    def _restore_window(self) -> None:
        settings = QSettings()
        geometry = settings.value("window/geometry")
        if geometry is not None:
            self.restoreGeometry(geometry)
        else:
            self.resize(1440, 860)
        splitter = settings.value("window/splitter")
        if splitter is not None:
            self.splitter.restoreState(splitter)

    def closeEvent(self, event: QCloseEvent) -> None:
        if not self._confirm_discard():
            event.ignore()
            return
        settings = QSettings()
        settings.setValue("window/geometry", self.saveGeometry())
        settings.setValue("window/splitter", self.splitter.saveState())
        self.bridge.stop()
        self.bridge.stop_preview()
        self.bridge.close_all_editors()
        self.browser.shutdown()
        event.accept()

    def showEvent(self, event) -> None:
        super().showEvent(event)
        QTimer.singleShot(0, self.arrangement.lanes.setFocus)
