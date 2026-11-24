#!/usr/bin/env bash
# Hidden verifier for cli-add-options: compares wordfreq.py against an embedded oracle on
# inputs the agent never sees.
set -u
export PYTHONDONTWRITEBYTECODE=1
ws=$PWD
[ -f "$ws/wordfreq.py" ] || { echo "wordfreq.py missing" >&2; exit 1; }
grep -q -- '--top' "$ws/README.md" && grep -q -- '--ignore-case' "$ws/README.md" \
    || { echo "README.md does not document --top and --ignore-case" >&2; exit 1; }
exec python3 - "$ws" <<'PY'
import os
import re
import subprocess
import sys
import tempfile
from collections import Counter

ws = sys.argv[1]
TEXT = (
    "The cat and the hat. THE end?\n"
    "Don't stop; don't STOP. 'quoted' words, and more words!\n"
    "zeta alpha beta alpha beta gamma 42 numbers_are-not words\n"
    "Apple apple APPLE banana Banana cherry\n"
)


def oracle(text, top=None, ignore_case=False):
    words = re.findall(r"[A-Za-z']+", text)
    if ignore_case:
        words = [w.lower() for w in words]
    counts = Counter(words)
    lines = [f"words: {sum(counts.values())}", f"unique: {len(counts)}"]
    if top is not None:
        ranked = sorted(counts.items(), key=lambda kv: (-kv[1], kv[0]))[:top]
        lines += [f"{w} {c}" for w, c in ranked]
    return "\n".join(lines) + "\n"


def run(args):
    return subprocess.run([sys.executable, os.path.join(ws, "wordfreq.py"), *args],
                          capture_output=True, text=True, timeout=30, cwd=tmp)


failures = []
with tempfile.TemporaryDirectory() as tmp:
    path = os.path.join(tmp, "input.txt")
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(TEXT)
    small = os.path.join(tmp, "small.txt")
    with open(small, "w", encoding="utf-8") as handle:
        handle.write("b a b\n")

    cases = [
        ([path], oracle(TEXT)),
        (["--ignore-case", path], oracle(TEXT, ignore_case=True)),
        (["--top", "3", path], oracle(TEXT, top=3)),
        ([path, "--top", "5", "--ignore-case"], oracle(TEXT, top=5, ignore_case=True)),
        (["--ignore-case", "--top=100", path], oracle(TEXT, top=100, ignore_case=True)),
        (["--top", "10", small], oracle("b a b\n", top=10)),
        (["--top", "1", small], "words: 3\nunique: 2\nb 2\n"),
    ]
    for args, expected in cases:
        result = run(args)
        if result.returncode != 0 or result.stdout != expected:
            failures.append(f"args={args!r} rc={result.returncode}\n--- expected\n{expected}--- got\n{result.stdout}{result.stderr}")

    for bad in ["0", "-1", "abc", "1.5"]:
        result = run(["--top", bad, path])
        if result.returncode != 2 or not result.stderr.strip():
            failures.append(f"--top {bad}: expected exit 2 with an error message, got rc={result.returncode}")

    missing = run([os.path.join(tmp, "nope.txt")])
    if missing.returncode == 0:
        failures.append("missing file should still fail")

if failures:
    print("\n\n".join(failures), file=sys.stderr)
    sys.exit(1)
print("ok")
PY
