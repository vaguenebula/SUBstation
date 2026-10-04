"""Running operations: the one path from a caller (a suggestion, the assistant,
an agent, a test) to the ProjectEditor.

For each call the runner:
1. finds the operation and checks its arguments against its schema ('invalid');
2. for an edit, checks `if_revision` against the project's revision
   ('conflict') and that nothing is recording ('busy');
3. runs it inside one undo macro labelled for its actor ("Agent (Claude Code):
   Add Device"), so an operation, or a batch, is one undo step whatever the
   editor pushes for it;
4. if it fails (an error, or the editor refusing an edit: 'frozen'), undoes
   the macro and takes it off the stack: all or nothing, and no trace;
   if it changed nothing, takes the empty step off the stack too;
5. adds `changed` and the project's `revision` to the result, and logs it
   (actors other than the user, edits only) in the activity log.

It runs on the thread the project lives on (the main thread); other threads
come through qt/dispatcher.py."""

from __future__ import annotations

from collections import OrderedDict

from ...model.editor import ProjectEditor
from ...model.project import Project
from ..activity import ActivityLog
from ..context.revision import IntelRevision
from ..facts import EngineFacts, UiFacts
from . import registry
from .context import USER, OpContext, undo_label
from .errors import BUSY, CONFLICT, OpError, from_exception, from_refusal, not_found
from .registry import Risk


class OpRunner:
    def __init__(self, project: Project, editor: ProjectEditor, engine: EngineFacts | None = None,
                 ui: UiFacts | None = None, revision: IntelRevision | None = None,
                 activity: ActivityLog | None = None, builder=None):
        self.project = project
        self.editor = editor
        self.engine = engine
        self.ui = ui
        self.revision = revision if revision is not None else IntelRevision(project)
        self.activity = activity
        self.builder = builder  # a SongContextBuilder (reads of the song reuse what it keeps)
        self.selections: OrderedDict[str, dict] = OrderedDict()  # pinned selection snapshots (get_selection)

    def context(self, actor: str = USER) -> OpContext:
        return OpContext(self.project, self.editor, actor, self.engine, self.ui, self.revision, self.selections,
                         self.builder)

    # --- Running -----------------------------------------------------------------------------

    def run(self, name: str, args: dict | None = None, actor: str = USER) -> dict:
        """Run an operation; its result (with `changed` and `revision`). Raises OpError."""
        op = registry.get(name)
        if op is None:
            raise not_found(f"There is no operation {name!r}", op=name)
        args = dict(args or {}) if isinstance(args, dict | None) else args
        try:
            if_revision = args.pop("if_revision", None) if isinstance(args, dict) and not op.is_read else None
            kwargs = op.bind(args)
            label = op.label
            if op.name == "batch" and kwargs.get("label"):
                label = kwargs["label"]
            result = self._execute(op, label, actor, kwargs, if_revision)
        except OpError as error:
            error.details.setdefault("revision", self.revision.value)  # (a rollback moves it on)
            self._log(actor, op, args, error.to_dict())
            raise
        self._log(actor, op, args, result)
        return result

    def call(self, name: str, args: dict | None = None, actor: str = USER) -> dict:
        """As run, but an error comes back as {"error": {code, message, details}} (for transports)."""
        try:
            return self.run(name, args, actor)
        except OpError as error:
            return error.to_dict()

    def run_batch(self, calls: list[dict], label: str | None = None, actor: str = USER,
                  if_revision: int | None = None) -> dict:
        """Several operations ({"op": name, "args": {...}}) as one undo step, all or nothing."""
        args: dict = {"calls": calls}
        if label:
            args["label"] = label
        if if_revision is not None:
            args["if_revision"] = if_revision
        return self.run("batch", args, actor)

    # --- Inside -----------------------------------------------------------------------------

    def _execute(self, op: registry.Operation, label: str, actor: str, kwargs: dict, if_revision: int | None) -> dict:
        if if_revision is not None and if_revision != self.revision.value:
            raise OpError(CONFLICT, f"The project changed since revision {if_revision} (it is at "
                                    f"{self.revision.value} now): read it again", revision=self.revision.value)
        if op.risk not in (Risk.READ, Risk.TRANSPORT) and self._recording():
            raise OpError(BUSY, "Not while recording")
        ctx = self.context(actor)
        stack = self.editor.undo_stack
        before = self.revision.value
        refusals: list[str] = []
        refused = refusals.append
        self.editor.refused.connect(refused)
        macro = op.undoable
        if macro:
            stack.beginMacro(undo_label(actor, label))
        try:
            try:
                result = op.func(ctx, **kwargs)
                if refusals:
                    raise from_refusal(refusals[0])
            except (OpError, ValueError, KeyError) as exc:
                # An edit the editor refused is why it failed, whatever the operation made of it.
                raise (from_refusal(refusals[0]) if refusals else from_exception(exc)) from exc
        except BaseException:
            if macro:
                stack.endMacro()
                _drop_last(stack)
            raise
        else:
            if macro:
                stack.endMacro()
                if self.revision.value == before or stack.command(stack.index() - 1).childCount() == 0:
                    _drop_last(stack)  # no empty undo step (nothing changed, or only what isn't undone: solo)
        finally:
            self.editor.refused.disconnect(refused)
        result = dict(result or {})
        if not op.is_read:
            result["changed"] = self.revision.value != before
        result["revision"] = self.revision.value
        return result

    def _recording(self) -> bool:
        return self.engine is not None and self.engine.transport().recording

    def _log(self, actor: str, op: registry.Operation, args, result: dict) -> None:
        if self.activity is None or actor == USER or op.is_read:
            return
        changed = result.get("changed")
        index = self.editor.undo_stack.index() if changed and op.undoable else None
        self.activity.record(actor, op.name, args if isinstance(args, dict) else {"args": args}, result, index)


def _drop_last(stack) -> None:
    """Undo the last step and take it off the stack (no redo left behind): an
    obsolete command is deleted rather than redone."""
    stack.undo()
    command = stack.command(stack.index())
    command.setObsolete(True)
    stack.redo()
