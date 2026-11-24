#!/usr/bin/env bash
# Reference solution for refactor-table-format.
set -eu
cat > formatting.py <<'PY'
"""Shared plain-text table formatting."""


def format_row(cells, widths, aligns):
    cells, widths = list(cells), list(widths)
    if not (len(cells) == len(widths) == len(aligns)):
        raise ValueError("cells, widths and aligns must have the same length")
    padded = []
    for cell, width, align in zip(cells, widths, aligns):
        text = str(cell)
        if align == "l":
            padded.append(text.ljust(width))
        elif align == "r":
            padded.append(text.rjust(width))
        else:
            raise ValueError(f"unknown alignment {align!r}")
    return "| " + " | ".join(padded) + " |"


def format_table(header, rows, aligns):
    header, rows = list(header), [list(row) for row in rows]
    widths = [max(len(str(row[i])) for row in [header] + rows) for i in range(len(header))]
    lines = [format_row(header, widths, aligns), "|-" + "-|-".join("-" * w for w in widths) + "-|"]
    lines += [format_row(row, widths, aligns) for row in rows]
    return "\n".join(lines)
PY
cat > report.py <<'PY'
"""Plain-text reports for the weekly status mail."""
from formatting import format_table


def sales_table(rows):
    """rows: list of (region, units, revenue) tuples."""
    body = [(region, str(units), f"{revenue:.2f}") for region, units, revenue in rows]
    return format_table(("Region", "Units", "Revenue"), body, "lrr")


def inventory_table(items):
    """items: dict of sku -> (name, quantity)."""
    body = [(sku, name, str(qty)) for sku, (name, qty) in sorted(items.items())]
    return format_table(("SKU", "Name", "Qty"), body, "llr")


def staff_table(people):
    """people: list of dicts with 'name', 'role' and 'hours'."""
    body = [(p["name"], p["role"], f"{p['hours']:g}") for p in people]
    return format_table(("Name", "Role", "Hours"), body, "llr")
PY
