#!/usr/bin/env bash
# Hidden verifier for refactor-table-format.
set -u
export PYTHONDONTWRITEBYTECODE=1
ws=$PWD
for f in report.py formatting.py test_report.py; do
    [ -f "$ws/$f" ] || { echo "$f missing" >&2; exit 1; }
done
exec python3 - "$ws" <<'PY'
import ast
import importlib
import sys
import unittest

ws = sys.argv[1]
sys.path.insert(0, ws)
failures = []

# ---- 1. Original implementation (oracle) ---------------------------------
def o_sales(rows):
    header = ("Region", "Units", "Revenue")
    cells = [header] + [(r, str(u), f"{v:.2f}") for r, u, v in rows]
    w = [max(len(str(c[i])) for c in cells) for i in range(3)]
    lines = ["| " + header[0].ljust(w[0]) + " | " + header[1].rjust(w[1]) + " | " + header[2].rjust(w[2]) + " |"]
    lines.append("|-" + "-|-".join("-" * x for x in w) + "-|")
    for c in cells[1:]:
        lines.append("| " + c[0].ljust(w[0]) + " | " + c[1].rjust(w[1]) + " | " + c[2].rjust(w[2]) + " |")
    return "\n".join(lines)

def o_inventory(items):
    header = ("SKU", "Name", "Qty")
    cells = [header] + [(sku, name, str(qty)) for sku, (name, qty) in sorted(items.items())]
    w = [max(len(c[i]) for c in cells) for i in range(3)]
    lines = ["| " + header[0].ljust(w[0]) + " | " + header[1].ljust(w[1]) + " | " + header[2].rjust(w[2]) + " |"]
    lines.append("|-" + "-|-".join("-" * x for x in w) + "-|")
    for c in cells[1:]:
        lines.append("| " + c[0].ljust(w[0]) + " | " + c[1].ljust(w[1]) + " | " + c[2].rjust(w[2]) + " |")
    return "\n".join(lines)

def o_staff(people):
    header = ["Name", "Role", "Hours"]
    body = [[p["name"], p["role"], f"{p['hours']:g}"] for p in people]
    w = [max(len(row[i]) for row in [header] + body) for i in range(3)]
    lines = ["| %s | %s | %s |" % (header[0].ljust(w[0]), header[1].ljust(w[1]), header[2].rjust(w[2]))]
    lines.append("|-%s-|" % "-|-".join("-" * x for x in w))
    for row in body:
        lines.append("| %s | %s | %s |" % (row[0].ljust(w[0]), row[1].ljust(w[1]), row[2].rjust(w[2])))
    return "\n".join(lines)

try:
    report = importlib.import_module("report")
    formatting = importlib.import_module("formatting")
except Exception as exc:  # noqa: BLE001
    print(f"import failed: {exc!r}", file=sys.stderr)
    sys.exit(1)

sales_cases = [[], [("North", 12, 340.5), ("South", 3, 12)],
               [("A very long region name", 123456, 0.004), ("X", 0, 99999.999)]]
inv_cases = [{}, {"Z-9": ("z", 1), "A-1": ("Something quite long", 1234567), "M": ("", 0)}]
staff_cases = [[], [{"name": "Christopher", "role": "QA", "hours": 0.25},
                    {"name": "Li", "role": "Principal Engineer", "hours": 1000}]]
for rows in sales_cases:
    if report.sales_table(rows) != o_sales(rows):
        failures.append(f"sales_table output changed for {rows!r}")
for items in inv_cases:
    if report.inventory_table(items) != o_inventory(items):
        failures.append(f"inventory_table output changed for {items!r}")
for people in staff_cases:
    if report.staff_table(people) != o_staff(people):
        failures.append(f"staff_table output changed for {people!r}")

# ---- 2. formatting API ----------------------------------------------------
fr = getattr(formatting, "format_row", None)
ft = getattr(formatting, "format_table", None)
if not callable(fr) or not callable(ft):
    failures.append("formatting.format_row / format_table missing")
