#!/usr/bin/env python3
"""Scripted OpenAI-compatible fake model for the bench smoke test (offline, no real keys).

Behaviour (decided from the conversation, never from the network):
- If a user message mentions ``config/app.ini`` and no tool result has arrived yet, it
  replies with one ``run_shell`` tool call that applies the correct edit, then a short final
  answer once the tool result comes back. -> the ``config-ini-update`` check should PASS.
- For any other task it answers with plain text and does nothing. -> checks should FAIL.
- Requests whose ``Authorization`` header does not carry the expected bearer token get 401,
  which proves the runner forwards ``DESK_API_KEY`` to desk.

Usage: python3 -I bench/smoke_model.py <port-file> <expected-api-key>
The chosen port is written to <port-file> once the server listens on 127.0.0.1.
"""
import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

EXPECTED_KEY = ""

CONFIG_FIX = r"""python3 - <<'PY'
import pathlib
p = pathlib.Path("config/app.ini")
s = p.read_text()
s = s.replace("port = 8080\n", "port = 8443\n", 1).replace("legacy_mode = true\n", "", 1)
s = s.replace("timeout_seconds = 30\n", "timeout_seconds = 30\ntls_cert = /etc/app/tls/cert.pem\ntls_key = /etc/app/tls/key.pem\n", 1)
s = s.replace("pool_size = 5\n", "pool_size = 20\n", 1)
p.write_text(s.rstrip("\n") + "\n\n[logging]\nlevel = debug\nfile = /var/log/app/app.log\n")
print("updated")
PY"""

USAGE = {"prompt_tokens": 900, "completion_tokens": 60, "total_tokens": 960,
         "prompt_tokens_details": {"cached_tokens": 300}}


def text_of(content):
    if isinstance(content, list):
        return " ".join(part.get("text", "") for part in content if isinstance(part, dict))
    return content or ""


def decide(messages):
    users = " ".join(text_of(m.get("content")) for m in messages if m.get("role") == "user")
    answered = any(m.get("role") == "tool" for m in messages)
    if "config/app.ini" in users and not answered:
        call = {"id": "call_smoke_1", "type": "function",
                "function": {"name": "run_shell", "arguments": json.dumps({"command": CONFIG_FIX})}}
        return None, call
    if answered:
        return "Updated config/app.ini as requested.", None
    return "Smoke-test model: I cannot solve this task.", None


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def _json(self, status, payload):
        body = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _authorised(self):
        header = self.headers.get("Authorization", "")
        return header == f"Bearer {EXPECTED_KEY}"

    def do_GET(self):
        if not self._authorised():
            return self._json(401, {"error": {"message": "bad key"}})
        self._json(200, {"data": [{"id": "smoke-model"}]})

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        request = json.loads(self.rfile.read(length) or b"{}")
        if not self._authorised():
            return self._json(401, {"error": {"message": "invalid api key", "type": "auth"}})
        text, call = decide(request.get("messages") or [])
        if not request.get("stream"):
            message = {"role": "assistant", "content": text}
            if call:
                message["tool_calls"] = [call]
            return self._json(200, {"choices": [{"index": 0, "message": message,
                                                 "finish_reason": "tool_calls" if call else "stop"}],
                                    "usage": USAGE})
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()

        def send(obj):
            self.wfile.write(b"data: " + json.dumps(obj).encode() + b"\n\n")
            self.wfile.flush()

        if call:
            send({"choices": [{"index": 0, "delta": {"role": "assistant", "tool_calls": [dict(call, index=0)]}}]})
            send({"choices": [{"index": 0, "delta": {}, "finish_reason": "tool_calls"}], "usage": USAGE})
        else:
            for i in range(0, len(text), 16):
                send({"choices": [{"index": 0, "delta": {"content": text[i:i + 16]}}]})
            send({"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}], "usage": USAGE})
        self.wfile.write(b"data: [DONE]\n\n")
        self.wfile.flush()


def main():
    global EXPECTED_KEY
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    EXPECTED_KEY = sys.argv[2]
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    with open(sys.argv[1], "w", encoding="utf-8") as handle:
        handle.write(str(server.server_address[1]))
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
