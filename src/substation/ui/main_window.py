"""Main window: transport bar on top, browser left, arrangement centre, device
view at the bottom, menus and keyboard shortcuts."""

from __future__ import annotations

from dataclasses import replace
from pathlib import Path

from PySide6.QtCore import QSettings, Qt, QTimer
from PySide6.QtGui import QAction, QActionGroup, QCloseEvent, QKeySequence, QUndoStack
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
from ..audio.settings import (
    RECORD_QUANTIZE,
    AudioSettings,
    record_quantize,
    set_record_quantize,
)
from ..model import automation
from ..model.devices import device_is_instrument, is_instrument
from ..model.editor import ProjectEditor
from ..model.presets import default_device
from ..model.project import PLUGIN_KIND, PluginRef, Project
from ..model.serialization import (
    EXTENSION,
    ProjectFileError,
    load_preset,
    load_project,
    save_project,
)
from . import freezing, icons, plugin_keys
from .arrangement.arrangement_view import ArrangementView
from .arrangement.track_headers import duplicate_tracks
from .arrangement.view_state import Selection
from .browser.browser_panel import BrowserPanel
from .computer_keyboard import ComputerKeyboard
from .device_panel import DevicePanel
from .dialogs import ExportDialog, PreferencesDialog
from .transport_bar import TransportBar

