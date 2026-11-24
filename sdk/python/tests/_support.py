"""Shared helpers for the SDK tests: makes ``src/`` importable and locates the fakes."""
import os
import sys
from pathlib import Path

sys.dont_write_bytecode = True
TESTS = Path(__file__).resolve().parent
SDK = TESTS.parent
REPO_ROOT = SDK.parent.parent
sys.path.insert(0, str(SDK / "src"))

FAKE_ACP = [sys.executable, str(TESTS / "fake_acp_agent.py")]
FAKE_DESK = [sys.executable, str(TESTS / "fake_desk_cli.py")]
SECRET = "sk-sdk-test-SECRET-4242abcd"


def clean_env(**extra):
    env = {k: v for k, v in os.environ.items() if not k.startswith(("DESK_", "FAKE_"))}
    env.update(extra)
    return env
