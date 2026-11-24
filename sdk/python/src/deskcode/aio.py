"""asyncio wrappers around the thread-based ACP client.

The protocol work happens in :class:`deskcode.acp.DeskAgent` background threads; these
wrappers only move blocking calls off the event loop with :func:`asyncio.to_thread`.
"""
from __future__ import annotations

import asyncio
import os
from typing import Any, AsyncIterator, Dict, List, Mapping, Optional, Sequence, Union

from ._run import DeskPath
from .acp import (DeskAgent, Event, Mode, PermissionHandler, Session, TurnEnd, TurnResult)

__all__ = ["AsyncDeskAgent", "AsyncSession"]

_END = object()


class AsyncSession:
    def __init__(self, session: Session) -> None:
        self._session = session

    @property
    def session_id(self) -> str:
        return self._session.session_id

    @property
    def current_mode(self) -> Optional[str]:
        return self._session.current_mode

    @property
    def available_modes(self) -> List[Mode]:
        return self._session.available_modes

    @property
    def sync(self) -> Session:
        return self._session

    async def set_mode(self, mode_id: str) -> None:
        await asyncio.to_thread(self._session.set_mode, mode_id)

    async def cancel(self) -> None:
        await asyncio.to_thread(self._session.cancel)

    async def prompt(self, text: str = "", resources: Optional[Sequence[Any]] = None,
                     blocks: Optional[Sequence[Mapping[str, Any]]] = None) -> AsyncIterator[Event]:
        iterator = self._session.prompt(text, resources, blocks)
        try:
            while True:
                event = await asyncio.to_thread(next, iterator, _END)
                if event is _END:
                    return
                yield event  # type: ignore[misc]
                if isinstance(event, TurnEnd):
                    return
        finally:
            await asyncio.to_thread(iterator.close)

    async def send(self, text: str = "", resources: Optional[Sequence[Any]] = None,
                   blocks: Optional[Sequence[Mapping[str, Any]]] = None) -> TurnResult:
        return await asyncio.to_thread(self._session.send, text, resources, blocks)


class AsyncDeskAgent:
    """``async with AsyncDeskAgent() as agent: session = await agent.new_session(...)``"""

    def __init__(self, desk_path: DeskPath = "desk", env: Optional[Mapping[str, str]] = None,
                 cwd: Optional[Union[str, "os.PathLike[str]"]] = None,
                 on_permission: Optional[PermissionHandler] = None, **kwargs: Any) -> None:
        self.sync = DeskAgent(desk_path, env=env, cwd=cwd, on_permission=on_permission, **kwargs)

    @property
    def agent_info(self) -> Dict[str, Any]:
        return self.sync.agent_info

    async def __aenter__(self) -> "AsyncDeskAgent":
        await asyncio.to_thread(self.sync.start)
        return self

    async def __aexit__(self, *exc: Any) -> None:
        await asyncio.to_thread(self.sync.close)

    async def new_session(self, cwd: Optional[Union[str, "os.PathLike[str]"]] = None, mode: Optional[str] = "ask",
                          mcp_servers: Optional[List[Dict[str, Any]]] = None,
                          on_permission: Optional[PermissionHandler] = None) -> AsyncSession:
        session = await asyncio.to_thread(self.sync.new_session, cwd, mode, mcp_servers, on_permission)
        return AsyncSession(session)
