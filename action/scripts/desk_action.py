#!/usr/bin/env python3
"""DeskCode GitHub Action driver (Python standard library only).

Reads the action inputs from ``INPUT_*`` environment variables (set by action.yml),
the triggering event from ``GITHUB_EVENT_PATH`` and talks to GitHub through the ``gh``
CLI.  Every piece of text that leaves this process (logs, comments, outputs, PR bodies)
is passed through :func:`redact` so the model API key and GitHub token never leak.

Test switches (not part of the public interface):
  DESK_ACTION_DESK_PATH   use this desk executable instead of downloading a release
  DESK_ACTION_GH          use this gh executable instead of the one on PATH
"""
from __future__ import annotations

import json
import os
import re
import secrets
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional

MARKER = "<!-- deskcode-action -->"
SELF_BOTS = {"github-actions[bot]"}
WRITE_PERMISSIONS = {"admin", "maintain", "write"}
BOT_NAME = "github-actions[bot]"
BOT_EMAIL = "41898282+github-actions[bot]@users.noreply.github.com"
COMMENT_LIMIT = 60000
ISSUE_ACTIONS = {"opened", "assigned", "labeled"}
STATUS_ICON = {
    "completed": "✅", "failed": "❌", "error": "❌", "not_configured": "⚙️",
    "interrupted": "⏹️", "approval_required": "✋", "denied": "🚫", "step_limit": "⏱️",
}

_SECRETS: List[str] = []


def redact(text: Any) -> str:
    text = "" if text is None else str(text)
    for secret in _SECRETS:
        if secret:
            text = text.replace(secret, "***")
    return text


def log(message: str) -> None:
    print(redact(message), flush=True)


def notice(message: str) -> None:
    log("::notice::" + message.replace("\n", " "))


def warning(message: str) -> None:
    log("::warning::" + message.replace("\n", " "))


def inp(name: str, default: str = "") -> str:
    value = os.environ.get("INPUT_" + name.upper().replace("-", "_"), "").strip()
    return value if value else default


def truthy(value: str) -> bool:
    return value.strip().lower() in ("1", "true", "yes", "on", "y")


class ActionError(Exception):
    pass


# --------------------------------------------------------------------------- gh / git


class GitHub:
    def __init__(self, repo: str, token: str) -> None:
        self.repo = repo
        self.exe = os.environ.get("DESK_ACTION_GH") or shutil.which("gh") or "gh"
        self.env = dict(os.environ)
        if token:
            self.env["GH_TOKEN"] = token
        self.env.setdefault("GH_PROMPT_DISABLED", "1")
        self.env.setdefault("NO_COLOR", "1")

    def call(self, args: List[str], payload: Optional[dict] = None, check: bool = True) -> Any:
        cmd = [self.exe] + args
        data = None
        if payload is not None:
            cmd += ["--input", "-"]
            data = json.dumps(payload)
        try:
            proc = subprocess.run(cmd, input=data, capture_output=True, text=True, env=self.env)
        except OSError as exc:
            raise ActionError(f"cannot run gh: {exc}") from exc
        if proc.returncode != 0:
            if check:
                raise ActionError(f"gh {args[0]} {args[1] if len(args) > 1 else ''} failed: "
                                  + redact(proc.stderr.strip() or proc.stdout.strip())[-800:])
            return None
        out = proc.stdout.strip()
        if out[:1] in ("{", "["):
            try:
                return json.loads(out)
            except ValueError:
                pass
        return out

    def api(self, path: str, method: str = "GET", payload: Optional[dict] = None, check: bool = True) -> Any:
        args = ["api", path]
        if method != "GET":
            args[1:1] = ["-X", method]
        return self.call(args, payload=payload, check=check)


def git(args: List[str], cwd: str, check: bool = True, env: Optional[Dict[str, str]] = None) -> str:
    full_env = dict(os.environ)
    full_env.setdefault("GIT_TERMINAL_PROMPT", "0")
    if env:
        full_env.update(env)
    proc = subprocess.run(["git"] + args, cwd=cwd, capture_output=True, text=True, env=full_env)
    if check and proc.returncode != 0:
        raise ActionError(f"git {' '.join(args[:2])} failed: " + redact(proc.stderr.strip())[-800:])
    return proc.stdout


