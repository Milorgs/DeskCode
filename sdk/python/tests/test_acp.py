import asyncio
import json
import os
import tempfile
import time
import unittest
from pathlib import Path

from _support import FAKE_ACP, SECRET, clean_env

import deskcode
from deskcode import (AcpError, AgentExited, AsyncDeskAgent, DeskAgent, DeskError, DiffContent, ModeChange,
                      PlanUpdate, TextChunk, TextContent, ToolCall, ToolCallUpdate, TurnEnd, UnknownUpdate,
                      allow_all, allow_reads_only, reject_all)


class AcpTestCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.log = Path(self.tmp.name) / "received.jsonl"
        self.env = clean_env(FAKE_ACP_LOG=str(self.log), DESK_API_KEY=SECRET)

    def agent(self, **kwargs):
        return DeskAgent(desk_path=FAKE_ACP, env=self.env, cwd=self.tmp.name, **kwargs)

    def received(self, method=None):
        msgs = [json.loads(l) for l in self.log.read_text().splitlines()]
        return [m for m in msgs if method is None or m.get("method") == method]


class InitAndSessionTests(AcpTestCase):
    def test_initialize_handshake(self):
        with self.agent() as agent:
            self.assertEqual(agent.protocol_version, 1)
            self.assertEqual(agent.agent_info["name"], "deskcode")
            self.assertEqual(agent.agent_info["version"], "1.16.0")
            self.assertTrue(agent.agent_capabilities["promptCapabilities"]["embeddedContext"])
            self.assertEqual(agent.auth_methods, [])
        init = self.received("initialize")[0]
        self.assertEqual(init["jsonrpc"], "2.0")
        self.assertEqual(init["params"]["protocolVersion"], 1)
        self.assertEqual(init["params"]["clientCapabilities"],
                         {"fs": {"readTextFile": False, "writeTextFile": False}, "terminal": False})
        self.assertEqual(init["params"]["clientInfo"], {"name": "deskcode-python", "version": deskcode.__version__})
        self.assertIsNotNone(agent.returncode)

    def test_new_session_and_modes(self):
        with self.agent() as agent:
            session = agent.new_session(cwd=self.tmp.name)
            self.assertEqual(session.session_id, "sess-1")
            self.assertEqual(session.current_mode, "ask")
            self.assertEqual([m.id for m in session.available_modes], ["ask", "edit", "full", "plan"])
            self.assertFalse(self.received("session/set_mode"))
            edit = agent.new_session(cwd=".", mode="edit")
            self.assertEqual(edit.current_mode, "edit")
            edit.set_mode("full")
            self.assertEqual(edit.current_mode, "full")
            with self.assertRaises(AcpError) as ctx:
                edit.set_mode("bogus")
            self.assertEqual(ctx.exception.code, -32602)
            # a mode change pushed by the agent outside a turn updates the session
            turn = edit.send("mode")
            self.assertEqual(edit.current_mode, "plan")
            self.assertIn(ModeChange("plan"), turn.events)
        new = self.received("session/new")
        self.assertEqual(new[0]["params"], {"cwd": os.path.abspath(self.tmp.name), "mcpServers": []})
        self.assertTrue(os.path.isabs(new[1]["params"]["cwd"]))
        modes = self.received("session/set_mode")
        self.assertEqual(modes[0]["params"], {"sessionId": "sess-2", "modeId": "edit"})


