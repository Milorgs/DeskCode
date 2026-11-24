"""One-shot ``desk --json -p`` runs."""
from __future__ import annotations

import asyncio
import json
import os
import subprocess
from dataclasses import dataclass, field
from typing import Any, Dict, List, Mapping, Optional, Sequence, Tuple, Union

from ._errors import DeskError, DeskTimeout, redact, summarize

DeskPath = Union[str, "os.PathLike[str]", Sequence[str]]

__all__ = ["Usage", "ToolRecord", "RunResult", "run", "arun"]


@dataclass
class Usage:
    input: int = 0
    output: int = 0
    cache_read: int = 0
    cost: Optional[float] = None
    currency: Optional[str] = None

    @classmethod
    def from_json(cls, data: Optional[Mapping[str, Any]]) -> "Usage":
        data = data or {}
        cost = data.get("cost")
        return cls(input=int(data.get("input") or 0), output=int(data.get("output") or 0),
                   cache_read=int(data.get("cache_read") or 0),
                   cost=float(cost) if cost is not None else None, currency=data.get("currency"))


@dataclass
class ToolRecord:
    name: str
    target: str = ""
    ok: bool = True
    error: Optional[str] = None

    @classmethod
    def from_json(cls, data: Mapping[str, Any]) -> "ToolRecord":
        return cls(name=str(data.get("name", "")), target=str(data.get("target", "")),
                   ok=bool(data.get("ok", True)), error=data.get("error"))


@dataclass
class RunResult:
    """Result of :func:`run` – mirrors the JSON line printed by ``desk --json``."""

    status: str
    reply: str = ""
    error: str = ""
    model: str = ""
    protocol: str = ""
    workspace: str = ""
    tokens: int = 0
    usage: Usage = field(default_factory=Usage)
    plan_submitted: Optional[bool] = None
    elapsed_seconds: float = 0.0
    tools: List[ToolRecord] = field(default_factory=list)
    version: str = ""
    exit_code: int = 0
    stderr: str = ""
    raw: Dict[str, Any] = field(default_factory=dict, repr=False)

    @property
    def ok(self) -> bool:
        return self.status == "completed"

    @classmethod
    def from_json(cls, data: Mapping[str, Any], exit_code: int = 0, stderr: str = "") -> "RunResult":
        return cls(
            status=str(data.get("status", "")),
            reply=str(data.get("reply") or ""),
            error=str(data.get("error") or ""),
            model=str(data.get("model") or ""),
            protocol=str(data.get("protocol") or ""),
            workspace=str(data.get("workspace") or ""),
            tokens=int(data.get("tokens") or 0),
            usage=Usage.from_json(data.get("usage")),
            plan_submitted=data.get("plan_submitted"),
            elapsed_seconds=float(data.get("elapsed_seconds") or 0.0),
            tools=[ToolRecord.from_json(t) for t in data.get("tools") or [] if isinstance(t, Mapping)],
            version=str(data.get("version") or ""),
            exit_code=exit_code,
            stderr=stderr,
            raw=dict(data),
        )


def command_prefix(desk_path: DeskPath) -> List[str]:
    if isinstance(desk_path, (str, os.PathLike)):
        return [os.fspath(desk_path)]
    return [str(part) for part in desk_path]


def _build(prompt: str, cwd: Optional[str], approval: str, plan: bool, max_steps: Optional[int],
           model: Optional[str], base_url: Optional[str], api_key: Optional[str], api_type: Optional[str],
           desk_path: DeskPath, env: Optional[Mapping[str, str]]) -> Tuple[List[str], Dict[str, str], List[Optional[str]]]:
    if approval not in ("ask", "edit", "full"):
        raise ValueError("approval must be 'ask', 'edit' or 'full'")
    cmd = command_prefix(desk_path) + ["--json", "-p", "-", "--approval", approval]
    if plan:
        cmd.append("--plan")
    if max_steps is not None:
        cmd += ["--max-steps", str(int(max_steps))]
    if cwd is not None:
        cmd += ["--workspace", os.path.abspath(os.fspath(cwd))]
    full_env = dict(os.environ)
    if env:
        full_env.update(env)
    for key, value in (("DESK_MODEL", model), ("DESK_BASE_URL", base_url),
                       ("DESK_API_KEY", api_key), ("DESK_API_TYPE", api_type)):
        if value is not None:
            full_env[key] = value
    return cmd, full_env, [api_key, full_env.get("DESK_API_KEY")]