# --------------------------------------------------------------------------- trigger


@dataclass
class Trigger:
    run: bool
    reason: str = ""
    request: str = ""
    context: str = ""
    title: str = ""
    issue_number: Optional[int] = None
    is_pr: bool = False
    pr: Optional[dict] = None
    comment_id: Optional[int] = None
    comment_kind: str = ""  # "issue" | "review"
    actor: str = ""
    actor_id: Optional[int] = None
    needs_permission: bool = False


def find_trigger(text: str, trigger: str) -> Optional[str]:
    """Return the text after the first standalone occurrence of ``trigger`` or None."""
    if not trigger:
        return text.strip()
    pattern = r"(?<![\w@/.-])" + re.escape(trigger) + r"(?![\w-])"
    match = re.search(pattern, text or "", re.IGNORECASE)
    if not match:
        return None
    return text[match.end():].strip()


def issue_context(kind: str, number: int, repo: str, title: str, body: str) -> str:
    body = (body or "").strip() or "(no description)"
    return f"GitHub {kind} #{number} in {repo}\nTitle: {title}\n\n{body}"


def parse_event(event_name: str, event: dict, prompt: str, trigger: str, repo: str) -> Trigger:
    sender = event.get("sender") or {}
    actor = sender.get("login", "") or os.environ.get("GITHUB_ACTOR", "")
    actor_id = sender.get("id")

    if event_name in ("issue_comment", "pull_request_review_comment"):
        comment = event.get("comment") or {}
        author = (comment.get("user") or {}).get("login", "")
        if event.get("action") not in (None, "created"):
            return Trigger(False, f"comment action '{event.get('action')}' is ignored")
        if author in SELF_BOTS or actor in SELF_BOTS:
            return Trigger(False, f"comment by {author or actor} (self-trigger guard)")
        body = comment.get("body") or ""
        if MARKER in body:
            return Trigger(False, "comment was written by DeskCode")
        after = find_trigger(body, trigger)
        if after is None:
            return Trigger(False, f"comment does not mention {trigger}")
        request = "\n\n".join(x for x in (prompt, after) if x) or "Please address the request described below."
        if event_name == "issue_comment":
            issue = event.get("issue") or {}
            is_pr = "pull_request" in issue
            number = issue.get("number")
            kind = "pull request" if is_pr else "issue"
            context = issue_context(kind, number, repo, issue.get("title", ""), issue.get("body", ""))
            return Trigger(True, request=request, context=context, title=issue.get("title", ""),
                           issue_number=number, is_pr=is_pr, comment_id=comment.get("id"),
                           comment_kind="issue", actor=author or actor,
                           actor_id=(comment.get("user") or {}).get("id", actor_id), needs_permission=True)
        pr = event.get("pull_request") or {}
        number = pr.get("number")
        context = issue_context("pull request", number, repo, pr.get("title", ""), pr.get("body", ""))
        where = comment.get("path", "")
        line = comment.get("line") or comment.get("original_line")
        if where:
            context += f"\n\nThe review comment is attached to {where}" + (f" line {line}" if line else "")
        if comment.get("diff_hunk"):
            context += "\nDiff hunk:\n```diff\n" + comment["diff_hunk"] + "\n```"
        return Trigger(True, request=request, context=context, title=pr.get("title", ""),
                       issue_number=number, is_pr=True, pr=pr, comment_id=comment.get("id"),
                       comment_kind="review", actor=author or actor,
                       actor_id=(comment.get("user") or {}).get("id", actor_id), needs_permission=True)

    if event_name == "issues":
        issue = event.get("issue") or {}
        if event.get("action") not in ISSUE_ACTIONS:
            return Trigger(False, f"issues action '{event.get('action')}' is ignored")
        if actor in SELF_BOTS:
            return Trigger(False, f"event sent by {actor} (self-trigger guard)")
        text = f"{issue.get('title', '')}\n{issue.get('body') or ''}"
        after = find_trigger(text, trigger)
        if after is None:
            return Trigger(False, f"issue does not mention {trigger}")
        request = "\n\n".join(x for x in (prompt, after) if x) or "Please resolve this issue."
        number = issue.get("number")
        context = issue_context("issue", number, repo, issue.get("title", ""), issue.get("body", ""))
        return Trigger(True, request=request, context=context, title=issue.get("title", ""),
                       issue_number=number, actor=actor, actor_id=actor_id, needs_permission=True)

    # workflow_dispatch, schedule, push, ... -> explicit prompt
    if not prompt:
        prompt = str((event.get("inputs") or {}).get("prompt") or "").strip()
    if not prompt:
        return Trigger(False, f"no prompt input for '{event_name}' event")
    return Trigger(True, request=prompt, actor=actor, actor_id=actor_id)


