"""The operations registry: every action the layer can take or describe is one
operation, with a name, a risk class, a summary and a function. Its arguments'
JSON schema comes from the function's type hints, so it can't drift from the
code, and the same types validate (and convert) the arguments a caller sends.

    @operation(risk=Risk.COSMETIC, summary="Rename a track, group or return.")
    def rename_track(ctx: OpContext, track_id: TrackId, name: Annotated[str, MinLen(1), MaxLen(64)]) -> dict:
        ...

The first parameter is the OpContext; the others are the arguments. Supported
types: str, int, float, bool, Literal[...], list[T], X | None, dict (a free
object), each optionally Annotated with Doc, MinLen, MaxLen, Ge, Le and
Pattern. Schemas are strict (additionalProperties: false)."""

from __future__ import annotations

import functools
import inspect
import math
import operator
import re
import types
import typing
from collections.abc import Callable
from dataclasses import dataclass
from enum import StrEnum
from typing import Annotated, Any, Literal, Union, get_args, get_origin

from .errors import invalid


class Risk(StrEnum):
    """What an operation can do, which decides who may run it without asking."""

    READ = "read"  # looks, changes nothing
    COSMETIC = "cosmetic"  # names and colours: never changes the sound
    EDIT = "edit"  # changes the song, undoably
    DESTRUCTIVE = "destructive"  # deletes or flattens: undoable, but never automatic
    TRANSPORT = "transport"  # plays, stops, locates: not an undo step
    FILES = "files"  # reads or writes files outside the project


# --- Constraints for Annotated ----------------------------------------------------------


@dataclass(frozen=True)
class Doc:
    text: str


@dataclass(frozen=True)
class MinLen:
    n: int


@dataclass(frozen=True)
class MaxLen:
    n: int


@dataclass(frozen=True)
class Ge:
    value: float


@dataclass(frozen=True)
class Le:
    value: float


@dataclass(frozen=True)
class Pattern:
    regex: str


_MISSING = inspect.Parameter.empty


# --- Schema and validation from types ------------------------------------------------------


def _split(tp) -> tuple[Any, tuple]:
    """A type and its Annotated metadata."""
    if get_origin(tp) is Annotated:
        base, *meta = get_args(tp)
        inner, more = _split(base)
        return inner, (*more, *meta)
    return tp, ()


def _optional(tp) -> tuple[Any, bool]:
    """X | None -> (X, True)."""
    if get_origin(tp) in (Union, types.UnionType):
        args = [a for a in get_args(tp) if a is not type(None)]
        if len(args) != len(get_args(tp)):
            return functools.reduce(operator.or_, args), True
    return tp, False


def type_schema(tp) -> dict:
    tp, meta = _split(tp)
    tp, nullable = _optional(tp)
    tp, inner_meta = _split(tp)
    meta = (*inner_meta, *meta)
    origin = get_origin(tp)
    if tp is str:
        schema: dict = {"type": "string"}
    elif tp is bool:
        schema = {"type": "boolean"}
    elif tp is int:
        schema = {"type": "integer"}
    elif tp is float:
        schema = {"type": "number"}
    elif tp is dict or origin is dict:
        schema = {"type": "object"}
    elif origin is Literal:
        values = list(get_args(tp))
        kind = "string" if all(isinstance(v, str) for v in values) else "number"
        schema = {"type": kind, "enum": values}
    elif origin is list:
        (item,) = get_args(tp)
        schema = {"type": "array", "items": type_schema(item)}
    else:
        raise TypeError(f"no schema for {tp!r}")
    for m in meta:
        if isinstance(m, Doc):
            schema["description"] = m.text
        elif isinstance(m, MinLen):
            schema["minItems" if schema["type"] == "array" else "minLength"] = m.n
        elif isinstance(m, MaxLen):
            schema["maxItems" if schema["type"] == "array" else "maxLength"] = m.n
        elif isinstance(m, Ge):
            schema["minimum"] = m.value
        elif isinstance(m, Le):
            schema["maximum"] = m.value
        elif isinstance(m, Pattern):
            schema["pattern"] = m.regex
    if nullable:
        schema["type"] = [schema["type"], "null"]
        if "enum" in schema:
            schema["enum"] = [*schema["enum"], None]
    return schema


