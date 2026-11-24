#!/usr/bin/env python3
"""End-to-end tests for the DeskCode GitHub Action driver (stdlib unittest only).

Each test builds a throw-away world inside one temporary directory:
  * a bare ``origin`` repository plus a clone used as ``GITHUB_WORKSPACE``;
  * a fake ``gh`` executable (first on PATH) that logs every call and answers from a JSON state file;
  * a forged ``GITHUB_EVENT_PATH`` event, ``GITHUB_OUTPUT`` and ``GITHUB_STEP_SUMMARY`` files;
  * the offline fake model ``bench/smoke_model.py`` and this repository's ``./build/desk``
    (via the ``DESK_ACTION_DESK_PATH`` test switch, so nothing is downloaded).

Run: python3 action/tests/test_action.py   (build first: make -j"$(nproc)" build/desk)
Set DESK_ACTION_TEST_DESK=/path/to/desk to test another binary.
"""
from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

sys.dont_write_bytecode = True
from pathlib import Path

HERE = Path(__file__).resolve().parent
ACTION = HERE.parent
REPO_ROOT = ACTION.parent
SCRIPT = ACTION / "scripts" / "desk_action.py"
SMOKE_MODEL = REPO_ROOT / "bench" / "smoke_model.py"
DESK = Path(os.environ.get("DESK_ACTION_TEST_DESK") or REPO_ROOT / "build" / "desk")
SECRET = "sk-test-DESKACTION-7f3a9c1e5b"
GH_TOKEN = "ghs_FAKEtokenFORtests0123456789"
REPO = "octo/demo"

FAKE_GH = r'''#!/usr/bin/env python3
import json, os, sys
args = sys.argv[1:]
stdin = sys.stdin.read() if "--input" in args else ""
state_path = os.environ["FAKE_GH_STATE"]
state = json.load(open(state_path))
with open(os.environ["FAKE_GH_LOG"], "a") as log:
    log.write(json.dumps({"args": args, "stdin": stdin,
                          "has_token": bool(os.environ.get("GH_TOKEN"))}) + "\n")
def out(obj):
    print(json.dumps(obj)); sys.exit(0)
if args[:1] == ["api"]:
    method = "GET"
    if "-X" in args:
        method = args[args.index("-X") + 1]
    path = [a for a in args[1:] if not a.startswith("-") and a not in ("GET", "POST", "PATCH")][0]
    if path.endswith("/permission"):
        user = path.split("/")[-2]
        perm = state.get("permissions", {}).get(user)
        if perm is None:
            sys.stderr.write("HTTP 404: Not Found\n"); sys.exit(1)
        out({"permission": perm, "role_name": perm, "user": {"login": user}})
    if "/pulls/" in path and method == "GET" and "/comments" not in path:
        number = path.rsplit("/", 1)[-1]
        out(state["pulls"][number])
    if path.endswith("/reactions"):
        out({"id": 1, "content": "eyes"})
    if path.endswith("/comments") and method == "POST":
        state["next_comment"] = state.get("next_comment", 900) + 1
        json.dump(state, open(state_path, "w"))
        out({"id": state["next_comment"], "html_url": "https://github.com/x/comment"})
    if "/issues/comments/" in path and method == "PATCH":
        out({"id": int(path.rsplit("/", 1)[-1])})
    sys.stderr.write("fake gh: unhandled api " + path + "\n"); sys.exit(1)
if args[:2] == ["pr", "create"]:
    print("https://github.com/octo/demo/pull/42"); sys.exit(0)
sys.stderr.write("fake gh: unhandled " + " ".join(args) + "\n"); sys.exit(1)
'''

APP_INI = "[server]\nport = 8080\nlegacy_mode = true\ntimeout_seconds = 30\n\n[db]\npool_size = 5\n"


def git(cwd, *args):
    return subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True, text=True).stdout


def parse_outputs(path: Path) -> dict:
    text = path.read_text() if path.exists() else ""
    values = {}
    for match in re.finditer(r"^(\w+)<<(\S+)\n(.*?)\n\2\n", text, re.S | re.M):
        values[match.group(1)] = match.group(3)
    return values


