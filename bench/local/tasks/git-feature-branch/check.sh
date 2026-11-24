#!/usr/bin/env bash
# Hidden verifier for git-feature-branch. The runner exports BENCH_BASE_COMMIT (the commit
# it created before the agent started); fall back to the root commit otherwise.
set -u
export PYTHONDONTWRITEBYTECODE=1
fail() { echo "FAIL: $*" >&2; exit 1; }
git rev-parse --git-dir >/dev/null 2>&1 || fail "not a git repository"
base=${BENCH_BASE_COMMIT:-$(git rev-list --max-parents=0 --all | tail -n 1)}
branch=feature/polite-greeting

[ "$(git symbolic-ref --short -q HEAD)" = main ] || fail "current branch is not main"
[ "$(git rev-parse main)" = "$base" ] || fail "main moved away from the base commit"
git rev-parse -q --verify "refs/heads/$branch" >/dev/null || fail "branch $branch does not exist"
[ "$(git rev-list --count "$base..$branch")" = 1 ] || fail "$branch must have exactly one commit on top of main"
[ "$(git rev-parse "$branch^")" = "$base" ] || fail "$branch is not based on main"
subject=$(git log -1 --format=%s "$branch")
[ "$subject" = "feat: polite greeting" ] || fail "unexpected commit subject: $subject"
changed=$(git diff --name-only "$base" "$branch" | sort | tr '\n' ' ')
[ "$changed" = "CHANGELOG.md greet.py " ] || fail "commit should change exactly CHANGELOG.md and greet.py (got: $changed)"

dirty=$(git status --porcelain --untracked-files=all | grep -v -e '__pycache__' -e '\.desk-agent/' || true)
[ -z "$dirty" ] || fail "working tree is not clean:
$dirty"
# main's files must be untouched in the working tree too.
git diff --quiet "$base" -- greet.py CHANGELOG.md || fail "main working tree differs from base"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
git show "$branch:greet.py" > "$tmp/greet.py"
git show "$branch:CHANGELOG.md" > "$tmp/CHANGELOG.md"
out=$(cd "$tmp" && python3 -c 'from greet import greet; print(repr(greet("Ada")), repr(greet("Bob Lee")))') || fail "greet.py on $branch is broken"
[ "$out" = "'Hello, Ada!' 'Hello, Bob Lee!'" ] || fail "greet() returns $out"
python3 - "$tmp/CHANGELOG.md" <<'PY' || fail "CHANGELOG.md entry missing or misplaced"
import sys
lines = open(sys.argv[1], encoding="utf-8").read().splitlines()
i = lines.index("## Unreleased")
following = [l for l in lines[i + 1:] if l.strip()]
assert following[0] == "- Polite greeting punctuation", following[:2]
assert "- Initial greeting module" in lines and "## 0.1.0" in lines
PY
echo "ok"
