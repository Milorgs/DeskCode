#!/usr/bin/env python3
"""DeskCode local agent benchmark runner (Python 3 standard library only).

Runs the offline tasks in ``bench/local/tasks`` against a ``desk`` binary in headless
``--json`` mode, verifies every result with the task's hidden ``check.sh`` and writes a JSON
report plus a Markdown summary.

Isolation: every run gets a fresh ``tempfile.mkdtemp(prefix="desk-bench-")`` directory with
its own workspace, ``HOME``, ``XDG_CONFIG_HOME``, ``XDG_STATE_HOME``, ``XDG_CACHE_HOME`` and
``XDG_DATA_HOME`` so the real ``~/.config/desk-agent`` (history, MCP settings, API keys) is never
read or written. The model is taken from ``DESK_BASE_URL`` / ``DESK_MODEL`` / ``DESK_API_KEY`` /
``DESK_API_TYPE`` in the caller's environment. Secrets are redacted from everything written.

Examples::

    python3 bench/run_local.py --self-test
    DESK_BASE_URL=... DESK_MODEL=... DESK_API_KEY=... DESK_API_TYPE=openai \\
        python3 bench/run_local.py --label my-model --repeat 3 --jobs 4
"""
from __future__ import annotations

import argparse
import collections
import datetime as _dt
import fnmatch
import json
import os
import re
import shutil
import signal
import statistics
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

BENCH_DIR = Path(__file__).resolve().parent
REPO_DIR = BENCH_DIR.parent
TASKS_DIR = BENCH_DIR / "local" / "tasks"
SOLUTIONS_DIR = BENCH_DIR / "local" / "solutions"
RESULTS_DIR = BENCH_DIR / "results"
TEMP_PREFIX = "desk-bench-"
SCHEMA_VERSION = 1
CHECK_TIMEOUT = 300
OUTPUT_TAIL = 4000

MODEL_ENV = ("DESK_BASE_URL", "DESK_MODEL", "DESK_API_KEY", "DESK_API_TYPE")
SECRET_NAME = re.compile(r"(KEY|TOKEN|SECRET|PASSWORD|PASSWD|CREDENTIAL|AUTH)", re.IGNORECASE)
GIT_IDENTITY = {
    "GIT_AUTHOR_NAME": "DeskCode Bench",
    "GIT_AUTHOR_EMAIL": "bench@deskcode.invalid",
    "GIT_COMMITTER_NAME": "DeskCode Bench",
    "GIT_COMMITTER_EMAIL": "bench@deskcode.invalid",
}

_ACTIVE: set[subprocess.Popen] = set()
_ACTIVE_LOCK = threading.Lock()
_TEMP_DIRS: set[str] = set()
_STOP = threading.Event()


# --------------------------------------------------------------------------- secrets
class Redactor:
    """Replaces secret environment values (and the API key) with ``***``."""

    def __init__(self, environ: dict[str, str]):
        values = set()
        for name, value in environ.items():
            if not value or len(value) < 6:
                continue
            if name == "DESK_API_KEY" or SECRET_NAME.search(name):
                values.add(value)
        # Longest first so overlapping secrets are fully masked.
        self.values = sorted(values, key=len, reverse=True)

    def text(self, value: str) -> str:
        for secret in self.values:
            if secret in value:
                value = value.replace(secret, "***")
        return value

    def deep(self, value):
        if isinstance(value, str):
            return self.text(value)
        if isinstance(value, list):
            return [self.deep(v) for v in value]
        if isinstance(value, dict):
            return {self.text(str(k)): self.deep(v) for k, v in value.items()}
        return value


def safe_url(url: str) -> str:
    """Drop user-info, query and fragment (they may carry credentials)."""
    try:
        parts = urllib.parse.urlsplit(url)
    except ValueError:
        return "<invalid url>"
    host = parts.hostname or ""
    if parts.port:
        host = f"{host}:{parts.port}"
    return urllib.parse.urlunsplit((parts.scheme, host, parts.path, "", ""))


