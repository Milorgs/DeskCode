"""Integration test for DeskServeClient against real ``./build/desk serve``."""
from __future__ import annotations

import json
import os
import subprocess
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from deskcode import DeskError, DeskServeClient

REPO_ROOT = Path(__file__).resolve().parents[3]
DESK_BIN = REPO_ROOT / "build" / "desk"


class _FakeModelHandler(BaseHTTPRequestHandler):
    def log_message(self, *args: object) -> None:
        pass

    def do_POST(self) -> None:
        length = int(self.headers.get("Content-Length") or 0)
        _ = self.rfile.read(length)
        payload = json.dumps({
            "choices": [{
                "index": 0,
                "message": {"role": "assistant", "content": "SDK_SERVE_HELLO"},
                "finish_reason": "stop",
            }],
            "usage": {"prompt_tokens": 12, "completion_tokens": 4, "total_tokens": 16},
        }).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)


@unittest.skipUnless(DESK_BIN.is_file() and os.access(DESK_BIN, os.X_OK), "build/desk not built")
class TestDeskServeClient(unittest.TestCase):
    def test_shared_session_over_http_and_sse(self) -> None:
        model_srv = ThreadingHTTPServer(("127.0.0.1", 0), _FakeModelHandler)
        model_thr = threading.Thread(target=model_srv.serve_forever, daemon=True)
        model_thr.start()
        try:
            with tempfile.TemporaryDirectory(prefix="desk-sdk-serve-") as tmp:
                tmp_path = Path(tmp)
                port_file = tmp_path / "serve.port"
                env = {k: v for k, v in os.environ.items() if not k.startswith("DESK_")}
                env.update(
                    XDG_CONFIG_HOME=str(tmp_path / "cfg"),
                    XDG_STATE_HOME=str(tmp_path / "state"),
                    DESK_NO_LSP="1",
                    DESK_NO_USER_SKILLS="1",
                    DESK_API_TYPE="openai",
                    DESK_BASE_URL=f"http://127.0.0.1:{model_srv.server_port}/v1",
                    DESK_MODEL="fake-model",
                    DESK_API_KEY="fake-sdk-key",
                )
                proc = subprocess.Popen(
                    [
                        str(DESK_BIN),
                        "--workspace", str(tmp_path),
                        "serve", "--port", "0", "--token", "sdk-tok",
                        "--port-file", str(port_file),
                    ],
                    env=env,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
                try:
                    deadline = time.monotonic() + 8.0
                    port = 0
                    while time.monotonic() < deadline and port <= 0:
                        if port_file.is_file():
                            txt = port_file.read_text(encoding="utf-8").strip()
                            if txt.isdigit():
                                port = int(txt)
                        if port <= 0:
                            time.sleep(0.03)
                    self.assertGreater(port, 0)

                    unauth = DeskServeClient(f"http://127.0.0.1:{port}")
                    with self.assertRaises(DeskError):
                        unauth.list_sessions()

                    client = DeskServeClient(f"http://127.0.0.1:{port}", token="sdk-tok")
                    health = client.health()
                    self.assertTrue(health.get("ok"))

                    created = client.create_session(mode="full", no_net=True)
                    sid = created["id"]
                    self.assertTrue(created.get("no_net"))

                    events = []
                    for ev_name, data in client.stream_events(
                        sid,
                        on_ready=lambda: client.send_prompt(sid, "Say hello from SDK"),
                    ):
                        events.append((ev_name, data))
                        if ev_name == "turn_done":
                            break

                    names = [e[0] for e in events]
                    self.assertIn("snapshot", names)
                    self.assertIn("turn_done", names)
                    snap = client.get_session(sid)
                    texts = [m["text"] for m in snap.get("transcript", [])]
                    self.assertIn("SDK_SERVE_HELLO", texts)
                finally:
                    proc.terminate()
                    proc.wait(timeout=5)
        finally:
            model_srv.shutdown()
            model_srv.server_close()


if __name__ == "__main__":
    unittest.main()
