"""Harbor installed-agent adapter for DeskCode (`desk`), e.g. for Terminal-Bench 2.0.

Usage (from the repository root; Harbor imports the class with ``importlib`` and does not add
the current directory to ``sys.path``, hence ``PYTHONPATH``)::

    export DESK_BASE_URL=https://api.example.com/v1 DESK_MODEL=<model> DESK_API_TYPE=openai
    export DESK_API_KEY=...            # never commit it; prefer reading it from a secret store
    PYTHONPATH="$PWD" harbor run -d terminal-bench@2.0 \
        --agent bench.harbor.deskcode_agent:DeskCodeAgent \
        --n-concurrent 4 \
        --ae DESK_BASE_URL="$DESK_BASE_URL" --ae DESK_MODEL="$DESK_MODEL" \
        --ae DESK_API_TYPE="$DESK_API_TYPE" --ae DESK_API_KEY="$DESK_API_KEY"

(``--agent-import-path`` is the older, now deprecated, spelling of the same option.)

Model configuration comes from ``DESK_BASE_URL`` / ``DESK_MODEL`` / ``DESK_API_KEY`` /
``DESK_API_TYPE``: values passed with ``--ae KEY=VALUE`` win, otherwise the host process
environment of ``harbor`` is used. ``-m provider/model`` is only a fallback for
``DESK_MODEL`` / ``DESK_API_TYPE``; DeskCode does not go through LiteLLM.

Agent kwargs (``--ak key=value``): ``release_url``, ``max_steps``, ``workdir``,
``strict_exit`` (true = a non-zero desk exit fails the trial instead of letting the
verifier decide), ``extra_args`` (space separated extra desk flags), ``sandbox`` (true keeps
DeskCode's Landlock sandbox; default false -> ``--no-sandbox`` because containers are
already isolated and often lack Landlock).

Verified against the Harbor source (github.com/laude-institute/harbor, v0.6.0 – v0.27.0,
``src/harbor/agents/installed/base.py``):
  * ``BaseInstalledAgent`` subclasses implement ``name()`` (staticmethod),
    ``async install(environment)``, ``@with_prompt_template async run(instruction,
    environment, context)`` and optionally ``populate_context_post_run(context)`` (sync)
    and ``get_version_command()``.
  * ``self.exec_as_root`` / ``self.exec_as_agent(environment, command, env=, cwd=,
    timeout_sec=)`` raise on non-zero exit; ``environment.upload_file(src, dst)``.
  * ``self.logs_dir`` on the host mirrors ``/logs/agent`` in the container.
  * ``--ae KEY=VALUE`` populates ``extra_env``; Harbor also scopes it into agent execs.

NEEDS MANUAL CHECK against the Harbor version you actually run:
  1. Older Harbor / Terminal-Bench 1.x used ``create_run_agent_commands(instruction) ->
     list[ExecInput]`` plus ``_install_agent_template_path``; a best-effort shim for that API
     is included below but was not exercised against a real old release.
  2. Whether ``/logs/agent`` is bind-mounted for your environment type (docker: yes; remote
     sandboxes such as Daytona/Modal may download logs instead) — token parsing falls back to
     the command's stdout in that case.
  3. ``AgentContext`` field names (``n_input_tokens``, ``n_output_tokens``, ``n_cache_tokens``,
     ``cost_usd``, ``metadata``) — they are set defensively with ``setattr``.
  4. Network policy of the task images: the model endpoint must be reachable from inside the
     container (see ``--allow-agent-host`` in recent Harbor versions).
"""
from __future__ import annotations

import json
import os
import shlex
import tempfile
from pathlib import Path
from typing import Any

try:  # Harbor is only needed at run time; keep the module importable for dry runs.
    from harbor.agents.installed.base import BaseInstalledAgent  # type: ignore
    _HARBOR = True
except Exception:  # noqa: BLE001
    _HARBOR = False

    class BaseInstalledAgent:  # type: ignore[no-redef]
        """Minimal stand-in so ``python3 deskcode_agent.py`` can print the commands."""

        def __init__(self, logs_dir=None, model_name=None, *args, **kwargs):
            self.logs_dir = Path(logs_dir or tempfile.gettempdir())
            self.model_name = model_name
            self._extra_env = dict(kwargs.get("extra_env") or {})

try:
    from harbor.agents.installed.base import with_prompt_template  # type: ignore
except Exception:  # noqa: BLE001
    def with_prompt_template(fn):  # type: ignore[no-redef]
        return fn

try:
    from harbor.agents.installed.base import ExecInput  # type: ignore  # legacy API only
except Exception:  # noqa: BLE001
    ExecInput = None  # type: ignore[assignment]

_NEW_API = hasattr(BaseInstalledAgent, "exec_as_agent")