# --------------------------------------------------------------------------- tasks
class Task:
    def __init__(self, directory: Path):
        self.dir = directory
        self.meta = json.loads((directory / "task.json").read_text(encoding="utf-8"))
        self.id = self.meta.get("id") or directory.name
        self.name = directory.name
        self.instruction = (directory / "instruction.md").read_text(encoding="utf-8").strip()
        self.check = directory / "check.sh"
        self.setup = directory / "setup"
        if not self.check.is_file():
            raise ValueError(f"{directory}: check.sh missing")

    @property
    def timeout(self) -> int:
        return int(self.meta.get("timeout_seconds") or 600)

    @property
    def max_steps(self) -> int | None:
        value = self.meta.get("max_steps")
        return int(value) if value is not None else None


def discover(patterns: str | None) -> list[Task]:
    tasks = []
    for path in sorted(TASKS_DIR.iterdir()) if TASKS_DIR.is_dir() else []:
        if (path / "task.json").is_file():
            tasks.append(Task(path))
    if patterns:
        wanted = [p.strip() for p in patterns.split(",") if p.strip()]
        tasks = [t for t in tasks if any(fnmatch.fnmatchcase(t.name, p) or fnmatch.fnmatchcase(t.id, p) for p in wanted)]
    return tasks


# --------------------------------------------------------------------------- processes
def _register(proc: subprocess.Popen) -> None:
    with _ACTIVE_LOCK:
        _ACTIVE.add(proc)


def _unregister(proc: subprocess.Popen) -> None:
    with _ACTIVE_LOCK:
        _ACTIVE.discard(proc)


def _kill_group(proc: subprocess.Popen) -> None:
    if proc.poll() is not None:
        return
    try:
        os.killpg(proc.pid, signal.SIGTERM)
    except (ProcessLookupError, PermissionError):
        return
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass
        proc.wait()


def run_process(cmd, *, cwd, env, timeout, stdout_path=None) -> dict:
    """Run ``cmd`` in its own process group; kill the whole group on timeout."""
    started = time.monotonic()
    out_file = open(stdout_path, "w+b") if stdout_path else tempfile.TemporaryFile()
    err_file = tempfile.TemporaryFile()
    timed_out = False
    try:
        proc = subprocess.Popen(cmd, cwd=cwd, env=env, stdin=subprocess.DEVNULL, stdout=out_file,
                                stderr=err_file, start_new_session=True)
        _register(proc)
        try:
            proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            _kill_group(proc)
        finally:
            # Children that outlive the leader (background jobs) are cleaned up too.
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                pass
            _unregister(proc)
        out_file.seek(0)
        err_file.seek(0)
        stdout = out_file.read().decode("utf-8", "replace")
        stderr = err_file.read().decode("utf-8", "replace")
        return {"returncode": proc.returncode, "timed_out": timed_out, "stdout": stdout, "stderr": stderr,
                "seconds": round(time.monotonic() - started, 2)}
    finally:
        out_file.close()
        err_file.close()


def git(ws: Path, *args, env) -> subprocess.CompletedProcess:
    return subprocess.run(["git", *args], cwd=ws, env=env, stdin=subprocess.DEVNULL,
                          capture_output=True, text=True, timeout=60)


# --------------------------------------------------------------------------- workspace
def isolated_env(root: Path, base: dict[str, str], include_model: bool) -> dict[str, str]:
    """Environment for desk / check / solve: private HOME and XDG dirs, fixed git identity."""
    env = {}
    for name, value in base.items():
        if name.startswith(("XDG_", "DESK_", "GIT_")) or name in {"HOME", "PYTHONPATH", "PYTHONHOME", "VIRTUAL_ENV"}:
            continue
        env[name] = value
    home = root / "home"
    for sub in ("home", "config", "state", "cache", "data"):
        (root / sub).mkdir(parents=True, exist_ok=True)
    env.update({
        "HOME": str(home),
        "XDG_CONFIG_HOME": str(root / "config"),
        "XDG_STATE_HOME": str(root / "state"),
        "XDG_CACHE_HOME": str(root / "cache"),
        "XDG_DATA_HOME": str(root / "data"),
        "GIT_CONFIG_NOSYSTEM": "1",
        "GIT_TERMINAL_PROMPT": "0",
        "DESK_NO_ARROW": "1",
        "PYTHONDONTWRITEBYTECODE": "1",
        "LC_ALL": base.get("LC_ALL") or "C.UTF-8",
        **GIT_IDENTITY,
    })
    if "TMPDIR" not in env:
        env["TMPDIR"] = tempfile.gettempdir()
    if include_model:
        for name in MODEL_ENV:
            if base.get(name):
                env[name] = base[name]
    return env


