#!/usr/bin/env bash
# Hidden verifier for rename-across-files.
set -u
export PYTHONDONTWRITEBYTECODE=1
ws=$PWD
fail() { echo "FAIL: $*" >&2; exit 1; }

[ -f shop/totals.py ] || fail "shop/totals.py does not exist"
[ ! -e shop/pricing.py ] || fail "shop/pricing.py still exists"
find . -path ./.git -prune -o -name 'pricing.py' -print | grep -q . && fail "a pricing.py file still exists"
leftover=$(grep -rnw --exclude-dir=.git --exclude-dir=__pycache__ --exclude-dir=.desk-agent -e calc_ttl -e pricing . 2>/dev/null)
[ -z "$leftover" ] || fail "old names still present:
$leftover"
grep -q 'calculate_total' README.md || fail "README.md does not mention calculate_total"
grep -q 'totals' README.md || fail "README.md does not mention the totals module"

expected=$'tax rate: 8%\ncart total: 13.50\nwith 10% off: 12.15\nsingle pen: 1.62'
out=$(python3 -m shop.cli 2>&1) || fail "python3 -m shop.cli failed: $out"
[ "$out" = "$expected" ] || fail "cli output changed: $out"

log=$(python3 -m unittest discover -s tests -t . 2>&1) || fail "workspace tests fail: $log"
echo "$log" | grep -Eq 'Ran ([4-9]|[1-9][0-9]+) tests' || fail "workspace tests were removed: $log"

python3 - "$ws" <<'PY' || exit 1
import importlib
import sys

sys.path.insert(0, sys.argv[1])
import shop  # noqa: E402
from shop.cart import Cart  # noqa: E402
from shop.totals import TAX_RATE, calculate_total  # noqa: E402

errors = []
if shop.calculate_total is not calculate_total:
    errors.append("shop.calculate_total should be re-exported from shop.totals")
if hasattr(shop, "calc_ttl"):
    errors.append("compat alias shop.calc_ttl must be removed")
if "calculate_total" not in getattr(shop, "__all__", []):
    errors.append("shop.__all__ should list calculate_total")
try:
    importlib.import_module("shop.pricing")
    errors.append("shop.pricing is still importable")
except ImportError:
    pass
if calculate_total([(10.0, 2)]) != round(20 * (1 + TAX_RATE), 2):
    errors.append("calculate_total result changed")
if calculate_total([(100.0, 1)], 0.5) != 54.0:
    errors.append("discount handling changed")
try:
    calculate_total([(1.0, 1)], -0.1)
    errors.append("negative discount must raise ValueError")
except ValueError:
    pass
cart = Cart()
cart.add("x", 2.5, 4)
if cart.total(0.2) != 8.64:
    errors.append(f"Cart.total changed: {cart.total(0.2)}")
if errors:
    print("\n".join(errors), file=sys.stderr)
    sys.exit(1)
PY
echo "ok"