# --------------------------------------------------------------------------- outputs


def write_outputs(values: Dict[str, str]) -> None:
    path = os.environ.get("GITHUB_OUTPUT")
    lines = []
    for key, value in values.items():
        value = redact(value)
        delim = "DESK_EOF_" + secrets.token_hex(8)
        while delim in value:
            delim = "DESK_EOF_" + secrets.token_hex(8)
        lines.append(f"{key}<<{delim}\n{value}\n{delim}\n")
    if path:
        with open(path, "a", encoding="utf-8") as handle:
            handle.write("".join(lines))
    else:
        for key, value in values.items():
            log(f"output {key}={value[:200]}")


def write_summary(text: str) -> None:
    path = os.environ.get("GITHUB_STEP_SUMMARY")
    if path:
        with open(path, "a", encoding="utf-8") as handle:
            handle.write(redact(text) + "\n")


# --------------------------------------------------------------------------- desk


def install_desk(version: str) -> str:
    override = os.environ.get("DESK_ACTION_DESK_PATH")
    if override:
        return override
    runner_temp = os.environ.get("RUNNER_TEMP") or tempfile.gettempdir()
    dest = os.path.join(runner_temp, "deskcode")
    script = Path(__file__).resolve().parent / "install_desk.sh"
    proc = subprocess.run(["bash", str(script), version, dest], capture_output=True, text=True)
    sys.stderr.write(redact(proc.stderr))
    if proc.returncode != 0:
        raise ActionError("failed to install DeskCode " + version)
    path = proc.stdout.strip().splitlines()[-1]
    github_path = os.environ.get("GITHUB_PATH")
    if github_path:
        with open(github_path, "a", encoding="utf-8") as handle:
            handle.write(os.path.dirname(path) + "\n")
    return path


SENSITIVE_ENV = re.compile(r"^(INPUT_.*|GH_TOKEN|GITHUB_TOKEN|GH_ENTERPRISE_TOKEN|ACTIONS_RUNTIME_TOKEN|"
                           r"ACTIONS_ID_TOKEN_REQUEST_.*|ACTIONS_RESULTS_URL|ACTIONS_CACHE_URL)$")


def build_prompt(trig: Trigger, plan: bool) -> str:
    parts = [trig.request.strip()]
    if trig.context:
        parts.append("---\nContext from GitHub:\n" + trig.context.strip())
    notes = ["You are running non-interactively inside a GitHub Actions job; nobody can answer questions.",
             "Work directly in the current repository checkout. Do not run git commit, git push or create branches;",
             "the workflow commits and opens the pull request for you. Finish with a concise Markdown summary."]
    if plan:
        notes = notes[:1] + ["Only investigate and present a plan; do not modify files."]
    parts.append("---\n" + " ".join(notes))
    return "\n\n".join(parts)


