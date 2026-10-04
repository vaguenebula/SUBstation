"""Structured errors: callers branch on `code`, never on the message."""

from __future__ import annotations

NOT_FOUND = "not_found"  # a track, device, clip, file or operation that isn't there
INVALID = "invalid"  # arguments the operation can't take
CONFLICT = "conflict"  # the project changed since `if_revision`, or the edit can't be made as things are
DENIED = "denied"  # not permitted (by an actor's permissions)
BUSY = "busy"  # not now: recording, or the main thread didn't answer in time
FROZEN = "frozen"  # it would change what a frozen track's audio holds

CODES = (NOT_FOUND, INVALID, CONFLICT, DENIED, BUSY, FROZEN)


class OpError(Exception):
    """An operation that didn't run (or was rolled back): a `code` from CODES,
    a message for people, and details for programs."""

    def __init__(self, code: str, message: str, **details):
        if code not in CODES:
            raise ValueError(f"unknown error code {code!r}")
        super().__init__(message)
        self.code = code
        self.message = message
        self.details = details

    def to_dict(self) -> dict:
        error = {"code": self.code, "message": self.message}
        if self.details:
            error["details"] = self.details
        return {"error": error}


def not_found(what: str, **details) -> OpError:
    return OpError(NOT_FOUND, what, **details)


def invalid(message: str, **details) -> OpError:
    return OpError(INVALID, message, **details)


def from_exception(exc: BaseException) -> OpError | None:
    """What the editor's own exceptions mean to a caller: ValueError (an edit the
    routing can't take) is 'invalid', KeyError (an id that isn't there) 'not_found'."""
    if isinstance(exc, OpError):
        return exc
    if isinstance(exc, ValueError):
        return invalid(str(exc))
    if isinstance(exc, KeyError):
        return not_found(f"Not found: {exc.args[0] if exc.args else exc}")
    return None


def from_refusal(message: str) -> OpError:
    """An edit the editor refused (its `refused` signal): frozen audio, mostly."""
    return OpError(FROZEN if "frozen" in message.lower() else CONFLICT, message)
