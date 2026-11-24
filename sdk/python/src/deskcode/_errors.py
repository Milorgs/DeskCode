"""Errors raised by the DeskCode SDK."""
from __future__ import annotations

from typing import Any, Iterable, Optional


class DeskError(Exception):
    """Base error: desk could not be started or produced no usable result."""

    def __init__(self, message: str, *, stderr: str = "", returncode: Optional[int] = None) -> None:
        super().__init__(message)
        self.stderr = stderr
        self.returncode = returncode


class DeskTimeout(DeskError):
    """``run()`` exceeded its timeout."""


class AcpError(DeskError):
    """The agent answered a JSON-RPC request with an error object."""

    def __init__(self, code: int, message: str, data: Any = None) -> None:
        super().__init__(f"ACP error {code}: {message}")
        self.code = code
        self.data = data


class AgentExited(DeskError):
    """The ``desk acp`` process exited (or closed stdout) while work was pending."""


def redact(text: str, secrets: Iterable[Optional[str]]) -> str:
    for secret in sorted((s for s in secrets if s), key=len, reverse=True):
        text = text.replace(secret, "***")
    return text


def summarize(text: str, limit: int = 2000) -> str:
    text = text.strip()
    return text if len(text) <= limit else "…" + text[-limit:]