def run_desk(desk: str, prompt: str, workspace: str, cfg: Dict[str, str]) -> Dict[str, Any]:
    runner_temp = os.environ.get("RUNNER_TEMP") or tempfile.gettempdir()
    scratch = tempfile.mkdtemp(prefix="deskcode-", dir=runner_temp)
    try:
        env = {k: v for k, v in os.environ.items() if not SENSITIVE_ENV.match(k)}
        env["XDG_CONFIG_HOME"] = os.path.join(scratch, "config")
        env["XDG_STATE_HOME"] = os.path.join(scratch, "state")
        env["XDG_CACHE_HOME"] = os.path.join(scratch, "cache")
        env["DESK_NO_ARROW"] = "1"
        for key in ("DESK_BASE_URL", "DESK_MODEL", "DESK_API_KEY", "DESK_API_TYPE"):
            env.pop(key, None)
            if cfg.get(key):
                env[key] = cfg[key]
        cmd = [desk, "--json", "-p", "-", "--approval", cfg["approval"], "--no-sandbox",
               "--max-steps", cfg["max_steps"]]
        if cfg["plan"]:
            cmd.append("--plan")
        log("::group::DeskCode run")
        log("$ " + " ".join(cmd))
        try:
            proc = subprocess.run(cmd, input=prompt, capture_output=True, text=True, cwd=workspace, env=env)
        except OSError as exc:
            log("::endgroup::")
            return {"status": "error", "error": f"cannot start desk: {exc}", "exit_code": -1}
        stderr = re.sub(r"[^\n]*\r", "", proc.stderr)  # drop spinner frames
        if stderr.strip():
            log(stderr.strip()[-6000:])
        log(f"exit code {proc.returncode}")
        log("::endgroup::")
        result = None
        for line in reversed(proc.stdout.splitlines()):
            line = line.strip()
            if line.startswith("{"):
                try:
                    candidate = json.loads(line)
                except ValueError:
                    continue
                if isinstance(candidate, dict) and "status" in candidate:
                    result = candidate
                    break
        if result is None:
            tail = (proc.stderr.strip() or proc.stdout.strip())[-1500:]
            return {"status": "error", "exit_code": proc.returncode,
                    "error": f"desk exited with code {proc.returncode} without a JSON result.\n{tail}"}
        result["exit_code"] = proc.returncode
        return result
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


# --------------------------------------------------------------------------- git helpers


def changed_files(workspace: str) -> List[str]:
    raw = git(["status", "--porcelain=v1", "-z", "--untracked-files=all"], workspace)
    files: List[str] = []
    entries = raw.split("\0")
    i = 0
    while i < len(entries):
        entry = entries[i]
        i += 1
        if len(entry) < 4:
            continue
        status, path = entry[:2], entry[3:]
        if "R" in status or "C" in status:
            i += 1  # skip the original path of a rename/copy
        files.append(path)
    return files


def commit_all(workspace: str, message: str) -> str:
    git(["add", "-A"], workspace)
    env = {"GIT_AUTHOR_NAME": BOT_NAME, "GIT_AUTHOR_EMAIL": BOT_EMAIL,
           "GIT_COMMITTER_NAME": BOT_NAME, "GIT_COMMITTER_EMAIL": BOT_EMAIL}
    proc = subprocess.run(["git", "-c", "commit.gpgsign=false", "commit", "-q", "--no-verify", "-F", "-"],
                          cwd=workspace, input=message, capture_output=True, text=True,
                          env={**os.environ, **env})
    if proc.returncode != 0:
        raise ActionError("git commit failed: " + redact(proc.stderr.strip())[-800:])
    return git(["rev-parse", "HEAD"], workspace).strip()


def commit_message(trig: Trigger) -> str:
    first = next((line.strip() for line in trig.request.splitlines() if line.strip()), "DeskCode changes")
    first = re.sub(r"\s+", " ", first)
    if len(first) > 72:
        first = first[:69].rstrip() + "..."
    body = ["", "Generated by DeskCode (https://github.com/Milorgs/DeskCode)."]
    if trig.issue_number and not trig.is_pr:
        body.append(f"Refs #{trig.issue_number}")
    if trig.actor and trig.actor_id and not trig.actor.endswith("[bot]"):
        body += ["", f"Co-authored-by: {trig.actor} <{trig.actor_id}+{trig.actor}@users.noreply.github.com>"]
    return first + "\n" + "\n".join(body) + "\n"


