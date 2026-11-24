#!/usr/bin/env bash
# Reference solution for git-feature-branch.
set -eu
git config user.name "Bench Solution"
git config user.email "bench@example.invalid"
git switch -c feature/polite-greeting
python3 - <<'PY'
import pathlib
g = pathlib.Path("greet.py")
g.write_text(g.read_text().replace('return "Hello " + name', 'return f"Hello, {name}!"'))
c = pathlib.Path("CHANGELOG.md")
c.write_text(c.read_text().replace("## Unreleased\n\n", "## Unreleased\n\n- Polite greeting punctuation\n", 1))
PY
git add greet.py CHANGELOG.md
git commit -q -m "feat: polite greeting"
git switch -q main