def prepare_workspace(task: Task, root: Path, env: dict[str, str]) -> tuple[Path, str]:
    ws = root / "ws"
    if task.setup.is_dir():
        shutil.copytree(task.setup, ws, symlinks=True)
    else:
        ws.mkdir()
    steps = [("init", "-q"), ("symbolic-ref", "HEAD", "refs/heads/main"), ("add", "-A"),
             ("commit", "-q", "--no-verify", "--allow-empty", "-m", "bench: initial task state")]
    for i, step in enumerate(steps):
        result = git(ws, *step, env=env)
        if result.returncode != 0:
            raise RuntimeError(f"git {' '.join(step)} failed: {result.stderr.strip()}")
        if i == 1:
            info = ws / ".git" / "info"
            info.mkdir(parents=True, exist_ok=True)
            with open(info / "exclude", "a", encoding="utf-8") as handle:
                handle.write(".desk-agent/\n__pycache__/\n")
    base = git(ws, "rev-parse", "HEAD", env=env).stdout.strip()
    return ws, base


def run_check(task: Task, ws: Path, env: dict[str, str], base: str) -> dict:
    check_env = dict(env, BENCH_BASE_COMMIT=base, TASK_DIR=str(task.dir))
    result = run_process(["bash", str(task.check)], cwd=ws, env=check_env, timeout=CHECK_TIMEOUT)
    output = (result["stdout"] + result["stderr"]).strip()
    return {"passed": result["returncode"] == 0 and not result["timed_out"],
            "exit_code": result["returncode"], "timed_out": result["timed_out"],
            "seconds": result["seconds"], "output": output[-OUTPUT_TAIL:]}


def diff_stats(ws: Path, env: dict[str, str], base: str) -> dict:
    stats = {"files": 0, "insertions": 0, "deletions": 0, "commits": 0}
    if git(ws, "add", "-A", env=env).returncode != 0:
        return stats
    numstat = git(ws, "diff", "--cached", "--numstat", base, env=env).stdout
    for line in numstat.splitlines():
        parts = line.split("\t")
        if len(parts) >= 3:
            stats["files"] += 1
            stats["insertions"] += int(parts[0]) if parts[0].isdigit() else 0
            stats["deletions"] += int(parts[1]) if parts[1].isdigit() else 0
    count = git(ws, "rev-list", "--count", "--all", f"^{base}", env=env).stdout.strip()
    stats["commits"] = int(count) if count.isdigit() else 0
    return stats


def new_root() -> Path:
    root = tempfile.mkdtemp(prefix=TEMP_PREFIX)
    _TEMP_DIRS.add(root)
    return Path(root)


def remove_root(root: Path) -> None:
    shutil.rmtree(root, ignore_errors=True)
    _TEMP_DIRS.discard(str(root))


# --------------------------------------------------------------------------- desk json
def parse_desk_json(stdout: str) -> dict | None:
    for line in reversed(stdout.strip().splitlines()):
        line = line.strip()
        if line.startswith("{"):
            try:
                value = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(value, dict):
                return value
    try:
        value = json.loads(stdout)
        return value if isinstance(value, dict) else None
    except json.JSONDecodeError:
        return None


def _number(value):
    if isinstance(value, bool):
        return None
    if isinstance(value, (int, float)):
        return value
    if isinstance(value, str):
        try:
            return float(value) if "." in value else int(value)
        except ValueError:
            return None
    return None