def exclude_desk_state(workspace: str) -> None:
    path = git(["rev-parse", "--git-path", "info/exclude"], workspace).strip()
    path = os.path.join(workspace, path) if not os.path.isabs(path) else path
    os.makedirs(os.path.dirname(path), exist_ok=True)
    current = Path(path).read_text(encoding="utf-8") if os.path.exists(path) else ""
    if ".desk-agent/" not in current.split("\n"):
        with open(path, "a", encoding="utf-8") as handle:
            handle.write(("" if current.endswith("\n") or not current else "\n") + ".desk-agent/\n")


# --------------------------------------------------------------------------- formatting


def fmt_int(value: Any) -> str:
    try:
        return f"{int(value):,}"
    except (TypeError, ValueError):
        return str(value)


def fmt_cost(usage: dict) -> str:
    cost = usage.get("cost")
    if cost is None:
        return ""
    try:
        return f"{float(cost):.4f}".rstrip("0").rstrip(".") or "0"
    except (TypeError, ValueError):
        return str(cost)


def result_comment(status: str, result: dict, files: List[str], pr_url: str, branch: str,
                   note: str, run_url: str, plan: bool) -> str:
    icon = STATUS_ICON.get(status, "ℹ️")
    title = "plan" if plan and status == "completed" else status
    lines = [MARKER, f"### 🤖 DeskCode — {icon} `{title}`", ""]
    reply = (result.get("reply") or "").strip()
    error = (result.get("error") or "").strip()
    if reply:
        lines += [reply, ""]
    if error and status != "completed":
        lines += ["**Error / 错误:**", "", "```text", error[-3000:], "```", ""]
    if files:
        lines.append(f"**Changed files / 改动文件 ({len(files)}):**")
        lines += [f"- `{f}`" for f in files[:100]]
        if len(files) > 100:
            lines.append(f"- … and {len(files) - 100} more")
        lines.append("")
    if pr_url:
        lines += [f"**Pull request:** {pr_url}", ""]
    elif branch:
        lines += [f"**Pushed to / 已推送到:** `{branch}`", ""]
    if note:
        lines += [f"> {note}", ""]
    usage = result.get("usage") or {}
    meta = []
    if result.get("tokens") is not None:
        detail = ", ".join(f"{k} {fmt_int(usage[k])}" for k in ("input", "output", "cache_read") if k in usage)
        meta.append(f"Tokens: {fmt_int(result.get('tokens'))}" + (f" ({detail})" if detail else ""))
    cost = fmt_cost(usage)
    if cost:
        meta.append(f"Cost: {cost} {usage.get('currency') or ''}".rstrip())
    if result.get("model"):
        meta.append(f"Model: `{result['model']}`")
    if run_url:
        meta.append(f"[Actions run]({run_url})")
    if meta:
        lines.append("<sub>" + " · ".join(meta) + "</sub>")
    text = "\n".join(lines)
    if len(text) > COMMENT_LIMIT:
        text = text[:COMMENT_LIMIT] + "\n\n… (truncated)"
    return redact(text)


# --------------------------------------------------------------------------- main flow


def main() -> int:
    outputs = {"status": "skipped", "reply": "", "pr_url": "", "branch": "", "tokens": "0", "cost": ""}
    try:
        code = run(outputs)
    except ActionError as exc:
        log("::error::" + redact(exc).replace("\n", " "))
        outputs["status"] = "error"
        outputs["reply"] = redact(exc)
        code = 1
    finally:
        write_outputs(outputs)
    return code