class StreamingTests(AcpTestCase):
    def test_text_stream(self):
        with self.agent() as agent:
            session = agent.new_session()
            events = list(session.prompt("hello"))
        self.assertEqual(events, [TextChunk("Hello", events[0].raw), TextChunk(", world", events[1].raw),
                                  TurnEnd("end_turn")])

    def test_tool_events_and_send(self):
        with self.agent() as agent:
            session = agent.new_session()
            events = list(session.prompt("tools"))
            kinds = [type(e) for e in events]
            self.assertEqual(kinds, [PlanUpdate, ToolCall, ToolCallUpdate, ToolCallUpdate, UnknownUpdate,
                                     TextChunk, TurnEnd])
            plan, call, upd1, upd2 = events[:4]
            self.assertEqual([(e.content, e.status) for e in plan.entries],
                             [("Read file", "in_progress"), ("Edit file", "pending")])
            self.assertEqual(call.tool_call_id, "t1")
            self.assertEqual(call.kind, "read")
            self.assertEqual(call.status, "pending")
            self.assertEqual(call.locations[0].line, 3)
            self.assertTrue(call.raw_input["path"].endswith("a.txt"))
            self.assertEqual(upd1.status, "in_progress")
            self.assertEqual(upd2.status, "completed")
            self.assertEqual(upd2.content[0], TextContent("file body"))
            self.assertEqual(upd2.text, "file body")
            self.assertIsInstance(upd2.content[1], DiffContent)
            self.assertIsNone(upd2.diffs[0].old_text)
            self.assertEqual(upd2.diffs[0].new_text, "new")
            self.assertEqual(upd2.raw_output, {"bytes": 9})
            self.assertEqual(events[4].kind, "brand_new_kind")

            turn = session.send("tools")
            self.assertEqual(turn.text, "Done.")
            self.assertEqual(turn.stop_reason, "end_turn")
            self.assertEqual(len(turn.tool_calls), 1)
            self.assertEqual(turn.tool_calls[0].status, "completed")
            self.assertEqual(turn.tool_calls[0].title, "Read a.txt")
            self.assertEqual(len(turn.plan), 2)

    def test_prompt_blocks_and_resources(self):
        file = Path(self.tmp.name) / "notes.md"
        file.write_text("x")
        with self.agent() as agent:
            session = agent.new_session()
            turn = session.send("echo", resources=[str(file), deskcode.embedded_resource("file:///m.txt", "body")])
        blocks = json.loads(turn.text)
        self.assertEqual(blocks[0], {"type": "text", "text": "echo"})
        self.assertEqual(blocks[1], {"type": "resource_link", "uri": file.resolve().as_uri(), "name": "notes.md"})
        self.assertEqual(blocks[2], {"type": "resource", "resource": {"uri": "file:///m.txt", "text": "body",
                                                                      "mimeType": "text/plain"}})

    def test_other_stop_reasons(self):
        with self.agent() as agent:
            self.assertEqual(agent.new_session().send("refuse").stop_reason, "refusal")


class PermissionTests(AcpTestCase):
    def ask(self, policy, kind="edit", session_policy=None):
        seen = []

        def wrapped(request):
            seen.append(request)
            return policy(request)

        with self.agent(on_permission=wrapped if session_policy is None else None) as agent:
            session = agent.new_session(on_permission=wrapped if session_policy else None)
            turn = session.send(f"permission:{kind}")
        return turn, seen

    def responses(self):
        return [m for m in self.received() if "method" not in m and m.get("id", 0) > 1000]

    def test_default_policy_rejects(self):
        with self.agent() as agent:
            turn = agent.new_session().send("permission:execute")
        self.assertIn("outcome=reject", turn.text)
        self.assertEqual(turn.tool_calls[0].status, "failed")
        self.assertEqual(self.responses()[0]["result"], {"outcome": {"outcome": "selected", "optionId": "reject"}})

    def test_allow_all(self):
        turn, seen = self.ask(allow_all, "execute")
        self.assertIn("outcome=allow", turn.text)
        self.assertEqual(turn.tool_calls[0].status, "completed")
        request = seen[0]
        self.assertEqual(request.session_id, "sess-1")
        self.assertEqual(request.tool_call.kind, "execute")
        self.assertEqual(request.tool_call.raw_input, {"command": "rm -rf build"})
        self.assertEqual([o.kind for o in request.options], ["allow_once", "allow_always", "reject_once"])
        self.assertEqual(self.responses()[0]["result"], {"outcome": {"outcome": "selected", "optionId": "allow"}})

    def test_reject_all(self):
        turn, _ = self.ask(reject_all, "read")
        self.assertIn("outcome=reject", turn.text)

    def test_allow_reads_only(self):
        turn, _ = self.ask(allow_reads_only, "read", session_policy=True)
        self.assertIn("outcome=allow", turn.text)
        turn, _ = self.ask(allow_reads_only, "edit")
        self.assertIn("outcome=reject", turn.text)

    def test_cancel_outcome(self):
        turn, _ = self.ask(lambda r: None)
        self.assertIn("outcome=cancelled", turn.text)
        self.assertEqual(self.responses()[0]["result"], {"outcome": {"outcome": "cancelled"}})
        with self.assertLogs("deskcode", "WARNING") as logs:
            turn, _ = self.ask(lambda r: "not-an-option")
        self.assertIn("outcome=cancelled", turn.text)
        self.assertIn("unknown optionId", logs.output[0])

        def boom(request):
            raise RuntimeError("handler bug")
        with self.assertLogs("deskcode", "ERROR"):
            turn, _ = self.ask(boom)
        self.assertIn("outcome=cancelled", turn.text)