def normalise_usage(raw) -> dict | None:
    """Accept ``{"input","output","cache_read","cost"}`` and a few common aliases."""
    if not isinstance(raw, dict):
        return None
    aliases = {
        "input": ("input", "input_tokens", "prompt_tokens"),
        "output": ("output", "output_tokens", "completion_tokens"),
        "cache_read": ("cache_read", "cache_read_tokens", "cached_tokens", "cache_read_input_tokens"),
        "cache_write": ("cache_write", "cache_creation_input_tokens"),
        "cost": ("cost", "cost_usd", "total_cost"),
    }
    usage = {}
    for key, names in aliases.items():
        for name in names:
            number = _number(raw.get(name))
            if number is not None:
                usage[key] = number
                break
    return usage or None


def summarise_desk(data: dict | None) -> dict:
    if not data:
        return {"json_ok": False}
    tools = data.get("tools") if isinstance(data.get("tools"), list) else []
    names = collections.Counter(str(t.get("name", "?")) for t in tools if isinstance(t, dict))
    failed = sum(1 for t in tools if isinstance(t, dict) and t.get("ok") is False)
    reply = data.get("reply") if isinstance(data.get("reply"), str) else ""
    return {
        "json_ok": True,
        "status": data.get("status"),
        "error": data.get("error"),
        "reply": reply[:2000],
        "tokens": _number(data.get("tokens")),
        "elapsed_seconds": _number(data.get("elapsed_seconds")),
        "usage": normalise_usage(data.get("usage")),
        "tool_calls": len(tools),
        "tool_failures": failed,
        "tools_by_name": dict(names.most_common()),
        "desk_version": data.get("version"),
        "model": data.get("model"),
    }


# --------------------------------------------------------------------------- one run
def run_task(task: Task, repeat_index: int, args, redactor: Redactor) -> dict:
    record = {"task": task.id, "repeat": repeat_index, "category": task.meta.get("category"),
              "difficulty": task.meta.get("difficulty"), "passed": False}
    if _STOP.is_set():
        record["error"] = "skipped (interrupted)"
        return record
    root = new_root()
    try:
        env = isolated_env(root, dict(os.environ), include_model=True)
        ws, base = prepare_workspace(task, root, env)
        timeout = args.timeout or task.timeout
        max_steps = args.max_steps if args.max_steps is not None else task.max_steps
        cmd = [args.desk, "--json", "-p", task.instruction, "--approval", "full", "--workspace", str(ws)]
        if max_steps is not None:
            cmd += ["--max-steps", str(max_steps)]
        cmd += args.desk_arg or []
        desk = run_process(cmd, cwd=ws, env=env, timeout=timeout)
        record.update({
            "timeout_seconds": timeout,
            "max_steps": max_steps,
            "desk_exit_code": desk["returncode"],
            "desk_timed_out": desk["timed_out"],
            "wall_seconds": desk["seconds"],
        })
        record.update(summarise_desk(parse_desk_json(desk["stdout"])))
        if desk["timed_out"]:
            record["desk_status"] = record.get("status")
            record["status"] = "timeout"
        if not record.get("json_ok"):
            record["desk_stdout_tail"] = desk["stdout"][-OUTPUT_TAIL:]
        record["desk_stderr_tail"] = desk["stderr"][-1500:]
        # The verifier never needs model credentials.
        plain_env = {k: v for k, v in env.items() if k not in MODEL_ENV}
        check = run_check(task, ws, plain_env, base)
        record["passed"] = check["passed"]
        record["check"] = check
        record["diff"] = diff_stats(ws, plain_env, base)
        if args.keep:
            record["kept_dir"] = str(root)
    except Exception as exc:  # noqa: BLE001 - report and continue with other tasks
        record["error"] = f"{type(exc).__name__}: {exc}"
    finally:
        if not args.keep:
            remove_root(root)
        else:
            _TEMP_DIRS.discard(str(root))
    return redactor.deep(record)