PROJECT_FILTER = f"SUBstation Project (*{EXTENSION})"
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
        self.editor.set_param_info(self.bridge.device_param_info)  # (plug-ins' parameters, for macros)
        self.editor.set_own_value(self.bridge.own_value)  # (what a plug-in's editor set: to undo a macro to)
        self.editor.set_device_defaults(default_device)  # (new devices start as their default presets have them)
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
        self.browser.preset_activated.connect(self.add_preset_to_selected_track)
        self.devices.preset_saved.connect(lambda _path: self.browser.presets_changed())
        plugins = self.browser.plugin_index
        plugins.updated.connect(lambda: self.bridge.set_known_plugins(plugins.plugins))
        self.bridge.owner_window = lambda: int(self.winId())  # plug-in editors float above this window
        # Only the selected track's plug-in editors are shown; a new plug-in shows its editor.
        self.selection.changed.connect(lambda: self.bridge.show_plugin_editors(self.selection.track_id))
        self.editor.plugin_added.connect(self._plugin_added)
        self.editor.refused.connect(self.show_message)  # (an edit a frozen track can't take)
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
        self.computer_keyboard = ComputerKeyboard(self.bridge, self)
        self.transport.computer_keys.toggled.connect(self.computer_keyboard.set_enabled)
        self.computer_keyboard.changed.connect(self._show_computer_keyboard)

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
        self._action(edit, "Cu&t", self.cut, QKeySequence.StandardKey.Cut)
        self._action(edit, "&Copy", self.copy, QKeySequence.StandardKey.Copy)
        self._action(edit, "&Paste", self.paste, QKeySequence.StandardKey.Paste)
        self._action(edit, "D&uplicate", self.duplicate, "Ctrl+D")
        self._action(edit, "&Split", self.split, "Ctrl+E")
        self._action(edit, "C&onsolidate", self.arrangement.lanes.consolidate, "Ctrl+J")
        edit.addSeparator()
        self._action(edit, "&Freeze / Unfreeze Track", self.toggle_freeze, "Ctrl+Shift+F")
        self._action(edit, "Flatten Track", self.flatten_tracks)
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
        quantize = edit.addMenu("Record &Quantization")
        quantize.setToolTip("Where recorded MIDI notes start: as played, or on this grid")
        group = QActionGroup(self)
        self.record_quantize_actions = []
        for label, grid in RECORD_QUANTIZE:
            action = self._action(quantize, label, lambda on, g=grid: on and set_record_quantize(g), checkable=True,
                                  checked=abs(grid - record_quantize()) < 1e-9)
            group.addAction(action)
            self.record_quantize_actions.append(action)
        self._action(edit, "Go to Start", lambda: self.locate(0.0), "Home")
        self.loop_action = self._action(edit, "Loop", self.editor.set_loop_enabled, "Ctrl+L", checkable=True)
        self.project.settings_changed.connect(lambda: self._sync_check(self.loop_action, self.project.loop_enabled))
        self.project.reset.connect(lambda: self._sync_check(self.loop_action, self.project.loop_enabled))
        self._action(edit, "Find in Browser", self._focus_search, "Ctrl+F")

        create = bar.addMenu("&Create")
        self._action(create, "Insert Audio &Track", self.insert_track, "Ctrl+T")
        self._action(create, "Insert &MIDI Track", self.insert_midi_track, "Ctrl+Shift+T")
        self._action(create, "Insert &Return Track", self.insert_return_track, "Ctrl+Alt+T")
        self._action(create, "Insert MIDI &Clip", self.insert_midi_clip, ["Ctrl+Shift+D", "Ctrl+Shift+M"])
        create.addSeparator()
        self._action(create, "&Group Tracks", self.group_selected_tracks, "Ctrl+G")  # (devices, in the device view)
        self._action(create, "&Ungroup Tracks", self.ungroup_selected_tracks, "Ctrl+Shift+G")
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
        self.computer_keyboard_action = self._action(options, "Computer &MIDI Keyboard",
                                                      self.computer_keyboard.set_enabled, "M", checkable=True)
        options.addSeparator()
        self.lock_action = self._action(options, "&Lock Envelopes", self.editor.set_automation_locked, checkable=True)
        for signal in (self.project.settings_changed, self.project.reset):
            signal.connect(lambda: self._sync_check(self.lock_action, self.project.automation_locked))

        help_menu = bar.addMenu("&Help")
        self._action(help_menu, "&About SUBstation", self.show_about)

    def _show_computer_keyboard(self) -> None:
        keyboard = self.computer_keyboard
        self._sync_check(self.computer_keyboard_action, keyboard.enabled)
        self.transport.computer_keys.set_checked_silently(keyboard.enabled)
        self.transport.computer_keys.setToolTip(
            f"Computer MIDI Keyboard (M): A S D F... play the white keys from {keyboard.octave_label()}, "
            "W E T Y U... the black keys; Z and X change the octave")

    @staticmethod
    def _sync_check(action: QAction, checked: bool) -> None:
        if action.isChecked() != checked:
            action.blockSignals(True)
            action.setChecked(checked)
            action.blockSignals(False)

    # --- Audio device ------------------------------------------------------------

    def start_audio(self) -> None:
        self.bridge.apply_audio_threads()
        self.bridge.open_midi_inputs()
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
        refs = self.editor.add_recordings(takes, record_quantize())
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

    def _after_selected_track(self) -> dict:
        """Where a new track goes: after the selected one (and what is in it), in its group."""
        index, parent = self.editor.insertion_point(self.selection.track_id)
        return {"index": index, "parent": parent}

    def insert_track(self) -> None:
        track = self.editor.add_audio_track(**self._after_selected_track())
        self.selection.select_track(track.id, focus_track=True)

    def insert_midi_track(self) -> None:
        track = self.editor.add_midi_track(**self._after_selected_track())
        self.selection.select_track(track.id, focus_track=True)

    def insert_return_track(self) -> None:
        """Ctrl+Alt+T: a return track, after the others (or after the selected one); it is selected."""
        track_id = self.selection.track_id
        index = self.project.return_index(track_id) + 1 if track_id and self.project.has_return(track_id) else None
        track = self.editor.add_return_track(index)
        self.selection.select_track(track.id, focus_track=True)

    def group_selected_tracks(self) -> None:
        """Ctrl+G: the selected tracks go into a new group, which is selected; in
        the device view, the selected devices into a rack."""
        if self.selection.focus == "devices":
            if not self.devices.group_selected():
                self.show_message("Select the devices to group.")
            return
        group = self.editor.group_tracks([t for t in self.selection.track_ids if self.project.has_track(t)])
        if group is None:
            self.show_message("Select the tracks to group.")
        else:
            self.selection.select_track(group.id, focus_track=True)

    def ungroup_selected_tracks(self) -> None:
        """Ctrl+Shift+G: the selected groups go; what was in them stays (in the
        device view: the selected racks)."""
        if self.selection.focus == "devices":
            if not self.devices.ungroup_selected():
                self.show_message("Select a rack to ungroup.")
            return
        groups = [t for t in self.selection.track_ids if self.project.has_track(t) and self.project.track(t).is_group]
        if groups:
            self.editor.ungroup(groups)
        else:
            self.show_message("Select a group track to ungroup.")

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
            track = self.editor.add_midi_track(**self._after_selected_track(), instrument=None if plugin else kind,
                                               plugin=plugin)
            self.selection.select_track(track.id, focus_track=True)
        elif has_track:
            self.editor.add_device(track_id, kind, plugin=plugin)
        else:
            self.show_message("Select a track to add the device to.")

    def add_preset_to_selected_track(self, path: str) -> None:
        """A preset's device, new, on the selected track (an instrument with no MIDI track selected: on a new one)."""
        try:
            device = load_preset(Path(path))
        except ProjectFileError as exc:
            self.show_message(str(exc))
            return
        text = f"Load Preset {Path(path).stem}"
        track_id = self.selection.track_id
        has_track = bool(track_id) and self.project.has_owner(track_id)
        if device_is_instrument(device) and not (has_track and self.project.track(track_id).is_midi):
            track = self.editor.add_midi_track_with(device, **self._after_selected_track(), text=text)
            self.selection.select_track(track.id, focus_track=True)
        elif not has_track:
            self.show_message("Select a track to add the preset to.")
        elif not self.editor.insert_device(track_id, device, text=text, show_editors=not device.is_rack):
            self.show_message("The preset can't go there: racks nest at most 8 deep.")

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

    def toggle_freeze(self) -> None:
        """Ctrl+Shift+F: freeze the selected tracks (and returns), or unfreeze them if they all are."""
        changed = freezing.toggle_freeze(self.editor, self.bridge, self.selection.track_ids)
        if changed:
            names = ", ".join(self.project.track(t).name for t in changed)
            self.show_message(f"{'Froze' if self.project.is_frozen(changed[0]) else 'Unfroze'} {names}")

    def flatten_tracks(self) -> None:
        """The selected frozen tracks become audio tracks playing their frozen audio."""
        flat = freezing.flatten_tracks(self.editor, self.selection, self.selection.track_ids)
        if flat:
            self.show_message(f"Flattened {', '.join(self.project.track(t).name for t in flat)}")

    def delete_track(self) -> None:
        """The selected tracks (and return tracks)."""
        self.editor.delete_tracks([t for t in self.selection.track_ids
                                   if self.project.has_track(t) or self.project.has_return(t)])

    def solo_selected_tracks(self) -> None:
        """S: solo the selected tracks (and unsolo the rest); if they all are already, unsolo every track."""
        tracks = [t for t in self.selection.track_ids if self.project.has_track(t) or self.project.has_return(t)]
        if tracks:
            solo = not all(self.project.track(t).solo for t in tracks)
            if not solo:
                tracks = [t.id for t in self.project.senders()]
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

    def _what_is_copied(self, verb: str) -> str | None:
        """What Cut/Copy act on now: "devices", "automation" (a lane range), "clips"
        (a clip range), "tracks" or None (nothing they can; breakpoints, returns:
        says so)."""
        selection = self.selection
        if selection.focus == "devices":
            return "devices"
        if selection.focus == "track" and selection.time_range is None:
            if any(self.project.has_track(t) for t in selection.track_ids):
                return "tracks"
            self.show_message(f"Only the arrangement's tracks can be {verb}, not returns or the master.")
            return None
        if selection.time_range is not None and selection.lanes:
            return "automation"
        if selection.points is not None:
            self.show_message(f"Breakpoints can't be {verb}: select a time range on the automation lane.")
            return None
        return "clips" if selection.clip_range else None

    def cut(self) -> None:
        what = self._what_is_copied("cut")
        if what == "devices":
            self.devices.cut_selected()
        elif what == "automation":
            self.arrangement.lanes.cut_automation()
        elif what == "clips":
            self.arrangement.lanes.cut_area()
        elif what == "tracks":
            self.arrangement.lanes.cut_tracks(list(self.selection.track_ids))

    def copy(self) -> None:
        what = self._what_is_copied("copied")
        if what == "devices":
            self.devices.copy_selected()
        elif what == "automation":
            self.arrangement.lanes.copy_automation()
        elif what == "clips":
            self.arrangement.lanes.copy_area()
        elif what == "tracks":
            self.arrangement.lanes.copy_tracks(list(self.selection.track_ids))

    def paste(self) -> None:
        if self.selection.focus == "devices":
            self.devices.paste()
        else:
            self.arrangement.lanes.paste()

    def duplicate(self) -> None:
        selection = self.selection
        if selection.focus == "devices":
            self.devices.duplicate_selected()
            return
        if selection.time_range is not None and selection.lanes:
            start, end, track_ids = selection.time_range
            lanes = selection.lanes
            self.editor.duplicate_automation_range(start, end, list(lanes))
            selection.set_time_range(end, 2 * end - start, track_ids, lanes=lanes)  # the copy
            selection.set_insert(end)
            return
        if selection.focus == "track" and selection.time_range is None:
            tracks = [t for t in selection.track_ids if self.project.has_track(t)]
            if tracks:
                duplicate_tracks(self.editor, self.bridge, selection, tracks)
            else:
                self.show_message("Only the arrangement's tracks can be duplicated, not returns or the master.")
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
            self.bridge.wait_for_device_states()  # samples still loading
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
