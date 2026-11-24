"""Agent Client Protocol (ACP) client for ``desk acp``.

Transport: JSON-RPC 2.0 over the child's stdio, one JSON object per line (no headers).
"""
from __future__ import annotations

import itertools
import json
import logging
import os
import queue
import subprocess
import threading
import collections
from dataclasses import dataclass, field
from pathlib import Path
from typing import (Any, Callable, Deque, Dict, Iterator, List, Mapping, Optional, Sequence, Union)

from ._errors import AcpError, AgentExited, DeskError, redact, summarize
from ._run import DeskPath, command_prefix
from ._version import __version__

logger = logging.getLogger("deskcode")

PROTOCOL_VERSION = 1
METHOD_NOT_FOUND = -32601
READ_ONLY_KINDS = frozenset({"read", "search", "think"})

__all__ = [
    "Location", "TextContent", "DiffContent", "PlanEntry", "Mode",
    "TextChunk", "ToolCall", "ToolCallUpdate", "PlanUpdate", "ModeChange", "UnknownUpdate", "TurnEnd",
    "Event", "TurnResult", "PermissionOption", "PermissionRequest", "PermissionHandler",
    "allow_all", "reject_all", "allow_reads_only", "DeskAgent", "Session",
]


# --------------------------------------------------------------------------- data types


@dataclass
class Location:
    path: str
    line: Optional[int] = None


@dataclass
class TextContent:
    text: str


@dataclass
class DiffContent:
    path: str
    old_text: Optional[str]
    new_text: str


ToolContent = Union[TextContent, DiffContent, Dict[str, Any]]


@dataclass
class PlanEntry:
    content: str
    priority: str = "medium"
    status: str = "pending"


@dataclass
class Mode:
    id: str
    name: str = ""
    description: str = ""


@dataclass
class TextChunk:
    text: str
    raw: Dict[str, Any] = field(default_factory=dict, repr=False)


@dataclass
class ToolCall:
    tool_call_id: str
    title: str = ""
    kind: str = "other"
    status: str = "pending"
    raw_input: Any = None
    locations: List[Location] = field(default_factory=list)
    content: List[ToolContent] = field(default_factory=list)
    raw_output: Any = None
    raw: Dict[str, Any] = field(default_factory=dict, repr=False)

    @classmethod
    def from_json(cls, data: Mapping[str, Any]) -> "ToolCall":
        return cls(tool_call_id=str(data.get("toolCallId", "")), title=str(data.get("title") or ""),
                   kind=str(data.get("kind") or "other"), status=str(data.get("status") or "pending"),
                   raw_input=data.get("rawInput"), locations=_locations(data.get("locations")),
                   content=_contents(data.get("content")), raw_output=data.get("rawOutput"), raw=dict(data))


@dataclass
class ToolCallUpdate:
    tool_call_id: str
    status: Optional[str] = None
    content: List[ToolContent] = field(default_factory=list)
    raw_output: Any = None
    title: Optional[str] = None
    locations: List[Location] = field(default_factory=list)
    raw: Dict[str, Any] = field(default_factory=dict, repr=False)

    @property
    def text(self) -> str:
        return "".join(c.text for c in self.content if isinstance(c, TextContent))

    @property
    def diffs(self) -> List[DiffContent]:
        return [c for c in self.content if isinstance(c, DiffContent)]


@dataclass
class PlanUpdate:
    entries: List[PlanEntry]
    raw: Dict[str, Any] = field(default_factory=dict, repr=False)


@dataclass
class ModeChange:
    mode_id: str


@dataclass
class UnknownUpdate:
    """A ``session/update`` kind this SDK does not know yet (forward compatibility)."""
    kind: str
    raw: Dict[str, Any] = field(default_factory=dict)


@dataclass
class TurnEnd:
    stop_reason: str


Event = Union[TextChunk, ToolCall, ToolCallUpdate, PlanUpdate, ModeChange, UnknownUpdate, TurnEnd]


@dataclass
class TurnResult:
    text: str
    stop_reason: str
    tool_calls: List[ToolCall] = field(default_factory=list)
    plan: List[PlanEntry] = field(default_factory=list)
    events: List[Event] = field(default_factory=list, repr=False)


