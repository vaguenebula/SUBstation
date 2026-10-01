"""Main window: transport bar on top, browser left, arrangement centre, device
view at the bottom, menus and keyboard shortcuts."""

from __future__ import annotations

from dataclasses import replace
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
from ..model import automation
from ..model.editor import ProjectEditor, is_instrument
from ..model.project import PLUGIN_KIND, PluginRef, Project
from ..model.serialization import (
    EXTENSION,
    ProjectFileError,
    load_project,
    save_project,
)
from . import icons, plugin_keys
from .arrangement.arrangement_view import ArrangementView
from .arrangement.view_state import Selection
from .browser.browser_panel import BrowserPanel
from .device_panel import DevicePanel
from .dialogs import ExportDialog, PreferencesDialog
from .transport_bar import TransportBar

PROJECT_FILTER = f"GIL Studio Project (*{EXTENSION})"
RECENT_KEY = "files/recent"
MAX_RECENT = 10


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
        # Only the selected track's plug-in editors are shown; a new plug-in shows its editor.
        self.selection.changed.connect(lambda: self.bridge.show_plugin_editors(self.selection.track_id))
        self.editor.plugin_added.connect(self._plugin_added)
        self.bridge.plugin_param_edited.connect(self._plugin_param_edited)
        self.bridge.plugin_param_touched.connect(
            lambda track_id, device_id, param_id: self.editor.touch_parameter(
                track_id, automation.device_key(device_id, param_id)))
        self.bridge.plugin_state_dirty.connect(self.undo_stack.resetClean)
        self.transport.play_requested.connect(self.toggle_play)
        self.transport.record_requested.connect(self.toggle_record)
        self.bridge.takes_recorded.connect(self._add_takes)
        self.transport.stop_requested.connect(self.stop_button)
        self.transport.preferences_requested.connect(self.show_preferences)
        self.transport.re_enable_requested.connect(self.bridge.re_enable_automation)
        self.undo_stack.cleanChanged.connect(self._update_title)
        self.project.reset.connect(self._update_title)

        self._create_actions()
        # The Ctrl/Alt shortcuts work while a plug-in's editor has the focus too.
        self._plugin_shortcuts = None
        if plugin_keys.supported():
            self._plugin_shortcuts = plugin_keys.PluginEditorShortcuts(self)
            QApplication.instance().installNativeEventFilter(self._plugin_shortcuts)
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
        self.recent_menu = file.addMenu("Open &Recent")
        self.recent_menu.aboutToShow.connect(self._fill_recent_menu)
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
        self._action(edit, "&Consolidate", self.arrangement.lanes.consolidate, "Ctrl+J")
        self._action(edit, "&Delete", self.delete_selection, [QKeySequence.StandardKey.Delete, "Backspace"])
        self._action(edit, "Select &All", self.select_all, QKeySequence.StandardKey.SelectAll)
        edit.addSeparator()
        self.re_enable_action = self._action(edit, "Re-Enable Automation", lambda: self.bridge.re_enable_automation())
        self.re_enable_action.setEnabled(False)
        self.bridge.automation_state_changed.connect(
            lambda _owner: self.re_enable_action.setEnabled(self.bridge.has_overrides))
        self._action(edit, "Solo Selected Tracks", self.solo_selected_tracks, "S")
        edit.addSeparator()
        self._action(edit, "Play / Stop", self.toggle_play, "Space")
        self._action(edit, "Record", self.toggle_record, "F9")
        self._action(edit, "Go to Start", lambda: self.locate(0.0), "Home")
        self.loop_action = self._action(edit, "Loop", self.editor.set_loop_enabled, "Ctrl+L", checkable=True)
        self.project.settings_changed.connect(lambda: self._sync_check(self.loop_action, self.project.loop_enabled))
        self.project.reset.connect(lambda: self._sync_check(self.loop_action, self.project.loop_enabled))
        self._action(edit, "Find in Browser", self._focus_search, "Ctrl+F")

        create = bar.addMenu("&Create")
        self._action(create, "Insert Audio &Track", self.insert_track, "Ctrl+T")
        self._action(create, "Insert &MIDI Track", self.insert_midi_track, "Ctrl+Shift+T")
        self._action(create, "Insert MIDI &Clip", self.insert_midi_clip, ["Ctrl+Shift+D", "Ctrl+Shift+M"])
        create.addSeparator()
        self._action(create, "Delete Selected Tracks", self.delete_track)

        view = bar.addMenu("&View")
        self.browser_action = self._action(view, "&Browser", self.browser.setVisible, "Ctrl+Alt+B",
                                           checkable=True, checked=True)
        self.devices_action = self._action(view, "&Device View", self.devices.setVisible, "Ctrl+Alt+L",
                                           checkable=True, checked=True)
        self._action(view, "&Clip View", self.arrangement.toggle_clip_view, "Shift+Tab")
        self._action(view, "&Automation", self.editor.toggle_all_automation, "A")
        if plugin_keys.supported():
            self._action(view, "Close Plug-in &Editor", lambda: plugin_keys.close_foremost_editor(), "Ctrl+W")
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
        options.addSeparator()
        self.lock_action = self._action(options, "&Lock Envelopes", self.editor.set_automation_locked, checkable=True)
        for signal in (self.project.settings_changed, self.project.reset):
            signal.connect(lambda: self._sync_check(self.lock_action, self.project.automation_locked))

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
        error = self.bridge.open_device(settings)
        if error is None:
            return
        # The device may not take the saved settings any more (an ASIO driver
        # locked to another clock), or be gone: its own settings, then the system default.
        fallbacks = [
            (replace(settings, sample_rate=0, buffer_frames=0, output_channels=(), input_channels=()),
             "Using the device's own settings instead."),
            (AudioSettings(buffer_frames=settings.buffer_frames), "Using the system default output instead."),
        ]
        for fallback, note in fallbacks:
            if fallback != settings and self.bridge.open_device(fallback) is None:
                self.show_message(f"{error}. {note}")
                return
        self.show_message(f"Audio is off: {error}. Choose a device in Options > Preferences.")

    def show_preferences(self) -> None:
        dialog = PreferencesDialog(self.bridge, self, plugins=self.browser.plugin_index)
        dialog.exec()
        dialog.deleteLater()

    # --- Transport -----------------------------------------------------------------

    def toggle_play(self) -> None:
        if self.bridge.is_playing:
            self.bridge.stop()
            self.bridge.locate(self._play_start)  # Ableton: return to where playback started
        else:
            self._play_start = self.selection.insert_beat
            self.bridge.locate(self._play_start)
            self.bridge.play()

    def toggle_record(self) -> None:
        """F9 / the record button: record the armed tracks from the insert marker
        (after the count-in) or, while playing, from where the playhead is; again: stop recording."""
        if self.bridge.is_recording:
            self.bridge.stop_recording()  # punch out: playing goes on
            return
        playing = self.bridge.is_playing
        if not playing:
            self._play_start = self.selection.insert_beat
            self.bridge.locate(self._play_start)
        error = self.bridge.start_recording(0.0 if playing else self.transport.count_in_beats())
        if error:
            self.show_message(error)

    def _add_takes(self, takes) -> None:
        refs = self.editor.add_recordings(takes)
        if refs:
            self.selection.select_clips(self.editor, refs)

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
        """Ctrl+Shift+D / Ctrl+Shift+M: a MIDI clip over the time selection on each MIDI
        track in it, or at the insert marker (a bar long) on the selected MIDI track;
        opened in the piano roll."""
        time_range = self.selection.time_range
        if time_range is not None:
            refs = self.editor.add_midi_clips_over(*time_range)
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
            self.selection.select_clips(self.editor, refs, track_id=refs[0][0])
            self.arrangement.open_clips(refs, lead=refs[0])  # straight into the piano roll
        else:
            self.show_message("Select a MIDI track (or a time range on one) to insert a MIDI clip.")

    def add_device_to_selected_track(self, kind: str, plugin: PluginRef | None = None) -> None:
        """A built-in device `kind`, or kind 'plugin' and a `plugin`."""
        track_id = self.selection.track_id
        has_track = bool(track_id) and self.project.has_owner(track_id)  # a track or the master
        if is_instrument(kind, plugin) and not (has_track and self.project.track(track_id).is_midi):
            # As in Ableton: an instrument chosen with no MIDI track selected gets a new one.
            track = self.editor.add_midi_track(self._after_selected_track(), instrument=None if plugin else kind,
                                               plugin=plugin)
            self.selection.select_track(track.id, focus_track=True)
        elif has_track:
            self.editor.add_device(track_id, kind, plugin=plugin)
        else:
            self.show_message("Select a track to add the device to.")

    def _plugin_added(self, track_id: str, device_id: str) -> None:
        # After the add is done (the track may be selected just after it, and a drop
        # finished): the editor opens when its track is shown.
        QTimer.singleShot(0, lambda: self.project.has_owner(track_id)
                          and self.bridge.request_plugin_editor(track_id, device_id))

    def _plugin_param_edited(self, track_id: str, device_id: str, param_id: str, value: float, old: float,
                             gesture: int) -> None:
        """A plug-in's own editor changed a parameter: an undo step (one per knob drag)."""
        if self.project.has_owner(track_id):
            self.editor.set_device_param(track_id, device_id, param_id, value,
                                         merge_key=("plugin edit", device_id, param_id, gesture), old=old)

    def delete_track(self) -> None:
        self.editor.delete_tracks([t for t in self.selection.track_ids if self.project.has_track(t)])

    def solo_selected_tracks(self) -> None:
        """S: solo the selected tracks (and unsolo the rest); if they all are already, unsolo every track."""
        tracks = [t for t in self.selection.track_ids if self.project.has_track(t)]
        if tracks:
            solo = not all(self.project.track(t).solo for t in tracks)
            if not solo:
                tracks = [t.id for t in self.project.tracks]
            self.editor.solo_tracks(tracks, solo, exclusive=True)

    def delete_selection(self) -> None:
        selection = self.selection
        if selection.focus == "devices":
            self.devices.delete_selected()  # not clips selected before: they aren't what the user is on
            return
        if selection.points is not None:
            owner, key, indices = selection.points
            self.editor.delete_automation_points(owner, key, indices)
            selection.select_points(owner, key, ())
        elif selection.time_range is not None and selection.lanes:
            self.editor.delete_automation_range(*selection.time_range[:2], list(selection.lanes))
        elif selection.time_range is not None:
            self.arrangement.lanes.delete_area()
        elif selection.focus == "track":
            self.delete_track()

    def duplicate(self) -> None:
        selection = self.selection
        if selection.time_range is not None and selection.lanes:
            start, end, track_ids = selection.time_range
            lanes = selection.lanes
            self.editor.duplicate_automation_range(start, end, list(lanes))
            selection.set_time_range(end, 2 * end - start, track_ids, lanes=lanes)  # the copy
            selection.set_insert(end)
            return
        self.arrangement.lanes.duplicate_area()

    def split(self) -> None:
        refs = sorted(self.selection.clips)
        if not refs and self.selection.track_id and self.project.has_track(self.selection.track_id):
            refs = self.editor.clips_at([self.selection.track_id], self.selection.insert_beat)
        if refs:
            self.editor.split_clips(refs, self.selection.insert_beat)

    def select_all(self) -> None:
        self.selection.select_clips(self.editor, [(t.id, c.id) for t in self.project.tracks for c in t.clips])

    def add_file_at_insert(self, path: str) -> None:
        info = self.bridge.file_info(path)
        if info is None:
            return
        track_id = self.selection.track_id if self.project.has_track(self.selection.track_id or "") else None
        refs = self.editor.add_clips(track_id, self.selection.insert_beat, [(path, info.duration)],
                                     track_index=len(self.project.tracks))
        if refs:
            self.selection.select_clips(self.editor, refs)

    def _focus_search(self) -> None:
        self.activateWindow()  # from a plug-in's editor
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
        self.selection.clear()
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
        self._add_recent(Path(path))
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
        self._add_recent(path)
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

    @staticmethod
    def recent_projects() -> list[str]:
        stored = QSettings().value(RECENT_KEY)
        if isinstance(stored, str):  # QSettings gives a one-item list back as a string
            stored = [stored]
        return [str(p) for p in stored] if isinstance(stored, list) else []

    @staticmethod
    def _set_recent(paths: list[str]) -> None:
        QSettings().setValue(RECENT_KEY, paths[:MAX_RECENT])

    def _add_recent(self, path: Path) -> None:
        entry = str(path.resolve())
        key = entry.casefold()
        self._set_recent([entry] + [p for p in self.recent_projects() if p.casefold() != key])

    def _fill_recent_menu(self) -> None:
        menu = self.recent_menu
        menu.clear()
        paths = self.recent_projects()
        if not paths:
            menu.addAction("No Recent Projects").setEnabled(False)
            return
        for i, path in enumerate(paths):
            name = Path(path).name.replace("&", "&&")  # a literal "&", not a mnemonic
            action = menu.addAction(f"&{i + 1}  {name}" if i < 9 else f"{i + 1}  {name}")
            action.setToolTip(path)
            action.setStatusTip(path)
            action.triggered.connect(lambda _checked=False, p=path: self._open_recent(p))
        menu.addSeparator()
        menu.addAction("&Clear List", lambda: self._set_recent([]))

    def _open_recent(self, path: str) -> None:
        if not Path(path).is_file():
            QMessageBox.warning(self, APP_NAME, f"{Path(path).name} can't be found. It was removed from the list.")
            self._set_recent([p for p in self.recent_projects() if p != path])
            return
        self.open_project(path)

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
        if self._plugin_shortcuts is not None:
            QApplication.instance().removeNativeEventFilter(self._plugin_shortcuts)
            self._plugin_shortcuts = None
        event.accept()

    def showEvent(self, event) -> None:
        super().showEvent(event)
        QTimer.singleShot(0, self.arrangement.lanes.setFocus)
