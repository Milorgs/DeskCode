#!/usr/bin/env bash
# Hidden verifier for c-fix-build. Builds in a scratch copy of the workspace.
set -u
ws=$PWD
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }

for f in Makefile geometry.h geometry.c main.c; do
    [ -f "$ws/$f" ] || fail "$f is missing"
done
for flag in -std=c11 -Wall -Wextra -Werror; do
    grep -E '^[[:space:]]*CFLAGS[[:space:]]*[:+?]?=' "$ws/Makefile" | grep -q -- "$flag" || fail "Makefile CFLAGS lost $flag"
done
grep -q 'Wno-' "$ws/Makefile" && fail "Makefile disables warnings with -Wno-*"
grep -Eq 'pragma[[:space:]]+GCC[[:space:]]+diagnostic' "$ws"/*.c "$ws"/*.h && fail "warnings suppressed with #pragma"

cp -R "$ws" "$tmp/build"
cd "$tmp/build" || exit 1
make clean >/dev/null 2>&1
make >"$tmp/make.log" 2>&1 || { cat "$tmp/make.log" >&2; fail "make failed"; }
[ -x ./shapes ] || fail "./shapes was not produced"
expected=$'distance=5.00\nperimeter=12.00'
actual=$(./shapes) || fail "./shapes exited non-zero"
[ "$actual" = "$expected" ] || fail "unexpected output: $actual"

# Independent strict compile of the library with a hidden driver.
cat > "$tmp/driver.c" <<'C'
#include <stdio.h>
#include "geometry.h"
int main(void)
{
    struct point square[] = {{0, 0}, {2, 0}, {2, 2}, {0, 2}};
    struct point one[] = {{5, 5}};
    struct point two[] = {{0, 0}, {0, 1.5}};
    printf("%.3f %.3f %.3f %.3f %.3f\n", perimeter(square, 4), perimeter(one, 1),
           perimeter(square, 0), perimeter(two, 2),
           distance((struct point){-1, -1}, (struct point){2, 3}));
    return 0;
}
C
cc -std=c11 -Wall -Wextra -Werror -I"$tmp/build" -o "$tmp/driver" "$tmp/driver.c" "$tmp/build/geometry.c" -lm \
    >"$tmp/cc.log" 2>&1 || { cat "$tmp/cc.log" >&2; fail "strict compile of geometry.c failed"; }
out=$("$tmp/driver") || fail "driver crashed"
[ "$out" = "8.000 0.000 0.000 3.000 5.000" ] || fail "geometry results wrong: $out"
echo "ok"