# --------------------------------------------------------------------------- self test
def self_test(tasks: list[Task], args) -> int:
    rows, ok_all = [], True
    for task in tasks:
        solution = SOLUTIONS_DIR / task.name
        row = {"task": task.id, "initial_fails": None, "solution_passes": None, "note": ""}
        root = new_root()
        try:
            env = isolated_env(root, dict(os.environ), include_model=False)
            ws, base = prepare_workspace(task, root, env)
            first = run_check(task, ws, env, base)
            row["initial_fails"] = not first["passed"]
            if args.verbose:
                tail = first["output"].strip().splitlines()[-3:]
                print(f"[{task.id}] initial check: " + " | ".join(tail)[:300], file=sys.stderr)
        except Exception as exc:  # noqa: BLE001
            row["note"] = f"initial: {exc}"
        finally:
            remove_root(root)
        if not (solution / "solve.sh").is_file() and not (solution / "files").is_dir():
            row["note"] = (row["note"] + " no reference solution").strip()
        else:
            root = new_root()
            try:
                env = isolated_env(root, dict(os.environ), include_model=False)
                ws, base = prepare_workspace(task, root, env)
                if (solution / "files").is_dir():
                    shutil.copytree(solution / "files", ws, dirs_exist_ok=True, symlinks=True)
                if (solution / "solve.sh").is_file():
                    solved = run_process(["bash", str(solution / "solve.sh")], cwd=ws,
                                         env=dict(env, SOLUTION_DIR=str(solution)), timeout=120)
                    if solved["returncode"] != 0:
                        row["note"] = f"solve.sh failed: {(solved['stdout'] + solved['stderr']).strip()[-300:]}"
                second = run_check(task, ws, env, base)
                row["solution_passes"] = second["passed"]
                if not second["passed"] and args.verbose:
                    row["note"] = (row["note"] + " " + second["output"][-600:]).strip()
            except Exception as exc:  # noqa: BLE001
                row["note"] = f"solution: {exc}"
            finally:
                if args.keep:
                    print(f"kept: {root}")
                    _TEMP_DIRS.discard(str(root))
                else:
                    remove_root(root)
        good = row["initial_fails"] is True and row["solution_passes"] is True
        ok_all &= good
        rows.append((row, good))

    print("\n## Self-test\n")
    print("| task | initial state fails | reference solution passes | result |")
    print("|---|---|---|---|")
    for row, good in rows:
        mark = lambda v: "yes" if v else ("no" if v is not None else "n/a")  # noqa: E731
        print(f"| {row['task']} | {mark(row['initial_fails'])} | {mark(row['solution_passes'])} | "
              f"{'OK' if good else 'BROKEN ' + row['note'][:200]} |")
    print(f"\n{sum(g for _, g in rows)}/{len(rows)} tasks verified.")
    return 0 if ok_all and rows else 1


# --------------------------------------------------------------------------- report
def fmt_num(value, digits=1):
    if value is None:
        return "-"
    if isinstance(value, float):
        return f"{value:.{digits}f}"
    return str(value)


def build_summary(results: list[dict]) -> dict:
    by_task = collections.OrderedDict()
    for r in results:
        by_task.setdefault(r["task"], []).append(r)

    def agg(runs):
        times = [r["wall_seconds"] for r in runs if r.get("wall_seconds") is not None]
        tokens = [r["tokens"] for r in runs if isinstance(r.get("tokens"), (int, float))]
        tools = [r["tool_calls"] for r in runs if isinstance(r.get("tool_calls"), int)]
        costs = [r["usage"]["cost"] for r in runs if isinstance(r.get("usage"), dict) and "cost" in r["usage"]]
        passed = sum(1 for r in runs if r.get("passed"))
        return {
            "runs": len(runs), "passed": passed,
            "pass_rate": round(passed / len(runs), 4) if runs else 0.0,
            "avg_seconds": round(statistics.mean(times), 2) if times else None,
            "total_tokens": sum(tokens) if tokens else 0,
            "avg_tokens": round(statistics.mean(tokens), 1) if tokens else None,
            "avg_tool_calls": round(statistics.mean(tools), 2) if tools else None,
            "total_cost": round(sum(costs), 6) if costs else None,
            "statuses": dict(collections.Counter(str(r.get("status") or r.get("error") and "error" or "unknown") for r in runs)),
        }

    return {"overall": agg(results), "tasks": {name: agg(runs) for name, runs in by_task.items()}}


