#!/usr/bin/env bash
# Hidden verifier for c-memory-bugs: compiles strlist.c with a hidden stress driver under
# AddressSanitizer + UndefinedBehaviorSanitizer (falls back to a plain build if the
# toolchain has no sanitizer runtime) and compares results.
set -u
ws=$PWD
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }

[ -f "$ws/strlist.c" ] && [ -f "$ws/strlist.h" ] && [ -f "$ws/demo.c" ] || fail "source files missing"
# The API header must keep its contract.
for sym in strlist_init strlist_push strlist_remove strlist_join strlist_free; do
    grep -q "$sym" "$ws/strlist.h" || fail "strlist.h lost $sym"
done

cat > "$tmp/driver.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "strlist.h"

#define CHECK(cond) do { if (!(cond)) { printf("check failed line %d: %s\n", __LINE__, #cond); return 1; } } while (0)

int main(void)
{
    struct strlist l;
    char buf[64];
    strlist_init(&l);

    char *s = strlist_join(&l, "-");
    CHECK(s && strcmp(s, "") == 0);
    free(s);
    CHECK(strlist_remove(&l, 0) == -1);

    CHECK(strlist_push(&l, "") == 0);
    s = strlist_join(&l, "+");
    CHECK(s && strcmp(s, "") == 0);
    free(s);
    CHECK(strlist_remove(&l, 1) == -1);
    CHECK(strlist_remove(&l, 0) == 0);
    CHECK(l.len == 0);

    for (int i = 0; i < 200; i++) {
        snprintf(buf, sizeof buf, "item-%03d-%s", i, (i % 7 == 0) ? "with-a-longer-tail-to-vary-sizes" : "x");
        CHECK(strlist_push(&l, buf) == 0);
    }
    CHECK(l.len == 200);
    CHECK(strlist_remove(&l, 200) == -1);
    CHECK(strlist_remove(&l, 199) == 0);   /* last */
    CHECK(strlist_remove(&l, 0) == 0);     /* first */
    CHECK(strlist_remove(&l, 50) == 0);    /* middle: item-052 */
    CHECK(l.len == 197);
    CHECK(strcmp(l.items[0], "item-001-x") == 0);
    CHECK(strcmp(l.items[49], "item-050-x") == 0);
    CHECK(strcmp(l.items[50], "item-052-x") == 0);
    CHECK(strcmp(l.items[196], "item-198-x") == 0);

    s = strlist_join(&l, "::");
    CHECK(s != NULL);
    size_t expect = 0;
    for (size_t i = 0; i < l.len; i++) expect += strlen(l.items[i]);
    expect += 2 * (l.len - 1);
    CHECK(strlen(s) == expect);
    CHECK(strncmp(s, "item-001-x::item-002-x::", 24) == 0);
    free(s);

    strlist_free(&l);
    CHECK(l.len == 0 && l.items == NULL);

    /* reuse after free */
    CHECK(strlist_push(&l, "solo") == 0);
    CHECK(strlist_push(&l, "pair") == 0);
    CHECK(strlist_push(&l, "trio") == 0);
    s = strlist_join(&l, ", ");
    CHECK(s && strcmp(s, "solo, pair, trio") == 0);
    free(s);
    CHECK(strlist_remove(&l, 1) == 0);
    s = strlist_join(&l, "");
    CHECK(s && strcmp(s, "solotrio") == 0);
    free(s);
    strlist_free(&l);
    strlist_free(&l);  /* freeing an empty list is fine */
    printf("driver ok\n");
    return 0;
}
C

san="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
echo 'int main(void){return 0;}' > "$tmp/probe.c"
if ! cc $san -o "$tmp/probe" "$tmp/probe.c" >/dev/null 2>&1 || ! "$tmp/probe" >/dev/null 2>&1; then
    echo "note: sanitizers unavailable, using a plain build" >&2
    san=""
fi
export ASAN_OPTIONS=detect_leaks=0:abort_on_error=0:exitcode=86
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1

cc -std=c11 -g -O1 $san -I"$ws" -o "$tmp/driver" "$tmp/driver.c" "$ws/strlist.c" >"$tmp/cc.log" 2>&1 \
    || { cat "$tmp/cc.log" >&2; fail "driver failed to compile"; }
out=$("$tmp/driver" 2>"$tmp/run.log"); rc=$?
[ $rc -eq 0 ] && [ "$out" = "driver ok" ] || { echo "$out" >&2; head -40 "$tmp/run.log" >&2; fail "hidden driver failed (rc=$rc)"; }

cc -std=c11 -g -O1 $san -I"$ws" -o "$tmp/demo" "$ws/demo.c" "$ws/strlist.c" >"$tmp/cc2.log" 2>&1 \
    || { cat "$tmp/cc2.log" >&2; fail "demo failed to compile"; }
out=$("$tmp/demo" 2>"$tmp/demo.log"); rc=$?
[ $rc -eq 0 ] && [ "$out" = "4 items: alpha, gamma, delta, epsilon" ] || { echo "$out" >&2; head -40 "$tmp/demo.log" >&2; fail "demo failed (rc=$rc)"; }
echo "ok"
