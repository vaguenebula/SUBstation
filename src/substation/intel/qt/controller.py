"""IntelController: the layer's one Qt-facing object. The main window makes one
with its project, editor, bridge and UiFacts; suggestions, the assistant and
the MCP server reach the layer through it.

It owns the revision counter, the activity log, the SongContext builder, the
operations runner (on the bridge's EngineFacts) and the main-thread
dispatcher. Calls from the main thread run directly; calls from other threads
go through the dispatcher (call_from_thread)."""

from __future__ import annotations

from PySide6.QtCore import QObject, Signal

from ...audio.engine_bridge import EngineBridge
from ...model.editor import ProjectEditor
from ...model.project import Project
from ..activity import ActivityEntry, ActivityLog
from ..context.revision import IntelRevision
from ..context.song import SongContext, SongContextBuilder
from ..facts import UiFacts
from ..ops import OpRunner
from ..ops.context import USER
from .dispatcher import DEFAULT_TIMEOUT, MainThreadDispatcher
from .engine_facts import BridgeFacts


class IntelController(QObject):
    activity_added = Signal(object)  # ActivityEntry: an actor other than the user did something
    revision_changed = Signal(int)

    def __init__(self, project: Project, editor: ProjectEditor, bridge: EngineBridge | None = None,
                 ui: UiFacts | None = None, activity: ActivityLog | None = None, parent: QObject | None = None):
        super().__init__(parent)
        self.project = project
        self.engine = BridgeFacts(bridge) if bridge is not None else None
        self.ui = ui
        self.revision = IntelRevision(project)
        self.revision.subscribe(self.revision_changed.emit)
        self.activity = activity if activity is not None else ActivityLog()
        self.activity.subscribe(self._on_activity)
        self.builder = SongContextBuilder(project, self.revision, plugin_categories=self._plugin_categories)
        self.runner = OpRunner(project, editor, self.engine, ui, self.revision, self.activity, self.builder)
        self.dispatcher = MainThreadDispatcher(self)

    def _on_activity(self, entry: ActivityEntry) -> None:
        self.activity_added.emit(entry)

    def _plugin_categories(self) -> dict[str, str]:
        return {p.uid: p.category for p in self.ui.available_plugins()} if self.ui is not None else {}

    # --- On the main thread ------------------------------------------------------------------

    def context(self) -> SongContext:
        return self.builder.context()

    def run(self, name: str, args: dict | None = None, actor: str = USER) -> dict:
        """Run an operation (raises OpError)."""
        return self.runner.run(name, args, actor)

    def call(self, name: str, args: dict | None = None, actor: str = USER) -> dict:
        """Run an operation; an error comes back as {"error": ...}."""
        return self.runner.call(name, args, actor)

    def run_batch(self, calls: list[dict], label: str | None = None, actor: str = USER,
                  if_revision: int | None = None) -> dict:
        return self.runner.run_batch(calls, label, actor, if_revision)

    # --- From any thread ------------------------------------------------------------------------

    def call_from_thread(self, name: str, args: dict | None = None, actor: str = USER,
                         timeout: float = DEFAULT_TIMEOUT) -> dict:
        """An operation run on the main thread for a caller on another one (the MCP
        server, a worker); the error 'busy' if the main thread doesn't get to it in time."""
        return self.dispatcher.call(lambda: self.runner.call(name, args, actor), timeout)
