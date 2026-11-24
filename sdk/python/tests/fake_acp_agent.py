#!/usr/bin/env python3
"""Scripted fake ACP agent used by the SDK tests (pure stdlib, no network).

Invoked as ``fake_acp_agent.py acp``.  Behaviour is driven by the prompt text:

  hello            two agent_message_chunk updates, end_turn
  tools            plan + tool_call + tool_call_update (in_progress / completed with text+diff)
  permission:KIND  tool_call of KIND, session/request_permission round trip, reports the outcome
  wait             one chunk, then waits for session/cancel -> stopReason "cancelled"
  unknown          sends an unknown agent->client request, reports the error code it got back
  mode             current_mode_update to "plan"
  crash            writes to stderr and exits with code 3 mid-turn
  echo             replies with the JSON of the received prompt blocks
  refuse           stopReason "refusal"

Environment: FAKE_ACP_LOG=<file> appends every received message (one JSON per line);
FAKE_ACP_EXIT_ON_INIT=1 exits with code 7 before answering initialize.
"""
import json
import os
import sys

LOG = os.environ.get("FAKE_ACP_LOG")
MODES = [{"id": m, "name": m.title(), "description": f"{m} mode"} for m in ("ask", "edit", "full", "plan")]
backlog = []
next_id = 1000
sessions = {}


def send(obj):
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


def read():
    if backlog:
        return backlog.pop(0)
    line = sys.stdin.readline()
    if not line:
        sys.exit(0)
    msg = json.loads(line)
    if LOG:
        with open(LOG, "a") as handle:
            handle.write(json.dumps(msg) + "\n")
    return msg


def wait_for(pred):
    """Read until pred(msg) is true; messages that do not match are kept for later."""
    skipped = []
    while True:
        msg = read()
        if pred(msg):
            backlog[:0] = skipped
            return msg
        skipped.append(msg)


def update(sid, upd):
    send({"jsonrpc": "2.0", "method": "session/update", "params": {"sessionId": sid, "update": upd}})


def chunk(sid, text):
    update(sid, {"sessionUpdate": "agent_message_chunk", "content": {"type": "text", "text": text}})


def agent_request(method, params):
    global next_id
    next_id += 1
    rid = next_id
    send({"jsonrpc": "2.0", "id": rid, "method": method, "params": params})
    return wait_for(lambda m: m.get("id") == rid and "method" not in m)


