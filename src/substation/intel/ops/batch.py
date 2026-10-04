"""The batch: several operations as one undo step, all or nothing. A suggestion
is a batch; agents get it as the `batch` tool.

A call's argument may use an earlier call's result: a string "$<index>.<path>"
(as "$0.track.id": the id of the track the first call made) stands for that
value."""

from __future__ import annotations

import re
from typing import Annotated

from . import registry
from .context import OpContext
from .errors import OpError, from_exception, from_refusal, invalid, not_found
from .registry import Doc, MaxLen, MinLen, Risk, operation

_REFERENCE = re.compile(r"\$(\d+)((?:\.[A-Za-z0-9_]+)+)")


def _resolve(value, results: list[dict]):
    if isinstance(value, str) and (match := _REFERENCE.fullmatch(value)):
        index = int(match.group(1))
        if index >= len(results):
            raise invalid(f"{value} refers to a call that hasn't run before it")
        found = results[index]
        for part in match.group(2)[1:].split("."):
            try:
                found = found[int(part)] if isinstance(found, list) else found[part]
            except (KeyError, IndexError, ValueError, TypeError):
                raise invalid(f"{value}: the result has no {part!r}") from None
        return found
    if isinstance(value, dict):
        return {k: _resolve(v, results) for k, v in value.items()}
    if isinstance(value, list):
        return [_resolve(v, results) for v in value]
    return value


@operation(risk=Risk.EDIT, name="batch", label="Batch",
           summary="Run several operations as one undo step, all or nothing: if one fails, none of them happened. "
                   "Each call is {\"op\": name, \"args\": {...}}; an argument \"$<i>.<path>\" (\"$0.track.id\") "
                   "takes a value from call i's result. Transport operations can't be in a batch.")
def batch(ctx: OpContext,
          calls: Annotated[list[dict], MinLen(1), MaxLen(256), Doc("The calls: {\"op\": name, \"args\": {...}}.")],
          label: Annotated[str | None, MaxLen(64), Doc("What the undo step is called.")] = None) -> dict:
    results: list[dict] = []
    refusals: list[str] = []
    refused = refusals.append
    ctx.editor.refused.connect(refused)
    try:
        for index, call in enumerate(calls):
            name = call.get("op")
            if not isinstance(name, str) or set(call) - {"op", "args"}:
                raise invalid(f"calls[{index}] must be {{\"op\": name, \"args\": {{...}}}}", index=index)
            op = registry.get(name)
            if op is None:
                raise not_found(f"calls[{index}]: there is no operation {name!r}", index=index)
            if op.name == "batch" or op.risk == Risk.TRANSPORT or not (op.undoable or op.is_read):
                raise invalid(f"calls[{index}]: {name} can't be in a batch", index=index)
            try:
                args = _resolve(call.get("args") or {}, results)
                results.append(dict(op.func(ctx, **op.bind(args)) or {}))
                if refusals:
                    raise from_refusal(refusals[0])
            except (OpError, ValueError, KeyError) as exc:
                error = from_refusal(refusals[0]) if refusals else from_exception(exc)
                error.details.setdefault("index", index)
                error.message = f"calls[{index}] ({name}): {error.message}"
                error.args = (error.message,)
                raise error from exc
    finally:
        ctx.editor.refused.disconnect(refused)
    return {"results": results}