HERE = Path(__file__).resolve().parent
INSTALL_SCRIPT = HERE / "install-deskcode.sh"
DEFAULT_RELEASE_URL = "https://github.com/Milorgs/DeskCode/releases/latest/download/desk-agent-linux-x64.tar.gz"
REMOTE_DIR = "/installed-agent"
REMOTE_LOGS = "/logs/agent"
RESULT_FILE = "desk-result.json"
MODEL_ENV = ("DESK_BASE_URL", "DESK_MODEL", "DESK_API_KEY", "DESK_API_TYPE")


def _truthy(value: Any) -> bool:
    return str(value).strip().lower() in {"1", "true", "yes", "on"}


def _parse_result(text: str | None) -> dict | None:
    for line in reversed((text or "").strip().splitlines()):
        line = line.strip()
        if line.startswith("{"):
            try:
                value = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(value, dict) and "status" in value:
                return value
    return None


class DeskCodeAgent(BaseInstalledAgent):
    """Runs ``desk --json -p - --approval full --no-sandbox --workspace <workdir>``."""

    def __init__(self, logs_dir: Path, *args, release_url: str | None = None, max_steps: Any = None,
                 workdir: str | None = None, strict_exit: Any = False, extra_args: str | None = None,
                 sandbox: Any = False, **kwargs):
        super().__init__(logs_dir, *args, **kwargs)
        self._release_url = release_url or DEFAULT_RELEASE_URL
        self._max_steps = int(max_steps) if max_steps not in (None, "") else None
        if self._max_steps is not None and not 0 <= self._max_steps <= 100:
            raise ValueError("max_steps must be within 0..100")
        self._workdir = workdir
        self._strict_exit = _truthy(strict_exit)
        self._extra_args = shlex.split(extra_args) if extra_args else []
        self._sandbox = _truthy(sandbox)
        self._last_result: dict | None = None

    # ------------------------------------------------------------------ identity
    @staticmethod
    def name() -> str:
        return "deskcode"

    def version(self) -> str | None:  # BaseInstalledAgent overrides this in new Harbor
        return getattr(self, "_version", None)

    def get_version_command(self) -> str | None:
        return 'PATH="/usr/local/bin:$PATH" desk --version'

    def parse_version(self, stdout: str) -> str:
        return stdout.strip().splitlines()[0] if stdout.strip() else stdout

    # ------------------------------------------------------------------ helpers
    def _desk_env(self) -> dict[str, str]:
        """DESK_* model settings: ``--ae`` values first, then the harbor host environment."""
        extra = dict(getattr(self, "_extra_env", {}) or {})
        env: dict[str, str] = {}
        for name in MODEL_ENV:
            value = extra.get(name) or os.environ.get(name)
            if value:
                env[name] = value
        model_name = getattr(self, "model_name", None) or ""
        if "DESK_MODEL" not in env and model_name:
            env["DESK_MODEL"] = model_name.split("/", 1)[1] if "/" in model_name else model_name
        if "DESK_API_TYPE" not in env:
            env["DESK_API_TYPE"] = "anthropic" if model_name.startswith("anthropic/") else "openai"
        missing = [n for n in ("DESK_BASE_URL", "DESK_MODEL", "DESK_API_KEY") if not env.get(n)]
        if missing:
            raise ValueError("DeskCode needs " + ", ".join(missing) +
                             " (pass with --ae NAME=value or export them before running harbor)")
        # Private, throw-away config/state inside the container; never a real user profile.
        env.update({
            "XDG_CONFIG_HOME": "/tmp/deskcode-xdg/config",
            "XDG_STATE_HOME": "/tmp/deskcode-xdg/state",
            "DESK_NO_ARROW": "1",
        })
        return env

    def _desk_flags(self) -> list[str]:
        flags = ["--json", "-p", "-", "--approval", "full"]
        if not self._sandbox:
            flags.append("--no-sandbox")
        if self._max_steps is not None:
            flags += ["--max-steps", str(self._max_steps)]
        return flags + self._extra_args

    def build_run_command(self, instruction_path: str) -> str:
        """Shell command run as the agent user inside the task container."""
        workspace = shlex.quote(self._workdir) if self._workdir else '"$PWD"'
        flags = " ".join(shlex.quote(f) for f in self._desk_flags())
        tail = 'exit "$rc"' if self._strict_exit else "exit 0"
        return (
            # Keep the image's PATH (venvs, cargo, conda...) and only make sure desk is found.
            "export PATH=\"/usr/local/bin:$PATH\"; "
            f"mkdir -p {REMOTE_LOGS} \"$XDG_CONFIG_HOME\" \"$XDG_STATE_HOME\"; "
            f"cd {workspace} || exit 1; "
            f"desk {flags} --workspace \"$PWD\" < {shlex.quote(instruction_path)} "
            f"> {REMOTE_LOGS}/{RESULT_FILE} 2> {REMOTE_LOGS}/desk-stderr.log; rc=$?; "
            f"echo \"$rc\" > {REMOTE_LOGS}/desk-exit-code; "
            f"cat {REMOTE_LOGS}/{RESULT_FILE}; tail -n 20 {REMOTE_LOGS}/desk-stderr.log >&2; {tail}"
        )

    def _apply_result(self, context: Any, data: dict | None) -> None:
        if not data or context is None:
            return
        self._last_result = data
        usage = data.get("usage") if isinstance(data.get("usage"), dict) else {}

        def put(field: str, value: Any) -> None:
            if value is None:
                return
            try:
                setattr(context, field, value)
            except Exception:  # noqa: BLE001 - schema differs between Harbor versions
                pass

        put("n_input_tokens", usage.get("input"))
        put("n_output_tokens", usage.get("output"))
        put("n_cache_tokens", usage.get("cache_read"))
        put("cost_usd", usage.get("cost"))
        tools = data.get("tools") if isinstance(data.get("tools"), list) else []
        metadata = dict(getattr(context, "metadata", None) or {})
        metadata["deskcode"] = {
            "status": data.get("status"),
            "error": data.get("error"),
            "tokens": data.get("tokens"),
            "elapsed_seconds": data.get("elapsed_seconds"),
            "tool_calls": len(tools),
            "tool_failures": sum(1 for t in tools if isinstance(t, dict) and t.get("ok") is False),
            "version": data.get("version"),
            "model": data.get("model"),
        }
        put("metadata", metadata)

    # ------------------------------------------------------------------ Harbor >= 0.6 API
    if _NEW_API:
        async def install(self, environment) -> None:
            await environment.upload_file(INSTALL_SCRIPT, f"{REMOTE_DIR}/install-deskcode.sh")
            await self.exec_as_root(
                environment,
                command=f"bash {REMOTE_DIR}/install-deskcode.sh",
                env={"DESK_RELEASE_URL": self._release_url},
            )

        @with_prompt_template
        async def run(self, instruction: str, environment, context) -> None:
            env = self._desk_env()
            remote_instruction = f"{REMOTE_DIR}/instruction.md"
            with tempfile.TemporaryDirectory(prefix="deskcode-harbor-") as tmp:
                local = Path(tmp) / "instruction.md"
                local.write_text(instruction, encoding="utf-8")
                await environment.upload_file(local, remote_instruction)
            # upload_file copies as root; the agent user must be able to read it.
            await self.exec_as_root(environment, command=f"chmod 644 {remote_instruction}")
            result = await self.exec_as_agent(environment, command=self.build_run_command(remote_instruction), env=env)
            self._apply_result(context, _parse_result(getattr(result, "stdout", None)))

        def populate_context_post_run(self, context) -> None:
            if self._last_result is not None:
                return
            path = Path(self.logs_dir) / RESULT_FILE
            if path.is_file():
                self._apply_result(context, _parse_result(path.read_text(encoding="utf-8", errors="replace")))

    # ------------------------------------------------------------------ legacy API (best effort)
    else:
        @property
        def _install_agent_template_path(self) -> Path:
            return INSTALL_SCRIPT

        @property
        def _env(self) -> dict[str, str]:
            return self._desk_env()

        def create_run_agent_commands(self, instruction: str):
            quoted = shlex.quote(instruction)
            path = f"{REMOTE_DIR}/instruction.md"
            commands = [f"mkdir -p {REMOTE_DIR} && printf '%s' {quoted} > {path}", self.build_run_command(path)]
            if ExecInput is None:
                return commands
            env = self._desk_env()
            return [ExecInput(command=c, env=env) for c in commands]


if __name__ == "__main__":
    # Dry run without Harbor: show what would be executed (secrets masked).
    os.environ.setdefault("DESK_BASE_URL", "https://api.example.com/v1")
    os.environ.setdefault("DESK_MODEL", "example-model")
    os.environ.setdefault("DESK_API_KEY", "dry-run-not-a-key")
    agent = DeskCodeAgent(Path(tempfile.gettempdir()), max_steps=50)
    env = {k: ("***" if "KEY" in k else v) for k, v in agent._desk_env().items()}
    print("harbor importable:", _HARBOR, "| new API:", _NEW_API)
    print("install script:", INSTALL_SCRIPT, "exists:", INSTALL_SCRIPT.is_file())
    print("env:", json.dumps(env, indent=2))
    print("run command:\n ", agent.build_run_command(f"{REMOTE_DIR}/instruction.md"))