def coerce(tp, value, path: str):
    """`value` (from JSON) as `tp`, or OpError invalid saying what is wrong where."""
    tp, meta = _split(tp)
    tp, nullable = _optional(tp)
    tp, inner_meta = _split(tp)
    meta = (*inner_meta, *meta)
    if value is None:
        if nullable:
            return None
        raise invalid(f"{path} is required, not null")
    origin = get_origin(tp)
    if tp is str:
        if not isinstance(value, str):
            raise invalid(f"{path} must be a string")
        result = value
    elif tp is bool:
        if not isinstance(value, bool):
            raise invalid(f"{path} must be true or false")
        result = value
    elif tp is int:
        if isinstance(value, float) and value.is_integer():
            value = int(value)
        if isinstance(value, bool) or not isinstance(value, int):
            raise invalid(f"{path} must be a whole number")
        result = value
    elif tp is float:
        if isinstance(value, bool) or not isinstance(value, int | float):
            raise invalid(f"{path} must be a number")
        result = float(value)
        if not math.isfinite(result):
            raise invalid(f"{path} must be a finite number")
    elif tp is dict or origin is dict:
        if not isinstance(value, dict):
            raise invalid(f"{path} must be an object")
        result = value
    elif origin is Literal:
        if value not in get_args(tp) or isinstance(value, bool) != any(isinstance(a, bool) for a in get_args(tp)):
            raise invalid(f"{path} must be one of {', '.join(map(repr, get_args(tp)))}")
        result = value
    elif origin is list:
        if not isinstance(value, list | tuple):
            raise invalid(f"{path} must be a list")
        (item,) = get_args(tp)
        result = [coerce(item, v, f"{path}[{i}]") for i, v in enumerate(value)]
    else:
        raise TypeError(f"can't check {tp!r}")
    for m in meta:
        size = len(result) if isinstance(result, str | list) else None
        if isinstance(m, MinLen) and size is not None and size < m.n:
            raise invalid(f"{path} is too short (at least {m.n})")
        if isinstance(m, MaxLen) and size is not None and size > m.n:
            raise invalid(f"{path} is too long (at most {m.n})")
        if isinstance(m, Ge) and result < m.value:
            raise invalid(f"{path} must be at least {m.value:g}")
        if isinstance(m, Le) and result > m.value:
            raise invalid(f"{path} must be at most {m.value:g}")
        if isinstance(m, Pattern) and not re.fullmatch(m.regex, result):
            raise invalid(f"{path} doesn't have the form {m.regex}")
    return result


# --- Operations --------------------------------------------------------------------------


@dataclass(frozen=True)
class Param:
    name: str
    type: Any
    default: Any = _MISSING

    @property
    def required(self) -> bool:
        return self.default is _MISSING


IF_REVISION_SCHEMA = {
    "type": ["integer", "null"],
    "description": "The project revision this edit is based on (from a read). If the project has changed "
                   "since, nothing is done and the error is 'conflict'.",
}


@dataclass(frozen=True)
class Operation:
    name: str
    risk: Risk
    summary: str
    func: Callable
    params: tuple[Param, ...]
    label: str  # the undo step's text ("Rename Track")
    undoable: bool  # an undo step (else: transport, view, engine state)
    idempotent: bool = False

    @property
    def is_read(self) -> bool:
        return self.risk == Risk.READ

    @property
    def schema(self) -> dict:
        """Its arguments' JSON schema (strict)."""
        properties = {}
        for p in self.params:
            schema = type_schema(p.type)
            if not p.required and p.default is not None:
                schema["default"] = p.default
            properties[p.name] = schema
        if not self.is_read:
            properties["if_revision"] = dict(IF_REVISION_SCHEMA)
        return {"type": "object", "properties": properties,
                "required": [p.name for p in self.params if p.required], "additionalProperties": False}

    def bind(self, args: dict | None) -> dict:
        """The function's keyword arguments from a caller's `args`, checked."""
        if args is not None and not isinstance(args, dict):
            raise invalid("arguments must be an object")
        args = dict(args or {})
        known = {p.name for p in self.params}
        unknown = sorted(set(args) - known)
        if unknown:
            raise invalid(f"{self.name} takes no argument {unknown[0]!r}", unknown=unknown)
        bound = {}
        for p in self.params:
            if p.name in args:
                bound[p.name] = coerce(p.type, args[p.name], p.name)
            elif p.required:
                raise invalid(f"{self.name} needs {p.name}")
        return bound


REGISTRY: dict[str, Operation] = {}


def _label(name: str) -> str:
    return " ".join(word.capitalize() for word in name.split("_"))


def operation(*, risk: Risk, summary: str, name: str | None = None, label: str | None = None,
              undoable: bool | None = None, idempotent: bool = False):
    """Registers a function as an operation (see the module's docstring)."""

    def register(func: Callable) -> Callable:
        op_name = name or func.__name__
        if op_name in REGISTRY:
            raise ValueError(f"operation {op_name!r} is registered twice")
        hints = typing.get_type_hints(func, include_extras=True)
        signature = inspect.signature(func)
        params = []
        for index, (pname, parameter) in enumerate(signature.parameters.items()):
            if index == 0:
                continue  # the OpContext
            if pname not in hints:
                raise TypeError(f"{op_name}: {pname} has no type")
            type_schema(hints[pname])  # (fails now rather than when asked)
            params.append(Param(pname, hints[pname], parameter.default))
        REGISTRY[op_name] = Operation(
            name=op_name, risk=risk, summary=summary, func=func, params=tuple(params),
            label=label or _label(op_name),
            undoable=undoable if undoable is not None else risk not in (Risk.READ, Risk.TRANSPORT),
            idempotent=idempotent)
        return func

    return register


def get(name: str) -> Operation | None:
    return REGISTRY.get(name)


def schemas() -> dict[str, dict]:
    """Every operation's name -> {risk, summary, schema}, sorted by name."""
    return {name: {"risk": str(op.risk), "summary": op.summary, "schema": op.schema}
            for name, op in sorted(REGISTRY.items())}
