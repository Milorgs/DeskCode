# shop

A toy shop package used in the onboarding docs.

- `shop/pricing.py` — `calc_ttl(items, discount=0.0)` computes the total price after discount and tax.
- `shop/cart.py` — `Cart` collects lines and calls `pricing.calc_ttl`.
- `shop/cli.py` — demo, run with `python3 -m shop.cli`.

Run the tests with `python3 -m unittest discover -s tests -t .`.