def print_summary(report: dict) -> None:
    summary = report["summary"]
    overall = summary["overall"]
    print(f"\n## DeskCode local bench — {report['label']}\n")
    print(f"- model: `{report['model'] or '-'}` ({report['api_type'] or '-'}) · desk {report['desk_version'] or '?'}")
    print(f"- pass rate: **{overall['passed']}/{overall['runs']} ({overall['pass_rate'] * 100:.1f}%)**"
          f" · avg time {fmt_num(overall['avg_seconds'])}s · total tokens {overall['total_tokens']}"
          + (f" · cost {overall['total_cost']}" if overall.get("total_cost") is not None else ""))
    print()
    print("| task | pass | rate | avg s | avg tokens | avg tools | status |")
    print("|---|---|---|---|---|---|---|")
    for name, row in summary["tasks"].items():
        statuses = ", ".join(f"{k}×{v}" for k, v in row["statuses"].items())
        print(f"| {name} | {row['passed']}/{row['runs']} | {row['pass_rate'] * 100:.0f}% | "
              f"{fmt_num(row['avg_seconds'])} | {fmt_num(row['avg_tokens'], 0)} | {fmt_num(row['avg_tool_calls'])} | {statuses} |")
    if report.get("output_file"):
        print(f"\nResults: {report['output_file']}")


# --------------------------------------------------------------------------- main
def desk_version(desk: str, env: dict[str, str]) -> str | None:
    try:
        out = subprocess.run([desk, "--version"], capture_output=True, text=True, timeout=20,
                             stdin=subprocess.DEVNULL, env=env)
        return out.stdout.strip().splitlines()[0] if out.returncode == 0 and out.stdout.strip() else None
    except (OSError, subprocess.SubprocessError):
        return None


def parse_args(argv=None):
    p = argparse.ArgumentParser(description="Run the DeskCode offline agent benchmark.")
    p.add_argument("--desk", default=str(REPO_DIR / "build" / "desk"), help="desk binary (default: ./build/desk)")
    p.add_argument("--tasks", help="comma separated task names or glob patterns (default: all)")
    p.add_argument("--repeat", type=int, default=1, help="runs per task (default 1)")
    p.add_argument("--jobs", type=int, default=1, help="parallel runs (default 1)")
    p.add_argument("--timeout", type=int, help="per-run timeout in seconds (default: task.json)")
    p.add_argument("--max-steps", type=int, help="override desk --max-steps (default: task.json)")
    p.add_argument("--label", help="result label, e.g. the model name (default: $DESK_MODEL)")
    p.add_argument("--out", help="result JSON path (default: bench/results/<timestamp>-<label>.json)")
    p.add_argument("--keep", action="store_true", help="keep temporary workspaces for inspection")
    p.add_argument("--self-test", action="store_true", help="verify check.sh with the reference solutions; no desk calls")
    p.add_argument("--desk-arg", action="append", help="extra argument passed to desk (repeatable), e.g. --desk-arg=--no-sandbox")
    p.add_argument("--list", action="store_true", help="list tasks and exit")
    p.add_argument("--verbose", action="store_true", help="show check output for failures")
    args = p.parse_args(argv)
    if args.repeat < 1 or args.jobs < 1:
        p.error("--repeat and --jobs must be >= 1")
    if args.max_steps is not None and not 0 <= args.max_steps <= 100:
        p.error("--max-steps must be within 0..100")
    return args


def cleanup_all() -> None:
    _STOP.set()
    with _ACTIVE_LOCK:
        procs = list(_ACTIVE)
    for proc in procs:
        _kill_group(proc)
    for root in list(_TEMP_DIRS):
        shutil.rmtree(root, ignore_errors=True)
        _TEMP_DIRS.discard(root)


def main(argv=None) -> int:
    args = parse_args(argv)
    tasks = discover(args.tasks)
    if not tasks:
        print(f"no tasks matched {args.tasks!r} in {TASKS_DIR}", file=sys.stderr)
        return 2
    if args.list:
        for t in tasks:
            print(f"{t.id:28} {t.meta.get('difficulty', '-'):7} {t.meta.get('category', '-'):16} {t.meta.get('title', '')}")
        return 0
    if shutil.which("git") is None:
        print("git is required", file=sys.stderr)
        return 2
    try:
        if args.self_test:
            return self_test(tasks, args)
        return run_bench(tasks, args)
    except KeyboardInterrupt:
        print("\ninterrupted; cleaning up", file=sys.stderr)
        return 130
    finally:
        cleanup_all()