else:
    checks = [
        (lambda: fr(["a", 1], [3, 4], "lr"), "| a   |    1 |"),
        (lambda: fr(("toolong",), (3,), "r"), "| toolong |"),
        (lambda: fr([], [], ""), "|  |"),
        (lambda: ft(["k", "value"], [["x", 10], ["long-key", 2]], "lr"),
         "| k        | value |\n|----------|-------|\n| x        |    10 |\n| long-key |     2 |"),
        (lambda: ft(("h",), [], "r"), "| h |\n|---|"),
    ]
    for fn, expected in checks:
        try:
            got = fn()
        except Exception as exc:  # noqa: BLE001
            got = repr(exc)
        if got != expected:
            failures.append(f"formatting result {got!r} != {expected!r}")
    for bad in (lambda: fr(["a"], [1, 2], "l"), lambda: fr(["a"], [1], "lr"), lambda: fr(["a"], [1], "c")):
        try:
            bad()
            failures.append("format_row should raise ValueError on bad input")
        except ValueError:
            pass
        except Exception as exc:  # noqa: BLE001
            failures.append(f"format_row raised {exc!r} instead of ValueError")

# ---- 3. structure: report.py must delegate to formatting -------------------
source = open(f"{ws}/report.py", encoding="utf-8").read()
tree = ast.parse(source)
imports_formatting = any(
    (isinstance(n, ast.ImportFrom) and n.module == "formatting")
    or (isinstance(n, ast.Import) and any(a.name == "formatting" for a in n.names))
    for n in ast.walk(tree))
if not imports_formatting:
    failures.append("report.py does not import formatting")
if "format_table" not in source:
    failures.append("report.py does not use format_table")
for node in ast.walk(tree):
    if isinstance(node, ast.Attribute) and node.attr in {"ljust", "rjust", "center"}:
        failures.append(f"report.py still pads manually ({node.attr})")
        break
    if (isinstance(node, ast.JoinedStr) and any(
            isinstance(v, ast.FormattedValue) and v.format_spec is not None and any(
                isinstance(p, ast.Constant) and any(ch in str(p.value) for ch in "<>^") for p in ast.walk(v.format_spec))
            for v in node.values)):
        failures.append("report.py still uses alignment format specs")
        break
import re
if re.search(r"%-?\d+s", source) or re.search(r"\{[^{}]*:[^{}]*[<>^][^{}]*\}", source):
    failures.append("report.py still uses width/alignment formatting")

# ---- 4. visible tests (pristine copy) --------------------------------------
TESTS = '''
import unittest
from report import inventory_table, sales_table, staff_table

class ReportTest(unittest.TestCase):
    def test_sales(self):
        self.assertEqual(sales_table([("North", 12, 340.5), ("South", 3, 12)]),
            "| Region | Units | Revenue |\\n|--------|-------|---------|\\n| North  |    12 |  340.50 |\\n| South  |     3 |   12.00 |")
    def test_inventory(self):
        self.assertEqual(inventory_table({"B-22": ("Large gizmo", 120), "A-01": ("Widget", 7)}),
            "| SKU  | Name        | Qty |\\n|------|-------------|-----|\\n| A-01 | Widget      |   7 |\\n| B-22 | Large gizmo | 120 |")
    def test_staff(self):
        people = [{"name": "Ana", "role": "Engineer", "hours": 37.5}, {"name": "Bo", "role": "Designer", "hours": 40}]
        self.assertEqual(staff_table(people),
            "| Name | Role     | Hours |\\n|------|----------|-------|\\n| Ana  | Engineer |  37.5 |\\n| Bo   | Designer |    40 |")
'''
namespace = {"__name__": "pristine_tests"}
exec(compile(TESTS, "pristine_tests", "exec"), namespace)
suite = unittest.defaultTestLoader.loadTestsFromTestCase(namespace["ReportTest"])
result = unittest.TextTestRunner(stream=sys.stderr, verbosity=0).run(suite)
if not result.wasSuccessful():
    failures.append("visible tests fail")

if failures:
    print("\n".join(failures), file=sys.stderr)
    sys.exit(1)
print("ok")
PY