def handle_prompt(msg):
    params = msg["params"]
    sid = params["sessionId"]
    text = " ".join(b.get("text", "") for b in params["prompt"] if b.get("type") == "text")
    stop = "end_turn"
    cwd = sessions.get(sid, "/")
    if text.startswith("hello"):
        chunk(sid, "Hello")
        chunk(sid, ", world")
    elif text.startswith("tools"):
        update(sid, {"sessionUpdate": "plan", "entries": [
            {"content": "Read file", "priority": "medium", "status": "in_progress"},
            {"content": "Edit file", "priority": "medium", "status": "pending"}]})
        path = os.path.join(cwd, "a.txt")
        update(sid, {"sessionUpdate": "tool_call", "toolCallId": "t1", "title": "Read a.txt", "kind": "read",
                     "status": "pending", "rawInput": {"path": path}, "locations": [{"path": path, "line": 3}]})
        update(sid, {"sessionUpdate": "tool_call_update", "toolCallId": "t1", "status": "in_progress"})
        update(sid, {"sessionUpdate": "tool_call_update", "toolCallId": "t1", "status": "completed",
                     "content": [{"type": "content", "content": {"type": "text", "text": "file body"}},
                                 {"type": "diff", "path": path, "oldText": None, "newText": "new"}],
                     "rawOutput": {"bytes": 9}})
        update(sid, {"sessionUpdate": "brand_new_kind", "x": 1})
        chunk(sid, "Done.")
    elif text.startswith("permission:"):
        kind = text.split(":", 1)[1].strip() or "edit"
        call = {"toolCallId": "p1", "title": f"{kind} something", "kind": kind, "status": "pending",
                "rawInput": {"command": "rm -rf build"}}
        update(sid, dict(call, sessionUpdate="tool_call"))
        resp = agent_request("session/request_permission", {
            "sessionId": sid, "toolCall": call,
            "options": [{"optionId": "allow", "name": "Allow", "kind": "allow_once"},
                        {"optionId": "allow_always", "name": "Always allow", "kind": "allow_always"},
                        {"optionId": "reject", "name": "Reject", "kind": "reject_once"}]})
        outcome = resp.get("result", {}).get("outcome", {})
        label = outcome.get("optionId") if outcome.get("outcome") == "selected" else outcome.get("outcome")
        status = "completed" if label in ("allow", "allow_always") else "failed"
        update(sid, {"sessionUpdate": "tool_call_update", "toolCallId": "p1", "status": status})
        chunk(sid, f"outcome={label}")
    elif text.startswith("wait"):
        chunk(sid, "working")
        wait_for(lambda m: m.get("method") == "session/cancel" and m["params"].get("sessionId") == sid)
        stop = "cancelled"
    elif text.startswith("unknown"):
        resp = agent_request("fs/frobnicate", {"sessionId": sid})
        chunk(sid, f"error={resp.get('error', {}).get('code')}")
    elif text.startswith("mode"):
        update(sid, {"sessionUpdate": "current_mode_update", "currentModeId": "plan"})
    elif text.startswith("crash"):
        chunk(sid, "about to crash")
        sys.stderr.write("fatal: simulated crash with key " + os.environ.get("DESK_API_KEY", "") + "\n")
        sys.stderr.flush()
        os._exit(3)
    elif text.startswith("echo"):
        chunk(sid, json.dumps(params["prompt"]))
    elif text.startswith("refuse"):
        stop = "refusal"
    else:
        chunk(sid, "unknown prompt")
    send({"jsonrpc": "2.0", "id": msg["id"], "result": {"stopReason": stop}})


def main():
    if sys.argv[1:] != ["acp"]:
        sys.stderr.write("usage: fake_acp_agent.py acp\n")
        return 2
    counter = 0
    while True:
        msg = read()
        method, rid = msg.get("method"), msg.get("id")
        if method == "initialize":
            if os.environ.get("FAKE_ACP_EXIT_ON_INIT"):
                sys.stderr.write("cannot initialize: model not configured\n")
                return 7
            send({"jsonrpc": "2.0", "id": rid, "result": {
                "protocolVersion": 1,
                "agentCapabilities": {"loadSession": False,
                                      "promptCapabilities": {"image": False, "audio": False, "embeddedContext": True},
                                      "mcpCapabilities": {"http": True, "sse": False}},
                "authMethods": [],
                "agentInfo": {"name": "deskcode", "title": "DeskCode", "version": "1.16.0"}}})
        elif method == "session/new":
            counter += 1
            sid = f"sess-{counter}"
            sessions[sid] = msg["params"]["cwd"]
            send({"jsonrpc": "2.0", "id": rid, "result": {
                "sessionId": sid, "modes": {"currentModeId": "ask", "availableModes": MODES}}})
        elif method == "session/set_mode":
            mode = msg["params"]["modeId"]
            if mode not in {m["id"] for m in MODES}:
                send({"jsonrpc": "2.0", "id": rid, "error": {"code": -32602, "message": f"unknown mode {mode}"}})
                continue
            update(msg["params"]["sessionId"], {"sessionUpdate": "current_mode_update", "currentModeId": mode})
            send({"jsonrpc": "2.0", "id": rid, "result": {}})
        elif method == "session/prompt":
            handle_prompt(msg)
        elif method == "session/cancel":
            pass  # nothing running
        elif rid is not None and method is not None:
            send({"jsonrpc": "2.0", "id": rid, "error": {"code": -32601, "message": "Method not found"}})


if __name__ == "__main__":
    sys.exit(main())