@dataclass
class PermissionOption:
    option_id: str
    name: str = ""
    kind: str = ""


@dataclass
class PermissionRequest:
    session_id: str
    tool_call: ToolCall
    options: List[PermissionOption]
    raw: Dict[str, Any] = field(default_factory=dict, repr=False)

    def option(self, *kinds: str) -> Optional[str]:
        """Return the id of the first option whose kind is in ``kinds`` (in preference order)."""
        for kind in kinds:
            for opt in self.options:
                if opt.kind == kind:
                    return opt.option_id
        return None


PermissionHandler = Callable[[PermissionRequest], Optional[str]]


def allow_all(request: PermissionRequest) -> Optional[str]:
    """Allow every tool call once."""
    return request.option("allow_once", "allow_always")


def reject_all(request: PermissionRequest) -> Optional[str]:
    """Reject every tool call (the default policy)."""
    return request.option("reject_once", "reject_always")


def allow_reads_only(request: PermissionRequest) -> Optional[str]:
    """Allow read/search/think tool calls, reject everything else."""
    if request.tool_call.kind in READ_ONLY_KINDS:
        return allow_all(request)
    return reject_all(request)


def _locations(data: Any) -> List[Location]:
    out = []
    for item in data or []:
        if isinstance(item, Mapping) and "path" in item:
            line = item.get("line")
            out.append(Location(str(item["path"]), int(line) if isinstance(line, int) else None))
    return out


def _contents(data: Any) -> List[ToolContent]:
    out: List[ToolContent] = []
    for item in data or []:
        if not isinstance(item, Mapping):
            continue
        if item.get("type") == "diff":
            out.append(DiffContent(str(item.get("path", "")), item.get("oldText"), str(item.get("newText") or "")))
        elif item.get("type") == "content" and isinstance(item.get("content"), Mapping) \
                and item["content"].get("type") == "text":
            out.append(TextContent(str(item["content"].get("text", ""))))
        else:
            out.append(dict(item))
    return out


def parse_update(update: Mapping[str, Any]) -> Event:
    kind = str(update.get("sessionUpdate", ""))
    if kind == "agent_message_chunk":
        content = update.get("content") or {}
        text = content.get("text", "") if isinstance(content, Mapping) and content.get("type") == "text" else ""
        return TextChunk(str(text), dict(update))
    if kind == "tool_call":
        return ToolCall.from_json(update)
    if kind == "tool_call_update":
        return ToolCallUpdate(tool_call_id=str(update.get("toolCallId", "")), status=update.get("status"),
                              content=_contents(update.get("content")), raw_output=update.get("rawOutput"),
                              title=update.get("title"), locations=_locations(update.get("locations")),
                              raw=dict(update))
    if kind == "plan":
        entries = [PlanEntry(str(e.get("content", "")), str(e.get("priority") or "medium"),
                             str(e.get("status") or "pending"))
                   for e in update.get("entries") or [] if isinstance(e, Mapping)]
        return PlanUpdate(entries, dict(update))
    if kind == "current_mode_update":
        return ModeChange(str(update.get("currentModeId", "")))
    return UnknownUpdate(kind, dict(update))


def _resource_block(item: Any) -> Dict[str, Any]:
    if isinstance(item, Mapping):
        return dict(item)
    if isinstance(item, (str, os.PathLike)):
        path = Path(os.fspath(item)).resolve()
        return {"type": "resource_link", "uri": path.as_uri(), "name": path.name}
    raise TypeError(f"unsupported resource: {item!r}")


def embedded_resource(uri: str, text: str, mime_type: str = "text/plain") -> Dict[str, Any]:
    """Build an embedded ``resource`` prompt block."""
    return {"type": "resource", "resource": {"uri": uri, "text": text, "mimeType": mime_type}}


# --------------------------------------------------------------------------- connection


class _Pending:
    __slots__ = ("event", "result", "error", "callback")

    def __init__(self, callback: Optional[Callable[[Any, Optional[BaseException]], None]] = None) -> None:
        self.event = threading.Event()
        self.result: Any = None
        self.error: Optional[BaseException] = None
        self.callback = callback


