#!/usr/bin/env bash
# Hidden verifier for config-ini-update.
set -u
ws=$PWD
exec python3 - "$ws" <<'PY'
import configparser
import hashlib
import os
import sys

ws = sys.argv[1]
failures = []
path = os.path.join(ws, "config", "app.ini")
example = os.path.join(ws, "config", "app.example.ini")
if not os.path.exists(path):
    print("config/app.ini missing", file=sys.stderr)
    sys.exit(1)

EXAMPLE = (
    "# Example configuration -- copy to app.ini and adjust.\n\n"
    "[server]\nhost = 127.0.0.1\nport = 8080\nworkers = 1\nlegacy_mode = true\n\n"
    "[database]\nurl = postgres://app@localhost:5432/app\npool_size = 5\n"
)
try:
    with open(example, "rb") as handle:
        if hashlib.sha256(handle.read()).hexdigest() != hashlib.sha256(EXAMPLE.encode()).hexdigest():
            failures.append("config/app.example.ini was modified")
except OSError:
    failures.append("config/app.example.ini was removed")

text = open(path, encoding="utf-8").read()
parser = configparser.ConfigParser(interpolation=None)
try:
    parser.read_string(text)
except configparser.Error as exc:
    print(f"app.ini no longer parses: {exc}", file=sys.stderr)
    sys.exit(1)

expected = {
    "server": {"host": "0.0.0.0", "port": "8443", "workers": "4", "timeout_seconds": "30",
               "tls_cert": "/etc/app/tls/cert.pem", "tls_key": "/etc/app/tls/key.pem"},
    "database": {"url": "postgres://app@db.internal:5432/app", "pool_size": "20", "ssl": "require"},
    "features": {"beta_dashboard": "false", "search_v2": "true"},
    "logging": {"level": "debug", "file": "/var/log/app/app.log"},
}
if sorted(parser.sections()) != sorted(expected):
    failures.append(f"sections are {parser.sections()}, expected {sorted(expected)}")
for section, keys in expected.items():
    if not parser.has_section(section):
        continue
    actual = dict(parser.items(section))
    if actual != keys:
        failures.append(f"[{section}] is {actual}, expected {keys}")

for comment in ("# Application configuration (production)",
                "# managed-by: ops-team -- do not remove this header",
                "# workers should match the CPU count of the node",
                "; semicolon comments are used here too"):
    if comment not in text.splitlines():
        failures.append(f"comment line lost: {comment!r}")
if "legacy_mode" in text:
    failures.append("legacy_mode still present")

# Comments must stay in their sections.
lines = [l.strip() for l in text.splitlines()]
try:
    if not (lines.index("[server]") < lines.index("# workers should match the CPU count of the node") < lines.index("[database]")
            < lines.index("; semicolon comments are used here too") < lines.index("[features]")):
        failures.append("comments moved out of their sections")
except ValueError:
    pass

if failures:
    print("\n".join(failures), file=sys.stderr)
    sys.exit(1)
print("ok")
PY
