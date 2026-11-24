import asyncio
import json
import os
import tempfile
import unittest
from unittest import mock

from _support import FAKE_DESK, SECRET, clean_env

import deskcode
from deskcode import DeskError, DeskTimeout, RunResult


class RunTests(unittest.TestCase):
    def setUp(self):
        patcher = mock.patch.dict(os.environ, clean_env(), clear=True)
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_completed_result_and_arguments(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = deskcode.run("fix the bug\nline two", cwd=tmp, approval="full", plan=True, max_steps=7,
                                  model="m1", base_url="http://x/v1", api_key=SECRET, api_type="anthropic",
                                  desk_path=FAKE_DESK, env={"EXTRA": "1"})
            self.assertIsInstance(result, RunResult)
            self.assertTrue(result.ok)
            self.assertEqual(result.exit_code, 0)
            echo = json.loads(result.reply)
            argv = echo["argv"]
            self.assertEqual(argv[:4], ["--json", "-p", "-", "--approval"])
            self.assertEqual(argv[4], "full")
            self.assertIn("--plan", argv)
            self.assertEqual(argv[argv.index("--max-steps") + 1], "7")
            self.assertEqual(argv[argv.index("--workspace") + 1], os.path.abspath(tmp))
            self.assertEqual(echo["prompt"], "fix the bug\nline two")
            self.assertEqual(os.path.realpath(echo["cwd"]), os.path.realpath(tmp))
            self.assertEqual(echo["env"], {"DESK_MODEL": "m1", "DESK_BASE_URL": "http://x/v1",
                                           "DESK_API_TYPE": "anthropic", "EXTRA": "1"})
            self.assertTrue(echo["has_key"])
        self.assertNotIn(SECRET, result.reply)
        self.assertEqual(result.tokens, 1234)
        self.assertEqual(result.usage.input, 1000)
        self.assertEqual(result.usage.cache_read, 34)
        self.assertAlmostEqual(result.usage.cost, 0.0042)
        self.assertEqual(result.usage.currency, "USD")
        self.assertEqual([t.name for t in result.tools], ["read_file", "run_shell"])
        self.assertFalse(result.tools[1].ok)
        self.assertEqual(result.tools[1].error, "exit 2")
        self.assertEqual(result.version, "1.16.0")
        self.assertIsNone(result.plan_submitted)
        self.assertIn("progress noise", result.stderr)

    def test_defaults(self):
        result = deskcode.run("hi", desk_path=FAKE_DESK)
        argv = json.loads(result.reply)["argv"]
        self.assertEqual(argv, ["--json", "-p", "-", "--approval", "edit"])

    def test_nonzero_exit_with_json_returns_result(self):
        result = deskcode.run("x", desk_path=FAKE_DESK, env={"FAKE_DESK_MODE": "fail"})
        self.assertFalse(result.ok)
        self.assertEqual(result.status, "step_limit")
        self.assertEqual(result.exit_code, 1)
        self.assertEqual(result.error, "step limit reached")
        self.assertIs(result.plan_submitted, False)

    def test_no_json_raises_redacted_error(self):
        with self.assertRaises(DeskError) as ctx:
            deskcode.run("x", desk_path=FAKE_DESK, api_key=SECRET, env={"FAKE_DESK_MODE": "nojson"})
        err = ctx.exception
        self.assertEqual(err.returncode, 2)
        self.assertIn("authentication failed for key ***", str(err))
        self.assertNotIn(SECRET, str(err))
        self.assertNotIn(SECRET, err.stderr)

    def test_key_from_env_is_redacted_too(self):
        with self.assertRaises(DeskError) as ctx:
            deskcode.run("x", desk_path=FAKE_DESK, env={"FAKE_DESK_MODE": "nojson", "DESK_API_KEY": SECRET})
        self.assertNotIn(SECRET, str(ctx.exception))

    def test_missing_executable(self):
        with self.assertRaises(DeskError):
            deskcode.run("x", desk_path="/nonexistent/desk-binary")

    def test_timeout(self):
        with self.assertRaises(DeskTimeout):
            deskcode.run("x", desk_path=FAKE_DESK, env={"FAKE_DESK_MODE": "sleep"}, timeout=0.5)

    def test_invalid_approval(self):
        with self.assertRaises(ValueError):
            deskcode.run("x", approval="yolo", desk_path=FAKE_DESK)


class AsyncRunTests(unittest.TestCase):
    def setUp(self):
        patcher = mock.patch.dict(os.environ, clean_env(), clear=True)
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_arun(self):
        result = asyncio.run(deskcode.arun("hello", desk_path=FAKE_DESK, max_steps=3))
        self.assertTrue(result.ok)
        self.assertEqual(json.loads(result.reply)["prompt"], "hello")

    def test_arun_errors(self):
        with self.assertRaises(DeskError) as ctx:
            asyncio.run(deskcode.arun("x", desk_path=FAKE_DESK, api_key=SECRET, env={"FAKE_DESK_MODE": "nojson"}))
        self.assertNotIn(SECRET, str(ctx.exception))
        with self.assertRaises(DeskTimeout):
            asyncio.run(deskcode.arun("x", desk_path=FAKE_DESK, env={"FAKE_DESK_MODE": "sleep"}, timeout=0.5))


if __name__ == "__main__":
    unittest.main()