def run(outputs: Dict[str, str]) -> int:
    api_key = inp("api_key")
    token = inp("github_token") or os.environ.get("GITHUB_TOKEN", "")
    _SECRETS.extend(s for s in (api_key, token) if s)
    _SECRETS.sort(key=len, reverse=True)

    repo = os.environ.get("GITHUB_REPOSITORY", "")
    event_name = os.environ.get("GITHUB_EVENT_NAME", "")
    event_path = os.environ.get("GITHUB_EVENT_PATH", "")
    event: dict = {}
    if event_path and os.path.exists(event_path):
        with open(event_path, encoding="utf-8") as handle:
            event = json.load(handle)
    server = os.environ.get("GITHUB_SERVER_URL", "https://github.com")
    run_id = os.environ.get("GITHUB_RUN_ID", "0")
    run_url = f"{server}/{repo}/actions/runs/{run_id}" if repo else ""
    plan = truthy(inp("plan", "false"))
    create_pr = truthy(inp("create_pr", "true"))

    trig = parse_event(event_name, event, inp("prompt"), inp("trigger", "@desk"), repo)
    if not trig.run:
        notice(f"DeskCode skipped: {trig.reason}")
        write_summary(f"DeskCode skipped: {trig.reason}")
        return 0

    gh = GitHub(repo, token)
    if trig.needs_permission:
        perm = gh.api(f"repos/{repo}/collaborators/{trig.actor}/permission", check=False)
        level = perm.get("permission", "none") if isinstance(perm, dict) else "none"
        role = perm.get("role_name", "") if isinstance(perm, dict) else ""
        if level not in WRITE_PERMISSIONS and role not in WRITE_PERMISSIONS:
            notice(f"DeskCode skipped: @{trig.actor} has '{level}' permission on {repo}; "
                   "write, maintain or admin is required.")
            write_summary(f"DeskCode skipped: @{trig.actor} lacks write permission ({level}).")
            return 0

    if not api_key:
        raise ActionError("The 'api_key' input is required (store it in a repository secret).")

    # --- feedback: 👀 reaction + progress comment
    if trig.comment_id:
        kind = "pulls" if trig.comment_kind == "review" else "issues"
        gh.api(f"repos/{repo}/{kind}/comments/{trig.comment_id}/reactions", "POST", {"content": "eyes"}, check=False)
    progress_id = None
    if trig.issue_number:
        body = (f"{MARKER}\n⏳ **DeskCode** is working on this… / 正在处理…\n\n"
                f"[Actions run]({run_url})")
        created = gh.api(f"repos/{repo}/issues/{trig.issue_number}/comments", "POST", {"body": body}, check=False)
        if isinstance(created, dict):
            progress_id = created.get("id")

    def report(status: str, result: dict, files: List[str], pr_url: str = "", branch: str = "",
               note: str = "") -> None:
        text = result_comment(status, result, files, pr_url, branch, note, run_url, plan)
        write_summary(text.replace(MARKER, ""))
        if not trig.issue_number:
            return
        if progress_id:
            if gh.api(f"repos/{repo}/issues/comments/{progress_id}", "PATCH", {"body": text}, check=False) is not None:
                return
        gh.api(f"repos/{repo}/issues/{trig.issue_number}/comments", "POST", {"body": text}, check=False)

    try:
        return execute(trig, gh, repo, run_id, plan, create_pr, api_key, outputs, report)
    except ActionError as exc:
        outputs["status"] = "error"
        outputs["reply"] = redact(exc)
        report("error", {"error": str(exc)}, [])
        log("::error::" + redact(exc).replace("\n", " "))
        return 1