class CancelAndErrorTests(AcpTestCase):
    def test_cancel(self):
        with self.agent() as agent:
            session = agent.new_session()
            events = []
            for event in session.prompt("wait"):
                events.append(event)
                if isinstance(event, TextChunk):
                    session.cancel()
            self.assertEqual(events[-1], TurnEnd("cancelled"))
            # the session keeps working afterwards
            self.assertEqual(session.send("hello").text, "Hello, world")
        cancel = self.received("session/cancel")
        self.assertEqual(cancel[0]["params"], {"sessionId": "sess-1"})
        self.assertNotIn("id", cancel[0])

    def test_abandoned_iterator_is_cancelled_and_drained(self):
        with self.agent() as agent:
            session = agent.new_session()
            stream = session.prompt("wait")
            self.assertEqual(next(stream).text, "working")
            stream.close()  # stop early -> SDK sends session/cancel
            self.assertEqual(session.send("hello").text, "Hello, world")

    def test_concurrent_prompt_rejected(self):
        with self.agent() as agent:
            session = agent.new_session()
            stream = session.prompt("wait")
            next(stream)
            with self.assertRaises(DeskError):
                next(session.prompt("hello"))
            session.cancel()
            self.assertEqual(list(stream)[-1], TurnEnd("cancelled"))

    def test_unknown_agent_request_gets_method_not_found(self):
        with self.agent() as agent:
            turn = agent.new_session().send("unknown")
        self.assertEqual(turn.text, "error=-32601")
        reply = [m for m in self.received() if m.get("id") == 1001][0]
        self.assertEqual(reply["error"]["code"], -32601)

    def test_crash_mid_turn(self):
        with self.agent() as agent:
            session = agent.new_session()
            seen = []
            with self.assertRaises(AgentExited) as ctx:
                for event in session.prompt("crash"):
                    seen.append(event)
            self.assertEqual(seen[0].text, "about to crash")
            err = ctx.exception
            self.assertEqual(err.returncode, 3)
            self.assertIn("simulated crash", str(err))
            self.assertNotIn(SECRET, str(err))
            self.assertNotIn(SECRET, err.stderr)
            self.assertNotIn(SECRET, agent.stderr_text)
            with self.assertRaises(AgentExited):
                session.send("hello")
            with self.assertRaises(AgentExited):
                agent.new_session()

    def test_exit_during_initialize(self):
        self.env["FAKE_ACP_EXIT_ON_INIT"] = "1"
        with self.assertRaises(AgentExited) as ctx:
            with self.agent():
                pass
        self.assertEqual(ctx.exception.returncode, 7)
        self.assertIn("model not configured", str(ctx.exception))

    def test_missing_executable(self):
        with self.assertRaises(DeskError):
            DeskAgent(desk_path="/nonexistent/desk").start()

    def test_close_is_quick_and_idempotent(self):
        agent = self.agent().start()
        start = time.monotonic()
        agent.close()
        agent.close()
        self.assertLess(time.monotonic() - start, 5)


class AsyncAcpTests(AcpTestCase):
    def test_async_stream_and_send(self):
        async def main():
            async with AsyncDeskAgent(desk_path=FAKE_ACP, env=self.env, on_permission=allow_all) as agent:
                self.assertEqual(agent.agent_info["title"], "DeskCode")
                session = await agent.new_session(cwd=self.tmp.name, mode="edit")
                self.assertEqual(session.current_mode, "edit")
                events = [e async for e in session.prompt("hello")]
                turn = await session.send("permission:execute")
                waiting = []
                async for event in session.prompt("wait"):
                    waiting.append(event)
                    if isinstance(event, TextChunk):
                        await session.cancel()
                return events, turn, waiting

        events, turn, waiting = asyncio.run(main())
        self.assertEqual([type(e) for e in events], [TextChunk, TextChunk, TurnEnd])
        self.assertIn("outcome=allow", turn.text)
        self.assertEqual(waiting[-1], TurnEnd("cancelled"))


if __name__ == "__main__":
    unittest.main()
