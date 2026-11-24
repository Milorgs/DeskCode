"""DeskCode Python SDK.

* :func:`run` / :func:`arun` – one-shot ``desk --json -p`` calls.
* :class:`DeskAgent` / :class:`AsyncDeskAgent` – streaming sessions over the Agent Client
  Protocol (``desk acp``).
"""
from ._errors import AcpError, AgentExited, DeskError, DeskTimeout
from ._run import RunResult, ToolRecord, Usage, arun, run
from ._version import __version__
from .acp import (DeskAgent, DiffContent, Event, Location, Mode, ModeChange, PermissionHandler,
                  PermissionOption, PermissionRequest, PlanEntry, PlanUpdate, Session, TextChunk,
                  TextContent, ToolCall, ToolCallUpdate, TurnEnd, TurnResult, UnknownUpdate,
                  allow_all, allow_reads_only, embedded_resource, reject_all)
from .aio import AsyncDeskAgent, AsyncSession

__all__ = [
    "__version__", "run", "arun", "RunResult", "Usage", "ToolRecord",
    "DeskError", "DeskTimeout", "AcpError", "AgentExited",
    "DeskAgent", "Session", "AsyncDeskAgent", "AsyncSession",
    "Event", "TextChunk", "ToolCall", "ToolCallUpdate", "PlanUpdate", "ModeChange", "UnknownUpdate",
    "TurnEnd", "TurnResult", "Location", "TextContent", "DiffContent", "PlanEntry", "Mode",
    "PermissionRequest", "PermissionOption", "PermissionHandler",
    "allow_all", "reject_all", "allow_reads_only", "embedded_resource",
]
