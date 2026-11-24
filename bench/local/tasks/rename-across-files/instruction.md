In the `shop` package, perform this rename consistently across the whole project
(code, tests and documentation):

- Rename the module `shop/pricing.py` to `shop/totals.py`.
- Rename the function `calc_ttl` to `calculate_total`.

Update every import, call site, test and mention in `README.md`. Do not leave
compatibility aliases or a leftover `pricing` module: afterwards, neither the identifier
`calc_ttl` nor the module name `pricing` may appear anywhere in the project. Behaviour must
not change — `python3 -m unittest discover -s tests -t .` and `python3 -m shop.cli` must
still work.
