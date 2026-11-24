"""End-to-end integration tests running the Python SDK against the real ``build/desk`` binary."""
import asyncio
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

from _support import SECRET, clean_env

import deskcode
from deskcode import AsyncDeskAgent, DeskAgent, TextChunk, ToolCall, ToolCallUpdate, TurnEnd, allow_all, reject_all

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[2]
DESK = REPO_ROOT / "build" / "desk"
SMOKE_MODEL = REPO_ROOT / "bench" / "smoke_model.py"
APP_INI = "[server]\nport = 8080\nlegacy_mode = true\ntimeout_seconds = 30\n\n[db]\npool_size = 5\n"


@unittest.skipUnless(DESK.exists() and SMOKE_MODEL.exists(), "build/desk and bench/smoke_model.py required")
class RealDeskIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = Path(tempfile.mkdtemp(prefix="desk-sdk-real-"))
        port_file = cls.root / "port"
        cls.model = subprocess.Popen(
            [sys.executable, "-I", str(SMOKE_MODEL), str(port_file), SECRET],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
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
        self.case = Path(tempfile.mkdtemp(prefix="case-", dir=self.root))
        for sub in ("home", "cfg", "state", "ws"):
            (self.case / sub).mkdir()
        self.ws = self.case / "ws"
        (self.ws / "config").mkdir()
        (self.ws / "config" / "app.ini").write_text(APP_INI, encoding="utf-8")
        self.env = clean_env(
            HOME=str(self.case / "home"),
            XDG_CONFIG_HOME=str(self.case / "cfg"),
            XDG_STATE_HOME=str(self.case / "state"),
            DESK_NO_LSP="1",
            DESK_NO_USER_SKILLS="1",
        )

    def test_one_shot_run_edits_workspace(self):
        res = deskcode.run(
            "Please update config/app.ini",
            cwd=self.ws,
            approval="full",
            model="smoke-model",
            base_url=self.base_url,
            api_key=SECRET,
            api_type="openai",
            desk_path=[str(DESK), "--no-sandbox"],
            env=self.env,
            timeout=30,
        )
        expected_ver = subprocess.check_output([str(DESK), "--version"], text=True).strip()
        self.assertTrue(res.ok, res.error or res.stderr)
        self.assertEqual(res.version, expected_ver)
        self.assertIn("Updated config/app.ini", res.reply)
        self.assertEqual(res.tokens, 1920)
        self.assertEqual(res.usage.cache_read, 600)
        self.assertTrue(any(t.name == "run_shell" and t.ok for t in res.tools))
        self.assertIn("port = 8443", (self.ws / "config" / "app.ini").read_text(encoding="utf-8"))

    def test_real_desk_acp_permission_and_streaming(self):
        acp_env = dict(
            self.env,
            DESK_MODEL="smoke-model",
            DESK_BASE_URL=self.base_url,
            DESK_API_KEY=SECRET,
            DESK_API_TYPE="openai",
        )
        seen_permissions = []

        def record_and_allow(req):
            seen_permissions.append(req)
            return allow_all(req)

        expected_ver = subprocess.check_output([str(DESK), "--version"], text=True).strip()
        with DeskAgent(desk_path=[str(DESK), "--no-sandbox"], env=acp_env, cwd=self.ws,
                       on_permission=record_and_allow) as agent:
            self.assertEqual(agent.protocol_version, 1)
            self.assertEqual(agent.agent_info.get("name"), "deskcode")
            self.assertEqual(agent.agent_info.get("version"), expected_ver)
            session = agent.new_session(cwd=self.ws, mode="ask")
            self.assertEqual([m.id for m in session.available_modes], ["ask", "edit", "full", "plan"])

            # First try rejecting permission via session policy
            denied_turn = session.send("Please update config/app.ini")
            session_with_reject = agent.new_session(cwd=self.ws, mode="ask", on_permission=reject_all)
            (self.ws / "config" / "app.ini").write_text(APP_INI, encoding="utf-8")
            rejected = session_with_reject.send("Please update config/app.ini")
            self.assertTrue(any(tc.status == "failed" for tc in rejected.tool_calls))
            self.assertIn("port = 8080", (self.ws / "config" / "app.ini").read_text(encoding="utf-8"))

            # Verify the allowed turn streamed chunks, asked permission with kind='execute', and updated app.ini
            self.assertEqual(denied_turn.stop_reason, "end_turn")
            self.assertIn("Updated config/app.ini", denied_turn.text)
            self.assertEqual(len(seen_permissions), 1)
            self.assertEqual(seen_permissions[0].tool_call.kind, "execute")
            self.assertIn("command", seen_permissions[0].tool_call.raw_input)
            self.assertTrue(any(isinstance(e, TextChunk) for e in denied_turn.events))
            self.assertTrue(any(isinstance(e, ToolCall) for e in denied_turn.events))
            self.assertTrue(any(isinstance(e, ToolCallUpdate) for e in denied_turn.events))
            self.assertEqual(denied_turn.events[-1], TurnEnd("end_turn"))

        # Re-run allowed on fresh app.ini via AsyncDeskAgent
        (self.ws / "config" / "app.ini").write_text(APP_INI, encoding="utf-8")

        async def run_async():
            async with AsyncDeskAgent(desk_path=[str(DESK), "--no-sandbox"], env=acp_env,
                                      cwd=self.ws, on_permission=allow_all) as agent:
                s = await agent.new_session(cwd=self.ws, mode="full")
                return await s.send("Please update config/app.ini")

        turn = asyncio.run(run_async())
        self.assertEqual(turn.stop_reason, "end_turn")
        self.assertIn("port = 8443", (self.ws / "config" / "app.ini").read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
