"""Tiny inventory helpers used by the warehouse scripts.

Inventory files contain one item per line in the form ``name,qty,price``.
Blank lines and comment lines (starting with ``#``, possibly indented) are ignored.
"""


def parse_line(line):
    """Parse ``'name,qty,price'`` into ``(name, qty, price)``.

    Surrounding whitespace around every field is ignored; ``qty`` is an int and
    ``price`` a float.  Raises ``ValueError`` for malformed lines.
    """
    name, qty, price = line.split(",")
    return name, int(qty), float(price)


def load_items(lines):
    """Return a list of ``(name, qty, price)`` tuples, skipping blanks and comments."""
    items = []
    for line in lines:
        if not line.strip() or line.startswith("#"):
            continue
        items.append(parse_line(line))
    return items


def total_value(lines):
    """Total stock value (sum of qty * price) rounded to 2 decimals."""
    total = 0
    for _, qty, price in load_items(lines):
        total += qty * price
    return round(total, 2)


def restock_list(items, threshold):
    """Names of items whose quantity is strictly below ``threshold``.

    Ordered by quantity ascending; items with equal quantity are ordered by name.
    """
    return [name for name, qty, _ in sorted(items, key=lambda item: item[1]) if qty <= threshold]