_TURN_DONE = object()


class DeskAgent:
    """Runs ``desk acp`` as a child process and speaks ACP with it.

    Use as a context manager::

        with DeskAgent() as agent:
            session = agent.new_session(cwd=".", mode="edit")
            print(session.send("Explain this repo").text)
    """

    def __init__(self, desk_path: DeskPath = "desk", env: Optional[Mapping[str, str]] = None,
                 cwd: Optional[Union[str, "os.PathLike[str]"]] = None,
                 on_permission: Optional[PermissionHandler] = None,
                 client_name: str = "deskcode-python", init_timeout: float = 30.0,
                 secrets: Sequence[str] = ()) -> None:
        self.desk_path = desk_path
        self.env = dict(env) if env else None
        self.cwd = os.fspath(cwd) if cwd is not None else None
        self.on_permission: PermissionHandler = on_permission or reject_all
        self.client_name = client_name
        self.init_timeout = init_timeout
        self.agent_info: Dict[str, Any] = {}
        self.agent_capabilities: Dict[str, Any] = {}
        self.auth_methods: List[Any] = []
        self.protocol_version: Optional[int] = None
        self._proc: Optional[subprocess.Popen] = None
        self._ids = itertools.count(1)
        self._pending: Dict[int, _Pending] = {}
        self._lock = threading.Lock()
        self._write_lock = threading.Lock()
        self._sessions: Dict[str, "Session"] = {}
        self._stderr: Deque[str] = collections.deque(maxlen=200)
        self._closed = threading.Event()
        self._exit_error: Optional[AgentExited] = None
        self._threads: List[threading.Thread] = []
        full_env = dict(os.environ)
        if self.env:
            full_env.update(self.env)
        self._secrets = [s for s in (full_env.get("DESK_API_KEY"), *secrets) if s]

    # ------------------------------------------------------------- lifecycle
    def __enter__(self) -> "DeskAgent":
        self.start()
        return self

    def __exit__(self, *exc: Any) -> None:
        self.close()

    def start(self) -> "DeskAgent":
        if self._proc is not None:
            return self
        full_env = dict(os.environ)
        if self.env:
            full_env.update(self.env)
        cmd = command_prefix(self.desk_path) + ["acp"]
        try:
            self._proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                          stderr=subprocess.PIPE, cwd=self.cwd, env=full_env, bufsize=0)
        except FileNotFoundError as exc:
            raise DeskError(f"desk executable not found: {cmd[0]}") from exc
        except OSError as exc:
            raise DeskError(f"cannot start desk acp: {exc}") from exc
        for target, name in ((self._read_stdout, "deskcode-acp-stdout"), (self._read_stderr, "deskcode-acp-stderr")):
            thread = threading.Thread(target=target, name=name, daemon=True)
            thread.start()
            self._threads.append(thread)
        try:
            result = self.request("initialize", {
                "protocolVersion": PROTOCOL_VERSION,
                "clientCapabilities": {"fs": {"readTextFile": False, "writeTextFile": False}, "terminal": False},
                "clientInfo": {"name": self.client_name, "version": __version__},
            }, timeout=self.init_timeout)
        except BaseException:
            self.close()
            raise
        self.protocol_version = result.get("protocolVersion")
        self.agent_capabilities = result.get("agentCapabilities") or {}
        self.agent_info = result.get("agentInfo") or {}
        self.auth_methods = result.get("authMethods") or []
        return self

    def close(self, timeout: float = 5.0) -> None:
        proc = self._proc
        if proc is None:
            return
        try:
            if proc.stdin and not proc.stdin.closed:
                proc.stdin.close()
        except OSError:
            pass
        try:
            proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            proc.terminate()
            try:
                proc.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        for thread in self._threads:
            thread.join(timeout=timeout)
        for stream in (proc.stdout, proc.stderr):
            try:
                if stream:
                    stream.close()
            except OSError:
                pass

    @property
    def returncode(self) -> Optional[int]:
        return self._proc.poll() if self._proc else None

    @property
    def stderr_text(self) -> str:
        return "\n".join(self._stderr)

    # ------------------------------------------------------------- JSON-RPC
    def _send(self, message: Dict[str, Any]) -> None:
        if self._proc is None or self._proc.stdin is None:
            raise DeskError("agent is not running; call start() or use 'with DeskAgent()'")
        if self._exit_error is not None:
            raise self._exit_error
        data = (json.dumps(message, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")
        with self._write_lock:
            try:
                self._proc.stdin.write(data)
                self._proc.stdin.flush()
            except (BrokenPipeError, OSError, ValueError):
                self._closed.wait(2.0)
                raise self._exit_error or AgentExited("desk acp is not accepting input",
                                                      stderr=self.stderr_text, returncode=self.returncode)

    def _start_request(self, method: str, params: Any,
                       callback: Optional[Callable[[Any, Optional[BaseException]], None]] = None) -> _Pending:
        request_id = next(self._ids)
        pending = _Pending(callback)
        with self._lock:
            if self._exit_error is not None:
                raise self._exit_error
            self._pending[request_id] = pending
        try:
            self._send({"jsonrpc": "2.0", "id": request_id, "method": method, "params": params})
        except BaseException:
            with self._lock:
                self._pending.pop(request_id, None)
            raise
        return pending

    def request(self, method: str, params: Any = None, timeout: Optional[float] = None) -> Any:
        """Send a request and block until its response arrives."""
        pending = self._start_request(method, params if params is not None else {})
        if not pending.event.wait(timeout):
            raise DeskError(f"timed out waiting for '{method}' response")
        if pending.error is not None:
            raise pending.error
        return pending.result

    def notify(self, method: str, params: Any = None) -> None:
        self._send({"jsonrpc": "2.0", "method": method, "params": params if params is not None else {}})

    def _respond(self, request_id: Any, result: Any = None, error: Optional[Dict[str, Any]] = None) -> None:
        message: Dict[str, Any] = {"jsonrpc": "2.0", "id": request_id}
        if error is not None:
            message["error"] = error
        else:
            message["result"] = result
        try:
            self._send(message)
        except DeskError:
            pass

    # ------------------------------------------------------------- reader threads
    def _read_stderr(self) -> None:
        assert self._proc and self._proc.stderr
        for raw in iter(self._proc.stderr.readline, b""):
            line = redact(raw.decode("utf-8", "replace").rstrip("\r\n"), self._secrets)
            self._stderr.append(line)
            logger.debug("desk acp stderr: %s", line)

    def _read_stdout(self) -> None:
        assert self._proc and self._proc.stdout
        try:
            for raw in iter(self._proc.stdout.readline, b""):
                line = raw.decode("utf-8", "replace").strip()
                if not line:
                    continue
                try:
                    message = json.loads(line)
                except ValueError:
                    logger.warning("desk acp: ignoring non-JSON line: %s", redact(line[:200], self._secrets))
                    continue
                if isinstance(message, dict):
                    try:
                        self._dispatch(message)
                    except Exception:  # never let a bad message kill the reader
                        logger.exception("desk acp: failed to handle message")
        finally:
            self._on_exit()

    def _on_exit(self) -> None:
        proc = self._proc
        code = None
        if proc is not None:
            try:
                code = proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                code = proc.poll()
        for thread in self._threads:
            if thread.name == "deskcode-acp-stderr" and thread is not threading.current_thread():
                thread.join(timeout=2)
        detail = summarize(self.stderr_text, 1500)
        error = AgentExited(f"desk acp exited with code {code}" + (f": {detail}" if detail else ""),
                            stderr=self.stderr_text, returncode=code)
        with self._lock:
            self._exit_error = error
            pending = list(self._pending.values())
            self._pending.clear()
        for item in pending:
            self._complete(item, None, error)
        for session in list(self._sessions.values()):
            session._agent_exited(error)
        self._closed.set()

    @staticmethod
    def _complete(pending: _Pending, result: Any, error: Optional[BaseException]) -> None:
        pending.result, pending.error = result, error
        if pending.callback is not None:
            try:
                pending.callback(result, error)
            except Exception:
                logger.exception("desk acp: response callback failed")
        pending.event.set()

    def _dispatch(self, message: Dict[str, Any]) -> None:
        method = message.get("method")
        if method is None:  # response
            request_id = message.get("id")
            with self._lock:
                pending = self._pending.pop(request_id, None) if isinstance(request_id, int) else None
            if pending is None:
                logger.debug("desk acp: response for unknown id %r", request_id)
                return
            err = message.get("error")
            if isinstance(err, Mapping):
                self._complete(pending, None, AcpError(int(err.get("code", -32603)), str(err.get("message", "")),
                                                       err.get("data")))
            else:
                self._complete(pending, message.get("result"), None)
            return
        params = message.get("params") or {}
        if "id" in message:  # request from the agent
            if method == "session/request_permission":
                self._respond(message["id"], self._handle_permission(params))
            else:
                self._respond(message["id"], error={"code": METHOD_NOT_FOUND, "message": f"Method not found: {method}"})
            return
        if method == "session/update":
            session = self._sessions.get(str(params.get("sessionId", "")))
            update = params.get("update")
            if session is not None and isinstance(update, Mapping):
                session._on_update(parse_update(update))
        else:
            logger.debug("desk acp: ignoring notification %s", method)

    def _handle_permission(self, params: Mapping[str, Any]) -> Dict[str, Any]:
        session_id = str(params.get("sessionId", ""))
        request = PermissionRequest(
            session_id=session_id,
            tool_call=ToolCall.from_json(params.get("toolCall") or {}),
            options=[PermissionOption(str(o.get("optionId", "")), str(o.get("name") or ""), str(o.get("kind") or ""))
                     for o in params.get("options") or [] if isinstance(o, Mapping)],
            raw=dict(params))
        session = self._sessions.get(session_id)
        handler = (session.on_permission if session and session.on_permission else None) or self.on_permission
        try:
            choice = handler(request)
        except Exception:
            logger.exception("permission handler failed; cancelling")
            choice = None
        if choice is not None and choice not in {o.option_id for o in request.options}:
            logger.warning("permission handler returned unknown optionId %r; cancelling", choice)
            choice = None
        if choice is None:
            return {"outcome": {"outcome": "cancelled"}}
        return {"outcome": {"outcome": "selected", "optionId": choice}}

    # ------------------------------------------------------------- sessions
    def new_session(self, cwd: Optional[Union[str, "os.PathLike[str]"]] = None, mode: Optional[str] = "ask",
                    mcp_servers: Optional[List[Dict[str, Any]]] = None,
                    on_permission: Optional[PermissionHandler] = None) -> "Session":
        """Create a session rooted at ``cwd`` (absolute; defaults to the agent cwd) in ``mode``."""
        root = os.path.abspath(os.fspath(cwd) if cwd is not None else (self.cwd or os.getcwd()))
        result = self.request("session/new", {"cwd": root, "mcpServers": list(mcp_servers or [])})
        session = Session(self, str(result["sessionId"]), root, on_permission)
        modes = result.get("modes") or {}
        session.current_mode = modes.get("currentModeId")
        session.available_modes = [Mode(str(m.get("id", "")), str(m.get("name") or ""), str(m.get("description") or ""))
                                   for m in modes.get("availableModes") or [] if isinstance(m, Mapping)]
        self._sessions[session.session_id] = session
        if mode and mode != session.current_mode:
            session.set_mode(mode)
        return session


class Session:
    """One ACP session.  Only one prompt turn may be active at a time."""

    def __init__(self, agent: DeskAgent, session_id: str, cwd: str,
                 on_permission: Optional[PermissionHandler] = None) -> None:
        self.agent = agent
        self.session_id = session_id
        self.cwd = cwd
        self.on_permission = on_permission
        self.current_mode: Optional[str] = None
        self.available_modes: List[Mode] = []
        self._queue: "queue.Queue[Any]" = queue.Queue()
        self._turn_lock = threading.Lock()
        self._active = False
        self._stale = False  # a previous turn was abandoned; drain until its end

    def __repr__(self) -> str:
        return f"Session(id={self.session_id!r}, cwd={self.cwd!r}, mode={self.current_mode!r})"

    # called from the reader thread
    def _on_update(self, event: Event) -> None:
        if isinstance(event, ModeChange):
            self.current_mode = event.mode_id
        if self._active or self._stale:
            self._queue.put(event)

    def _agent_exited(self, error: AgentExited) -> None:
        self._queue.put((_TURN_DONE, None, error))

    def set_mode(self, mode_id: str) -> None:
        self.agent.request("session/set_mode", {"sessionId": self.session_id, "modeId": mode_id})
        self.current_mode = mode_id

    def cancel(self) -> None:
        """Ask the agent to stop the current turn (it ends with stop_reason ``cancelled``)."""
        self.agent.notify("session/cancel", {"sessionId": self.session_id})

    def _drain_stale(self, timeout: float = 30.0) -> None:
        while self._stale:
            try:
                item = self._queue.get(timeout=timeout)
            except queue.Empty:
                break
            if isinstance(item, tuple) and item and item[0] is _TURN_DONE:
                if isinstance(item[2], AgentExited):
                    self._queue.put(item)
                break
        self._stale = False
        while True:
            try:
                self._queue.get_nowait()
            except queue.Empty:
                break

    def prompt(self, text: str = "", resources: Optional[Sequence[Any]] = None,
               blocks: Optional[Sequence[Mapping[str, Any]]] = None) -> Iterator[Event]:
        """Send a prompt and stream events until the final :class:`TurnEnd`.

        ``resources`` may contain file paths (sent as ``resource_link``) or ready-made
        content block dicts (see :func:`embedded_resource`).
        """
        if not self._turn_lock.acquire(blocking=False):
            raise DeskError("a prompt turn is already running in this session")
        finished = False
        try:
            if self._stale:
                self._drain_stale()
            content: List[Dict[str, Any]] = []
            if text:
                content.append({"type": "text", "text": text})
            content += [_resource_block(r) for r in resources or []]
            content += [dict(b) for b in blocks or []]
            if not content:
                raise ValueError("prompt needs text, resources or blocks")

            def done(result: Any, error: Optional[BaseException]) -> None:
                self._queue.put((_TURN_DONE, result, error))

            self._active = True
            self.agent._start_request("session/prompt", {"sessionId": self.session_id, "prompt": content},
                                      callback=done)
            while True:
                item = self._queue.get()
                if isinstance(item, tuple) and item and item[0] is _TURN_DONE:
                    finished = True
                    _, result, error = item
                    if error is not None:
                        if isinstance(error, AgentExited):
                            self._queue.put(item)  # keep failing future turns
                        raise error
                    yield TurnEnd(str((result or {}).get("stopReason", "end_turn")))
                    return
                yield item
        finally:
            self._active = False
            if not finished and self.agent._exit_error is None:
                # The caller stopped iterating early: cancel and drain at the next prompt.
                self._stale = True
                try:
                    self.cancel()
                except DeskError:
                    pass
            self._turn_lock.release()

    def send(self, text: str = "", resources: Optional[Sequence[Any]] = None,
             blocks: Optional[Sequence[Mapping[str, Any]]] = None) -> TurnResult:
        """Run one prompt turn and collect the text, tool calls, plan and stop reason."""
        parts: List[str] = []
        calls: Dict[str, ToolCall] = {}
        plan: List[PlanEntry] = []
        events: List[Event] = []
        stop = "end_turn"
        for event in self.prompt(text, resources, blocks):
            events.append(event)
            if isinstance(event, TextChunk):
                parts.append(event.text)
            elif isinstance(event, ToolCall):
                calls[event.tool_call_id] = event
            elif isinstance(event, ToolCallUpdate):
                call = calls.setdefault(event.tool_call_id, ToolCall(event.tool_call_id))
                if event.status:
                    call.status = event.status
                if event.title:
                    call.title = event.title
                if event.content:
                    call.content = list(event.content)
                if event.raw_output is not None:
                    call.raw_output = event.raw_output
                if event.locations:
                    call.locations = list(event.locations)
            elif isinstance(event, PlanUpdate):
                plan = list(event.entries)
            elif isinstance(event, TurnEnd):
                stop = event.stop_reason
        return TurnResult("".join(parts), stop, list(calls.values()), plan, events)
