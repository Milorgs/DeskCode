#!/usr/bin/env python3
"""Fake ``desk --json`` CLI for run()/arun() tests.

Mode comes from FAKE_DESK_MODE: ok (default) | fail | nojson | sleep.
The reply echoes argv, stdin and selected DESK_* variables so tests can check them.
"""
import json
import os
import sys
import time

mode = os.environ.get("FAKE_DESK_MODE", "ok")
prompt = sys.stdin.read()
if mode == "sleep":
    time.sleep(30)
if mode == "nojson":
    sys.stderr.write("error: authentication failed for key " + os.environ.get("DESK_API_KEY", "") + "\n")
    print("plain text only")
    sys.exit(2)
echo = {"argv": sys.argv[1:], "prompt": prompt, "cwd": os.getcwd(),
        "env": {k: os.environ.get(k) for k in ("DESK_MODEL", "DESK_BASE_URL", "DESK_API_TYPE", "EXTRA")},
        "has_key": bool(os.environ.get("DESK_API_KEY"))}
result = {
    "status": "completed" if mode == "ok" else "step_limit",
    "reply": json.dumps(echo),
    "model": "fake", "protocol": "openai", "workspace": os.getcwd(), "tokens": 1234,
    "usage": {"input": 1000, "output": 200, "cache_read": 34, "cost": 0.0042, "currency": "USD"},
    "elapsed_seconds": 1.5,
    "tools": [{"name": "read_file", "target": "a.txt", "ok": True},
              {"name": "run_shell", "target": "make", "ok": False, "error": "exit 2"}],
    "version": "1.16.0",
}
if mode == "fail":
    result["error"] = "step limit reached"
    result["plan_submitted"] = False
sys.stderr.write("progress noise\n")
print("some banner")
print(json.dumps(result))
sys.exit(0 if mode == "ok" else 1)
