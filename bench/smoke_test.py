#!/usr/bin/env python3
"""End-to-end smoke test of the bench pipeline with a scripted local fake model.

Starts ``bench/smoke_model.py`` on 127.0.0.1, runs ``bench/run_local.py`` against a few
tasks with fake ``DESK_*`` settings and isolated temporary XDG directories, then asserts:

- desk was invoked and its ``--json`` output parsed for every run;
- the scripted task (config-ini-update) passes its check and the others fail;
- the result file was written, contains no API key, and the Markdown summary printed;
- no ``desk-bench-*`` temporary directories are left behind.

Usage: python3 bench/smoke_test.py [--desk PATH] [--keep-results]
Exits 0 on success. Never touches ~/.config/desk-agent and never uses the network.
"""
import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

BENCH = Path(__file__).resolve().parent
FAKE_KEY = "smoke-fake-key-NOT-REAL-7f3a9c"
TASKS = ["config-ini-update", "py-fix-failing-tests", "git-feature-branch"]
EXPECT_PASS = {"config-ini-update"}


def leftovers(tmp_root):
    return set(glob.glob(os.path.join(tmp_root, "desk-bench-*")))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--desk", default=str(BENCH.parent / "build" / "desk"))
    parser.add_argument("--keep-results", action="store_true", help="copy the result JSON into bench/results/")
    args = parser.parse_args()

    tmp_root = tempfile.gettempdir()
    before = leftovers(tmp_root)
    work = Path(tempfile.mkdtemp(prefix="desk-smoke-"))
    port_file = work / "port"
    out_file = work / "results.json"
    server = subprocess.Popen([sys.executable, "-I", str(BENCH / "smoke_model.py"), str(port_file), FAKE_KEY],
                              stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    errors = []
    try:
        for _ in range(100):
            if port_file.exists() and port_file.read_text().strip():
                break
            time.sleep(0.05)
        else:
            print("smoke model did not start", file=sys.stderr)
            return 1
        port = port_file.read_text().strip()
        env = {k: v for k, v in os.environ.items() if not k.startswith(("DESK_", "XDG_"))}
        env.update({
            "DESK_BASE_URL": f"http://127.0.0.1:{port}/v1",
            "DESK_MODEL": "smoke-model",
            "DESK_API_KEY": FAKE_KEY,
            "DESK_API_TYPE": "openai",
        })
        cmd = [sys.executable, str(BENCH / "run_local.py"), "--desk", args.desk, "--tasks", ",".join(TASKS),
               "--jobs", "2", "--timeout", "120", "--label", "smoke", "--out", str(out_file)]
        proc = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=600, stdin=subprocess.DEVNULL)
        sys.stdout.write(proc.stdout)
        sys.stderr.write(proc.stderr)
        if proc.returncode != 0:
            errors.append(f"run_local.py exited {proc.returncode}")
        if FAKE_KEY in proc.stdout or FAKE_KEY in proc.stderr:
            errors.append("API key leaked to runner output")
        if "| task | pass |" not in proc.stdout:
            errors.append("Markdown summary table missing")
        if not out_file.is_file():
            errors.append("result file not written")
        else:
            raw = out_file.read_text(encoding="utf-8")
            if FAKE_KEY in raw:
                errors.append("API key leaked into the result file")
            report = json.loads(raw)
            seen = {r["task"]: r for r in report["results"]}
            for task in TASKS:
                r = seen.get(task)
                if r is None:
                    errors.append(f"{task}: no result")
                    continue
                if not r.get("json_ok"):
                    errors.append(f"{task}: desk JSON not parsed ({r.get('error') or r.get('desk_stdout_tail', '')[:200]})")
                if not r.get("tokens"):
                    errors.append(f"{task}: tokens missing")
                if r.get("passed") != (task in EXPECT_PASS):
                    errors.append(f"{task}: passed={r.get('passed')} (expected {task in EXPECT_PASS}); "
                                  f"status={r.get('status')} check={r.get('check', {}).get('output', '')[-300:]}")
            cfg = seen.get("config-ini-update", {})
            if cfg.get("tool_calls") != 1 or cfg.get("status") != "completed":
                errors.append(f"config-ini-update: expected 1 tool call and status completed, got "
                              f"{cfg.get('tool_calls')} / {cfg.get('status')}")
            if cfg.get("diff", {}).get("files") != 1:
                errors.append(f"config-ini-update: expected a one-file diff, got {cfg.get('diff')}")
            if args.keep_results:
                dest = BENCH / "results" / f"smoke-{time.strftime('%Y%m%dT%H%M%S')}.json"
                dest.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy(out_file, dest)
                print(f"kept result file: {dest}")
    finally:
        server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()
        shutil.rmtree(work, ignore_errors=True)

    left = leftovers(tmp_root) - before
    if left:
        errors.append(f"temporary directories left behind: {sorted(left)}")
    if errors:
        print("\nSMOKE FAILED:\n- " + "\n- ".join(errors), file=sys.stderr)
        return 1
    print("\nSMOKE OK: desk invoked, JSON parsed, checks discriminate pass/fail, results written, temp dirs cleaned.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
