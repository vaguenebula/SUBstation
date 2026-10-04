"""The operations registry and its operations: every action the layer can take
or describe, typed, schema'd, permissioned (by risk class) and undoable.
Suggestions, the assistant's tools and agents' MCP tools all come from it.

Importing this package registers every operation (one module per area);
OpRunner runs them."""

from . import automation, batch, clips, devices, freezing, midi, presets, project, reads, tracks  # noqa: F401
from .context import ASSISTANT, AUTO, USER, OpContext, agent, undo_label
from .errors import CODES, OpError
from .registry import REGISTRY, Operation, Risk, operation, schemas
from .runner import OpRunner

__all__ = [
    "ASSISTANT",
    "AUTO",
    "CODES",
    "REGISTRY",
    "USER",
    "OpContext",
    "OpError",
    "OpRunner",
    "Operation",
    "Risk",
    "agent",
    "operation",
    "schemas",
    "undo_label",
]