@unittest.skipUnless(DESK.exists(), f"{DESK} not built; run make -j\"$(nproc)\" build/desk")
class ActionEndToEnd(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = Path(tempfile.mkdtemp(prefix="desk-action-test-"))
        port_file = cls.root / "port"
        cls.model = subprocess.Popen([sys.executable, "-I", str(SMOKE_MODEL), str(port_file), SECRET],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(100):
            if port_file.exists() and port_file.read_text().strip():
                break
            time.sleep(0.05)
        cls.base_url = f"http://127.0.0.1:{port_file.read_text().strip()}/v1"

    @classmethod
    def tearDownClass(cls):
        cls.model.terminate()
        cls.model.wait(timeout=5)
        shutil.rmtree(cls.root, ignore_errors=True)

    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(prefix="case-", dir=self.root))
        for name in ("home", "xdg-config", "xdg-state", "runner", "bin"):
            (self.dir / name).mkdir()
        self.origin = self.dir / "origin.git"
        self.ws = self.dir / "ws"
        git(self.dir, "init", "-q", "--bare", "-b", "main", str(self.origin))
        seed = self.dir / "seed"
        git(self.dir, "init", "-q", "-b", "main", str(seed))
        (seed / "config").mkdir()
        (seed / "config" / "app.ini").write_text(APP_INI)
        (seed / "README.md").write_text("demo\n")
        git(seed, "add", "-A")
        git(seed, "-c", "user.name=t", "-c", "user.email=t@e", "commit", "-qm", "init")
        git(seed, "remote", "add", "origin", str(self.origin))
        git(seed, "push", "-q", "origin", "main")
        git(seed, "checkout", "-qb", "feature")
        (seed / "feature.txt").write_text("feature\n")
        git(seed, "add", "-A")
        git(seed, "-c", "user.name=t", "-c", "user.email=t@e", "commit", "-qm", "feature")
        git(seed, "push", "-q", "origin", "feature")
        git(self.dir, "clone", "-q", str(self.origin), str(self.ws))
        gh = self.dir / "bin" / "gh"
        gh.write_text(FAKE_GH.replace("#!/usr/bin/env python3", "#!" + sys.executable, 1))
        gh.chmod(0o755)
        self.gh_log = self.dir / "gh.log"
        self.gh_state = self.dir / "gh-state.json"
        self.state = {"permissions": {"alice": "write", "bob": "read"},
                      "pulls": {"5": {"number": 5, "head": {"ref": "feature", "sha": "x",
                                                         "repo": {"full_name": REPO}}},
                                "6": {"number": 6, "head": {"ref": "patch-1", "sha": "y",
                                                         "repo": {"full_name": "mallory/demo"}}}}}
        self.gh_state.write_text(json.dumps(self.state))

    # ------------------------------------------------------------------ helpers
    def run_action(self, event_name, event, desk=None, **inputs):
        event.setdefault("repository", {"full_name": REPO, "default_branch": "main"})
        event_path = self.dir / "event.json"
        event_path.write_text(json.dumps(event))
        self.out_path = self.dir / "github_output"
        self.summary_path = self.dir / "summary.md"
        env = {
            "PATH": f"{self.dir / 'bin'}{os.pathsep}{os.environ['PATH']}",
            "HOME": str(self.dir / "home"),
            "XDG_CONFIG_HOME": str(self.dir / "xdg-config"),
            "XDG_STATE_HOME": str(self.dir / "xdg-state"),
            "GIT_CONFIG_NOSYSTEM": "1",
            "LANG": "C.UTF-8",
            "GITHUB_EVENT_NAME": event_name,
            "GITHUB_EVENT_PATH": str(event_path),
            "GITHUB_REPOSITORY": REPO,
            "GITHUB_RUN_ID": "777",
            "GITHUB_SERVER_URL": "https://github.com",
            "GITHUB_WORKSPACE": str(self.ws),
            "GITHUB_OUTPUT": str(self.out_path),
            "GITHUB_STEP_SUMMARY": str(self.summary_path),
            "RUNNER_TEMP": str(self.dir / "runner"),
            "FAKE_GH_LOG": str(self.gh_log),
            "FAKE_GH_STATE": str(self.gh_state),
            "DESK_ACTION_DESK_PATH": str(desk or DESK),
            "INPUT_API_KEY": SECRET,
            "INPUT_GITHUB_TOKEN": GH_TOKEN,
            "INPUT_BASE_URL": self.base_url,
            "INPUT_MODEL": "smoke-model",
            "INPUT_TRIGGER": "@desk",
        }
        for key, value in inputs.items():
            env["INPUT_" + key.upper()] = str(value)
        proc = subprocess.run([sys.executable, str(SCRIPT)], cwd=str(self.ws), env=env,
                              capture_output=True, text=True, timeout=120)
        self.proc = proc
        self.outputs = parse_outputs(self.out_path)
        self.calls = [json.loads(l) for l in self.gh_log.read_text().splitlines()] if self.gh_log.exists() else []
        self.assert_no_secret()
        return proc

    def assert_no_secret(self):
        blobs = [self.proc.stdout, self.proc.stderr, self.gh_log.read_text() if self.gh_log.exists() else "",
                 self.out_path.read_text() if self.out_path.exists() else "",
                 self.summary_path.read_text() if self.summary_path.exists() else ""]
        for path in self.ws.rglob("*"):
            if path.is_file() and ".git" not in path.parts:
                blobs.append(path.read_text(errors="ignore"))
        blobs.append(git(self.origin, "log", "--all", "--format=%B"))
        for blob in blobs:
            self.assertNotIn(SECRET, blob)
            self.assertNotIn(GH_TOKEN, blob)

    def api_calls(self, needle, method=None):
        result = []
        for call in self.calls:
            args = call["args"]
            if args[:1] != ["api"] or not any(needle in a for a in args):
                continue
            m = args[args.index("-X") + 1] if "-X" in args else "GET"
            if method is None or method == m:
                result.append(call)
        return result

    def final_comment(self):
        bodies = [json.loads(c["stdin"])["body"] for c in self.calls
                  if c["args"][:1] == ["api"] and c["stdin"] and ("PATCH" in c["args"] or "POST" in c["args"])
                  and "/reactions" not in " ".join(c["args"])]
        self.assertTrue(bodies, "no comment written")
        return bodies[-1]

    def comment_event(self, body, user="alice", pr=False, number=3):
        issue = {"number": number, "title": "Update the server config",
                 "body": "Ports and TLS need updating.", "user": {"login": "carol"}}
        if pr:
            issue["pull_request"] = {"url": "https://api.github.com/repos/octo/demo/pulls/%d" % number}
        return {"action": "created", "issue": issue,
                "comment": {"id": 555, "body": body, "user": {"login": user, "id": 1001}},
                "sender": {"login": user, "id": 1001}}

    # ------------------------------------------------------------------ tests
    def test_issue_comment_creates_branch_and_pr(self):
        proc = self.run_action("issue_comment",
                               self.comment_event("Hi @desk please update config/app.ini as described"))
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        out = self.outputs
        self.assertEqual(out["status"], "completed")
        self.assertEqual(out["pr_url"], "https://github.com/octo/demo/pull/42")
        self.assertRegex(out["branch"], r"^desk/3-[0-9a-f]{7}$")
        self.assertEqual(out["tokens"], "1920")
        self.assertIn("Updated config/app.ini", out["reply"])
        # permission check, reaction and progress comment happened, all with a token
        self.assertTrue(self.api_calls("collaborators/alice/permission"))
        self.assertTrue(self.api_calls("issues/comments/555/reactions", "POST"))
        self.assertTrue(all(c["has_token"] for c in self.calls))
        progress = self.api_calls("issues/3/comments", "POST")
        self.assertIn("正在处理", json.loads(progress[0]["stdin"])["body"])
        # branch pushed to origin with the bot identity and the file change
        branch = out["branch"]
        log = git(self.origin, "log", "-1", "--format=%an <%ae>%n%B", branch)
        self.assertIn("github-actions[bot] <41898282+github-actions[bot]@users.noreply.github.com>", log)
        self.assertIn("please update config/app.ini", log)
        self.assertIn("Co-authored-by: alice <1001+alice@users.noreply.github.com>", log)
        show = git(self.origin, "show", "--stat", "--format=", branch)
        self.assertIn("config/app.ini", show)
        self.assertNotIn(".desk-agent", show)
        self.assertIn("port = 8443", git(self.origin, "show", f"{branch}:config/app.ini"))
        # gh pr create
        pr = [c["args"] for c in self.calls if c["args"][:2] == ["pr", "create"]]
        self.assertEqual(len(pr), 1)
        args = pr[0]
        self.assertEqual(args[args.index("--base") + 1], "main")
        self.assertEqual(args[args.index("--head") + 1], branch)
        self.assertIn("Closes #3", args[args.index("--body") + 1])
        # final comment updates the progress comment
        patch = self.api_calls("issues/comments/", "PATCH")
        self.assertEqual(len(patch), 1)
        body = self.final_comment()
        for needle in ("deskcode-action", "completed", "Updated config/app.ini", "`config/app.ini`",
                       "https://github.com/octo/demo/pull/42", "Tokens: 1,920",
                       "https://github.com/octo/demo/actions/runs/777"):
            self.assertIn(needle, body)
        self.assertIn("pull/42", self.summary_path.read_text())

    def test_comment_without_trigger_is_skipped(self):
        proc = self.run_action("issue_comment", self.comment_event("just chatting about desktops @desktop"))
        self.assertEqual(proc.returncode, 0)
        self.assertEqual(self.outputs["status"], "skipped")
        self.assertEqual(self.calls, [])
        self.assertIn("skipped", proc.stdout)

    def test_bot_comment_is_skipped(self):
        proc = self.run_action("issue_comment", self.comment_event("@desk fix config/app.ini",
                                                                   user="github-actions[bot]"))
        self.assertEqual(proc.returncode, 0)
        self.assertEqual(self.outputs["status"], "skipped")
        self.assertEqual(self.calls, [])

    def test_reader_without_write_permission_is_skipped(self):
        proc = self.run_action("issue_comment", self.comment_event("@desk update config/app.ini", user="bob"))
        self.assertEqual(proc.returncode, 0)
        self.assertEqual(self.outputs["status"], "skipped")
        self.assertEqual(len(self.calls), 1)
        self.assertIn("write, maintain or admin", proc.stdout)
        # unknown users (404) are skipped as well
        proc = self.run_action("issue_comment", self.comment_event("@desk update config/app.ini", user="eve"))
        self.assertEqual(self.outputs["status"], "skipped")
        self.assertEqual(git(self.origin, "branch", "--list", "desk/*").strip(), "")

    def test_pr_comment_pushes_to_head_branch(self):
        before = git(self.origin, "rev-parse", "feature").strip()
        proc = self.run_action("issue_comment",
                               self.comment_event("@desk update config/app.ini please", pr=True, number=5))
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(self.outputs["branch"], "feature")
        self.assertEqual(self.outputs["pr_url"], "")
        after = git(self.origin, "rev-parse", "feature").strip()
        self.assertNotEqual(before, after)
        self.assertEqual(git(self.origin, "rev-parse", "feature~1").strip(), before)
        self.assertFalse([c for c in self.calls if c["args"][:2] == ["pr", "create"]])
        self.assertNotIn("Refs #5", git(self.origin, "log", "-1", "--format=%B", "feature"))
        self.assertIn("`feature`", self.final_comment())

    def test_fork_pr_comment_does_not_push(self):
        (self.ws / ".git").exists()
        # simulate GitHub's refs/pull/6/head on origin
        git(self.origin, "update-ref", "refs/pull/6/head", "feature")
        refs_before = git(self.origin, "for-each-ref")
        proc = self.run_action("issue_comment",
                               self.comment_event("@desk update config/app.ini", pr=True, number=6))
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(self.outputs["branch"], "")
        self.assertEqual(git(self.origin, "for-each-ref"), refs_before)
        self.assertIn("fork", self.final_comment())

    def test_review_comment_trigger(self):
        event = {"action": "created",
                 "comment": {"id": 77, "body": "@desk update config/app.ini here", "path": "config/app.ini",
                             "line": 2, "diff_hunk": "@@ -1 +1 @@", "user": {"login": "alice", "id": 1001}},
                 "pull_request": {"number": 5, "title": "Feature", "body": "",
                                  "head": {"ref": "feature", "repo": {"full_name": REPO}}},
                 "sender": {"login": "alice", "id": 1001}}
        proc = self.run_action("pull_request_review_comment", event)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertTrue(self.api_calls("pulls/comments/77/reactions", "POST"))
        self.assertEqual(self.outputs["branch"], "feature")

    def test_issues_opened_with_trigger(self):
        event = {"action": "opened",
                 "issue": {"number": 9, "title": "Config", "body": "@desk update config/app.ini for TLS",
                           "user": {"login": "alice"}},
                 "sender": {"login": "alice", "id": 1001}}
        proc = self.run_action("issues", event)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertRegex(self.outputs["branch"], r"^desk/9-")
        # an issue edited event is ignored
        event["action"] = "edited"
        self.run_action("issues", event)
        self.assertEqual(self.outputs["status"], "skipped")

    def test_workflow_dispatch_plan_mode(self):
        proc = self.run_action("workflow_dispatch", {"inputs": {}, "sender": {"login": "alice"}},
                               prompt="Explain how to harden the service\nsecond line", plan="true")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(self.outputs["status"], "completed")
        self.assertIn("Smoke-test model", self.outputs["reply"])
        self.assertEqual(self.outputs["branch"], "")
        self.assertFalse(self.api_calls("permission"))  # no actor check for dispatch
        self.assertFalse(self.api_calls("comments"))     # no issue to comment on
        self.assertIn("DeskCode", self.summary_path.read_text())

    def test_workflow_dispatch_without_prompt_is_skipped(self):
        self.run_action("workflow_dispatch", {"inputs": {}})
        self.assertEqual(self.outputs["status"], "skipped")

    def test_dispatch_with_branch_prefix_and_run_id(self):
        proc = self.run_action("workflow_dispatch", {"inputs": {"prompt": "update config/app.ini"}},
                               branch_prefix="bot/")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertRegex(self.outputs["branch"], r"^bot/777-[0-9a-f]{7}$")
        args = [c["args"] for c in self.calls if c["args"][:2] == ["pr", "create"]][0]
        self.assertNotIn("Closes #", args[args.index("--body") + 1])

    def test_create_pr_false_leaves_changes(self):
        proc = self.run_action("workflow_dispatch", {}, prompt="update config/app.ini", create_pr="false")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(self.outputs["branch"], "")
        self.assertIn("config/app.ini", git(self.ws, "status", "--porcelain"))

    def test_desk_failure_reports_error_without_leaking_key(self):
        broken = self.dir / "bin" / "broken-desk"
        broken.write_text("#!/bin/sh\necho \"fatal: rejected key $DESK_API_KEY\" >&2\nexit 1\n")
        broken.chmod(0o755)
        proc = self.run_action("issue_comment", self.comment_event("@desk update config/app.ini"), desk=broken)
        self.assertEqual(proc.returncode, 1)
        self.assertEqual(self.outputs["status"], "error")
        body = self.final_comment()
        self.assertIn("without a JSON result", body)
        self.assertIn("rejected key ***", body)
        self.assertIn("rejected key ***", proc.stdout)

    def test_desk_env_is_isolated(self):
        spy = self.dir / "bin" / "spy-desk"
        spy.write_text("#!/bin/sh\nenv > \"$RUNNER_TEMP/../desk-env.txt\"\ncat > \"$RUNNER_TEMP/../prompt.txt\"\n"
                       "printf '%s\\n' '{\"status\":\"completed\",\"reply\":\"ok\",\"tokens\":5,"
                       "\"usage\":{\"input\":3,\"output\":2,\"cache_read\":0,\"cost\":0.0123,\"currency\":\"USD\"}}'\n")
        spy.chmod(0o755)
        self.run_action("issue_comment", self.comment_event("@desk say hi"), desk=spy)
        env = (self.dir / "desk-env.txt").read_text()
        self.assertNotIn("GH_TOKEN", env)
        self.assertNotIn("INPUT_", env)
        self.assertIn("DESK_API_TYPE=openai", env)
        self.assertIn(f"XDG_CONFIG_HOME={self.dir / 'runner'}", env)
        prompt = (self.dir / "prompt.txt").read_text()
        self.assertTrue(prompt.startswith("say hi"))
        self.assertIn("Update the server config", prompt)
        self.assertEqual(self.outputs["cost"], "0.0123")
        self.assertIn("Cost: 0.0123 USD", self.final_comment())
        # scratch XDG dirs are removed afterwards
        self.assertEqual(list((self.dir / "runner").iterdir()), [])


FAKE_CURL = r'''#!/bin/sh
# fake curl: -fsSL ... -o OUT URL ; serves files from $FAKE_RELEASE_DIR by basename
out=; url=
while [ $# -gt 0 ]; do
  case "$1" in -o) out=$2; shift 2;; --retry|--retry-delay) shift 2;; -*) shift;; *) url=$1; shift;; esac
done
echo "$url" >> "$FAKE_CURL_LOG"
src="$FAKE_RELEASE_DIR/$(basename "$url")"
[ -f "$src" ] || exit 22
cp "$src" "$out"
'''


@unittest.skipUnless(sys.platform.startswith("linux") and DESK.exists(), "linux + build/desk required")
class InstallScript(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp(prefix="desk-action-install-"))
        self.addCleanup(shutil.rmtree, self.dir, True)
        rel = self.dir / "release"
        pkg = rel / "desk-agent-linux-x64"
        pkg.mkdir(parents=True)
        shutil.copy(DESK, pkg / "desk")
        shutil.copy(REPO_ROOT / "linux" / "install.sh", pkg / "install.sh")
        subprocess.run(["tar", "-C", str(rel), "-czf", str(rel / "desk-agent-linux-x64.tar.gz"),
                        "desk-agent-linux-x64"], check=True)
        shutil.rmtree(pkg)
        digest = subprocess.run(["sha256sum", "desk-agent-linux-x64.tar.gz"], cwd=rel, check=True,
                                capture_output=True, text=True).stdout
        (rel / "SHA256SUMS.txt").write_text("0" * 64 + "  desk-agent-windows-x64.zip\n" + digest)
        (self.dir / "bin").mkdir()
        curl = self.dir / "bin" / "curl"
        curl.write_text(FAKE_CURL)
        curl.chmod(0o755)
        self.env = {**os.environ, "PATH": f"{self.dir / 'bin'}:{os.environ['PATH']}",
                    "FAKE_RELEASE_DIR": str(rel), "FAKE_CURL_LOG": str(self.dir / "curl.log"),
                    "RUNNER_OS": "Linux", "RUNNER_ARCH": "X64", "HOME": str(self.dir)}

    def install(self, version):
        return subprocess.run(["bash", str(ACTION / "scripts" / "install_desk.sh"), version,
                               str(self.dir / "dest")], env=self.env, capture_output=True, text=True)

    def test_install_tag_and_latest(self):
        proc = self.install("1.2.3")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        path = proc.stdout.strip().splitlines()[-1]
        self.assertEqual(path, str(self.dir / "dest" / "prefix" / "bin" / "desk"))
        self.assertTrue(os.access(path, os.X_OK))
        self.assertIn("Checksum OK", proc.stderr)
        urls = (self.dir / "curl.log").read_text()
        self.assertIn("https://github.com/Milorgs/DeskCode/releases/download/v1.2.3/desk-agent-linux-x64.tar.gz", urls)
        shutil.rmtree(self.dir / "dest")
        proc = self.install("latest")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("releases/latest/download/desk-agent-linux-x64.tar.gz",
                      (self.dir / "curl.log").read_text())

    def test_checksum_mismatch_and_unsupported_arch(self):
        sums = Path(self.env["FAKE_RELEASE_DIR"]) / "SHA256SUMS.txt"
        sums.write_text("f" * 64 + "  desk-agent-linux-x64.tar.gz\n")
        proc = self.install("v9.9.9")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("Checksum mismatch", proc.stderr)
        self.env["RUNNER_ARCH"] = "ARM64"
        proc = self.install("latest")
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("only x64", proc.stderr)


class StaticChecks(unittest.TestCase):
    def test_every_input_is_forwarded(self):
        text = (ACTION / "action.yml").read_text()
        block = text.split("\ninputs:\n", 1)[1].split("\noutputs:\n", 1)[0]
        names = re.findall(r"^  (\w+):\s*$", block, re.M)
        self.assertEqual(len(names), 13)
        for name in names:
            self.assertIn(f"INPUT_{name.upper()}: ${{{{ inputs.{name} }}}}", text)

    def test_trigger_matching(self):
        sys.path.insert(0, str(ACTION / "scripts"))
        import desk_action as da
        self.assertEqual(da.find_trigger("hey @desk fix it", "@desk"), "fix it")
        self.assertEqual(da.find_trigger("@DESK  do\nthis", "@desk"), "do\nthis")
        self.assertIsNone(da.find_trigger("@desktop app", "@desk"))
        self.assertIsNone(da.find_trigger("mail me@desk", "@desk"))
        self.assertEqual(da.find_trigger("(@desk) x", "@desk"), ") x")


if __name__ == "__main__":
    unittest.main(verbosity=2)
