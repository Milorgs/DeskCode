"""Plain-text reports for the weekly status mail."""


def sales_table(rows):
    """rows: list of (region, units, revenue) tuples."""
    header = ("Region", "Units", "Revenue")
    cells = [header] + [(r, str(u), f"{v:.2f}") for r, u, v in rows]
    w0 = max(len(str(c[0])) for c in cells)
    w1 = max(len(str(c[1])) for c in cells)
    w2 = max(len(str(c[2])) for c in cells)
    lines = ["| " + header[0].ljust(w0) + " | " + header[1].rjust(w1) + " | " + header[2].rjust(w2) + " |"]
    lines.append("|-" + "-" * w0 + "-|-" + "-" * w1 + "-|-" + "-" * w2 + "-|")
    for c in cells[1:]:
        lines.append("| " + c[0].ljust(w0) + " | " + c[1].rjust(w1) + " | " + c[2].rjust(w2) + " |")
    return "\n".join(lines)


def inventory_table(items):
    """items: dict of sku -> (name, quantity)."""
    header = ("SKU", "Name", "Qty")
    cells = [header] + [(sku, name, str(qty)) for sku, (name, qty) in sorted(items.items())]
    widths = []
    for col in range(3):
        widths.append(max(len(c[col]) for c in cells))
    out = "| " + header[0].ljust(widths[0]) + " | " + header[1].ljust(widths[1]) + " | " + header[2].rjust(widths[2]) + " |\n"
    out += "|-" + "-|-".join("-" * w for w in widths) + "-|\n"
    for c in cells[1:]:
        out += "| " + c[0].ljust(widths[0]) + " | " + c[1].ljust(widths[1]) + " | " + c[2].rjust(widths[2]) + " |\n"
    return out.rstrip("\n")


def staff_table(people):
    """people: list of dicts with 'name', 'role' and 'hours'."""
    header = ["Name", "Role", "Hours"]
    body = [[p["name"], p["role"], f"{p['hours']:g}"] for p in people]
    widths = [max(len(row[i]) for row in [header] + body) for i in range(3)]
    result = []
    result.append("| %s | %s | %s |" % (header[0].ljust(widths[0]), header[1].ljust(widths[1]), header[2].rjust(widths[2])))
    result.append("|-%s-|" % "-|-".join("-" * w for w in widths))
    for row in body:
        result.append("| %s | %s | %s |" % (row[0].ljust(widths[0]), row[1].ljust(widths[1]), row[2].rjust(widths[2])))
    return "\n".join(result)