def execute(trig: Trigger, gh: GitHub, repo: str, run_id: str, plan: bool, create_pr: bool,
            api_key: str, outputs: Dict[str, str], report) -> int:
    workspace = os.environ.get("GITHUB_WORKSPACE") or os.getcwd()
    if git(["rev-parse", "--is-inside-work-tree"], workspace, check=False).strip() != "true":
        raise ActionError(f"{workspace} is not a git checkout; add actions/checkout before DeskCode.")

    # --- PR scenario: work on the PR head
    pr_branch = ""
    fork = False
    if trig.is_pr:
        pr = trig.pr if trig.pr and trig.pr.get("head") else gh.api(f"repos/{repo}/pulls/{trig.issue_number}")
        head = (pr or {}).get("head") or {}
        head_repo = ((head.get("repo") or {}).get("full_name") or "")
        fork = head_repo.lower() != repo.lower()
        if not fork:
            pr_branch = head.get("ref", "")
            git(["fetch", "--no-tags", "origin", f"+refs/heads/{pr_branch}:refs/remotes/origin/{pr_branch}"], workspace)
            git(["checkout", "-q", "-B", pr_branch, f"refs/remotes/origin/{pr_branch}"], workspace)
        else:
            ref = f"refs/remotes/pull/{trig.issue_number}/head"
            git(["fetch", "--no-tags", "origin", f"+refs/pull/{trig.issue_number}/head:{ref}"], workspace)
            git(["checkout", "-q", "--detach", ref], workspace)

    base = git(["rev-parse", "--abbrev-ref", "HEAD"], workspace, check=False).strip()
    if not base or base == "HEAD":
        event_repo = {}
        try:
            with open(os.environ.get("GITHUB_EVENT_PATH", ""), encoding="utf-8") as handle:
                event_repo = json.load(handle).get("repository") or {}
        except (OSError, ValueError):
            pass
        base = event_repo.get("default_branch") or os.environ.get("GITHUB_REF_NAME", "main")
    exclude_desk_state(workspace)

    desk = install_desk(inp("version", "latest"))
    cfg = {
        "DESK_API_KEY": api_key,
        "DESK_BASE_URL": inp("base_url"),
        "DESK_MODEL": inp("model"),
        "DESK_API_TYPE": inp("api_type", "openai"),
        "approval": inp("approval", "full"),
        "max_steps": inp("max_steps", "40"),
        "plan": plan,
    }
    if cfg["approval"] not in ("ask", "edit", "full"):
        raise ActionError(f"invalid approval '{cfg['approval']}' (ask|edit|full)")
    if not cfg["max_steps"].isdigit():
        raise ActionError(f"invalid max_steps '{cfg['max_steps']}'")

    result = run_desk(desk, build_prompt(trig, plan), workspace, cfg)
    status = str(result.get("status") or "error")
    usage = result.get("usage") or {}
    outputs.update(status=status, reply=redact(result.get("reply") or result.get("error") or ""),
                   tokens=str(result.get("tokens") or 0), cost=fmt_cost(usage))

    files = changed_files(workspace)
    pr_url = branch = note = ""
    if files and plan:
        note = "Plan mode: changes were not committed. / 计划模式：未提交任何改动。"
    elif files and status != "completed":
        note = (f"DeskCode did not complete (`{status}`); changes were not committed. "
                f"/ 任务未完成，改动未提交。")
    elif files and trig.is_pr:
        if fork:
            note = ("This pull request comes from a fork, so DeskCode did not push. "
                    "/ 该 PR 来自 fork，未推送改动。")
        else:
            commit_all(workspace, commit_message(trig))
            git(["push", "origin", f"HEAD:refs/heads/{pr_branch}"], workspace)
            branch = pr_branch
    elif files and create_pr:
        sha = commit_all(workspace, commit_message(trig))
        branch = f"{inp('branch_prefix', 'desk/')}{trig.issue_number or run_id}-{sha[:7]}"
        git(["push", "origin", f"HEAD:refs/heads/{branch}"], workspace)
        title = commit_message(trig).splitlines()[0]
        body = [f"**Request / 需求:**\n\n{trig.request.strip()}", "",
                f"**DeskCode:**\n\n{(result.get('reply') or '').strip()}", ""]
        if trig.issue_number:
            body.append(f"Closes #{trig.issue_number}")
        body.append(f"\n<sub>Generated by DeskCode · [Actions run]({os.environ.get('GITHUB_SERVER_URL', 'https://github.com')}"
                    f"/{repo}/actions/runs/{run_id})</sub>")
        out = gh.call(["pr", "create", "--repo", repo, "--base", base, "--head", branch,
                       "--title", title, "--body", redact("\n".join(body))[:COMMENT_LIMIT]])
        urls = re.findall(r"https?://\S+", str(out))
        pr_url = urls[-1] if urls else str(out).strip()
    elif files:
        note = "create_pr is false; changes are left in the workspace. / 未创建 PR，改动保留在工作区。"

    outputs.update(pr_url=pr_url, branch=branch)
    report(status, result, files, pr_url, branch, note)
    if status != "completed":
        warning(f"DeskCode finished with status '{status}'")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