def _parse(stdout: str, stderr: str, code: int, secrets: List[Optional[str]]) -> RunResult:
    stderr = redact(stderr, secrets)
    for line in reversed(stdout.splitlines()):
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            data = json.loads(line)
        except ValueError:
            continue
        if isinstance(data, dict) and "status" in data:
            return RunResult.from_json(data, exit_code=code, stderr=stderr)
    detail = summarize(stderr or redact(stdout, secrets))
    raise DeskError(f"desk exited with code {code} without a JSON result" + (f": {detail}" if detail else ""),
                    stderr=stderr, returncode=code)


def run(prompt: str, *, cwd: Optional[Union[str, "os.PathLike[str]"]] = None, approval: str = "edit",
        plan: bool = False, max_steps: Optional[int] = None, model: Optional[str] = None,
        base_url: Optional[str] = None, api_key: Optional[str] = None, api_type: Optional[str] = None,
        desk_path: DeskPath = "desk", timeout: Optional[float] = None,
        env: Optional[Mapping[str, str]] = None) -> RunResult:
    """Run ``desk --json -p`` once and return the parsed result.

    A non-zero exit code is *not* an exception as long as desk printed its JSON result
    (check :attr:`RunResult.ok` / :attr:`RunResult.status`).  :class:`DeskError` is raised
    when desk cannot be started or prints no JSON; its message never contains the API key.
    """
    cwd_s = os.fspath(cwd) if cwd is not None else None
    cmd, full_env, secrets = _build(prompt, cwd_s, approval, plan, max_steps, model, base_url, api_key,
                                    api_type, desk_path, env)
    try:
        proc = subprocess.run(cmd, input=prompt, capture_output=True, text=True, encoding="utf-8",
                              errors="replace", cwd=cwd_s, env=full_env, timeout=timeout)
    except FileNotFoundError as exc:
        raise DeskError(f"desk executable not found: {cmd[0]}") from exc
    except subprocess.TimeoutExpired as exc:
        err = exc.stderr.decode("utf-8", "replace") if isinstance(exc.stderr, bytes) else (exc.stderr or "")
        err = redact(err, secrets)
        raise DeskTimeout(f"desk timed out after {timeout}s", stderr=err) from None
    except OSError as exc:
        raise DeskError(f"cannot start desk: {exc}") from exc
    return _parse(proc.stdout, proc.stderr, proc.returncode, secrets)


async def arun(prompt: str, *, cwd: Optional[Union[str, "os.PathLike[str]"]] = None, approval: str = "edit",
               plan: bool = False, max_steps: Optional[int] = None, model: Optional[str] = None,
               base_url: Optional[str] = None, api_key: Optional[str] = None, api_type: Optional[str] = None,
               desk_path: DeskPath = "desk", timeout: Optional[float] = None,
               env: Optional[Mapping[str, str]] = None) -> RunResult:
    """asyncio version of :func:`run` (same arguments and semantics)."""
    cwd_s = os.fspath(cwd) if cwd is not None else None
    cmd, full_env, secrets = _build(prompt, cwd_s, approval, plan, max_steps, model, base_url, api_key,
                                    api_type, desk_path, env)
    try:
        proc = await asyncio.create_subprocess_exec(
            *cmd, stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE, cwd=cwd_s, env=full_env)
    except FileNotFoundError as exc:
        raise DeskError(f"desk executable not found: {cmd[0]}") from exc
    except OSError as exc:
        raise DeskError(f"cannot start desk: {exc}") from exc
    try:
        out, err = await asyncio.wait_for(proc.communicate(prompt.encode("utf-8")), timeout)
    except asyncio.TimeoutError:
        proc.kill()
        await proc.wait()
        raise DeskTimeout(f"desk timed out after {timeout}s") from None
    except BaseException:  # e.g. task cancellation: do not leave desk running
        if proc.returncode is None:
            proc.kill()
            await proc.wait()
        raise
    return _parse(out.decode("utf-8", "replace"), err.decode("utf-8", "replace"),
                  proc.returncode if proc.returncode is not None else -1, secrets)
