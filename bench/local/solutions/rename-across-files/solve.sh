#!/usr/bin/env bash
# Reference solution for rename-across-files.
set -eu
git mv shop/pricing.py shop/totals.py 2>/dev/null || mv shop/pricing.py shop/totals.py
for f in shop/__init__.py shop/totals.py shop/cart.py shop/cli.py tests/test_pricing.py README.md; do
    sed -i -e 's/calc_ttl/calculate_total/g' -e 's/shop\.pricing/shop.totals/g' \
        -e 's/from shop import pricing/from shop import totals/' -e 's/pricing\.calculate_total/totals.calculate_total/g' \
        -e 's#shop/pricing\.py#shop/totals.py#g' "$f"
done