def run_bench(tasks: list[Task], args) -> int:
    missing = [name for name in ("DESK_BASE_URL", "DESK_MODEL", "DESK_API_KEY") if not os.environ.get(name)]
    if missing:
        print("missing model configuration: " + ", ".join(missing) + "\n"
              "Set them in your shell for this command only, for example:\n"
              "  DESK_BASE_URL=https://api.example.com/v1 DESK_MODEL=<model> DESK_API_TYPE=openai \\\n"
              "  DESK_API_KEY=\"$(cat ~/.secrets/key)\" python3 bench/run_local.py\n"
              "(DESK_API_TYPE is openai or anthropic; the runner never reads ~/.config/desk-agent.)",
              file=sys.stderr)
        return 2
    api_type = os.environ.get("DESK_API_TYPE") or "openai"
    if api_type not in ("openai", "anthropic"):
        print("DESK_API_TYPE must be openai or anthropic", file=sys.stderr)
        return 2
    desk = Path(args.desk).expanduser()
    if not (desk.is_file() and os.access(desk, os.X_OK)):
        print(f"desk binary not found or not executable: {desk} (build it with make -j\"$(nproc)\")", file=sys.stderr)
        return 2
    args.desk = str(desk.resolve())

    redactor = Redactor(dict(os.environ))
    label = args.label or os.environ.get("DESK_MODEL") or "desk"
    safe_label = re.sub(r"[^A-Za-z0-9._-]+", "_", label).strip("_") or "desk"
    started = _dt.datetime.now(_dt.timezone.utc)
    out = Path(args.out) if args.out else RESULTS_DIR / f"{started.strftime('%Y%m%dT%H%M%SZ')}-{safe_label}.json"

    version_root = new_root()
    try:
        version = desk_version(args.desk, isolated_env(version_root, dict(os.environ), include_model=False))
    finally:
        remove_root(version_root)

    jobs = [(t, i) for t in tasks for i in range(args.repeat)]
    print(f"Running {len(jobs)} run(s) of {len(tasks)} task(s) with {args.jobs} job(s); desk {version or '?'}",
          file=sys.stderr)
    results = []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {pool.submit(run_task, t, i, args, redactor): (t, i) for t, i in jobs}
        try:
            for future in as_completed(futures):
                record = future.result()
                results.append(record)
                mark = "PASS" if record.get("passed") else "FAIL"
                extra = record.get("error") or record.get("status") or ""
                print(f"  [{len(results)}/{len(jobs)}] {mark} {record['task']}#{record['repeat']} "
                      f"{fmt_num(record.get('wall_seconds'))}s {extra}", file=sys.stderr)
        except KeyboardInterrupt:
            _STOP.set()
            for f in futures:
                f.cancel()
            raise
    order = {t.id: n for n, t in enumerate(tasks)}
    results.sort(key=lambda r: (order.get(r["task"], 0), r["repeat"]))

    report = {
        "schema": SCHEMA_VERSION,
        "label": label,
        "started_at": started.isoformat(timespec="seconds"),
        "finished_at": _dt.datetime.now(_dt.timezone.utc).isoformat(timespec="seconds"),
        "desk": args.desk,
        "desk_version": version,
        "model": os.environ.get("DESK_MODEL"),
        "api_type": api_type,
        "base_url": safe_url(os.environ.get("DESK_BASE_URL", "")),
        "settings": {"repeat": args.repeat, "jobs": args.jobs, "timeout": args.timeout, "max_steps": args.max_steps,
                     "desk_args": args.desk_arg or [], "tasks": [t.id for t in tasks]},
        "host": {"platform": sys.platform, "python": sys.version.split()[0]},
        "summary": build_summary(results),
        "results": results,
    }
    report = redactor.deep(report)
    out.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(report, ensure_ascii=False, indent=2) + "\n"
    out.write_text(redactor.text(text), encoding="utf-8")
    report["output_file"] = str(out)
    print_summary(report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
