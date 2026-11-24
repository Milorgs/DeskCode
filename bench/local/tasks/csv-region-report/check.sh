#!/usr/bin/env bash
# Hidden verifier for csv-region-report: runs summarize.sh on unseen inputs and compares
# against an embedded oracle.
set -u
ws=$PWD
[ -f "$ws/summarize.sh" ] || { echo "summarize.sh missing" >&2; exit 1; }
[ -x "$ws/summarize.sh" ] || { echo "summarize.sh is not executable" >&2; exit 1; }
exec python3 - "$ws" <<'PY'
import csv
import io
import os
import subprocess
import sys
import tempfile
from collections import defaultdict

ws = sys.argv[1]


def oracle(text):
    rows = [r for r in csv.reader(io.StringIO(text, newline="")) if r and any(c.strip() for c in r)]
    header = [h.strip() for h in rows[0]]
    idx = {name: header.index(name) for name in ("region", "quantity", "unit_price", "status")}
    totals, counts = defaultdict(float), defaultdict(int)
    for r in rows[1:]:
        if r[idx["status"]].strip().lower() == "cancelled":
            continue
        region = r[idx["region"]].strip()
        totals[region] += float(r[idx["quantity"]]) * float(r[idx["unit_price"]])
        counts[region] += 1
    lines = ["region,orders,total"]
    for region in sorted(totals, key=lambda k: (-round(totals[k], 2), k)):
        lines.append(f"{region},{counts[region]},{totals[region]:.2f}")
    return "\n".join(lines) + "\n"


CASES = {
    "given.csv": open(os.path.join(ws, "data", "orders.csv"), encoding="utf-8").read()
    if os.path.exists(os.path.join(ws, "data", "orders.csv")) else None,
    "shuffled.csv": (
        'status,unit_price,note,region,quantity,id\n'
        'shipped,10.00,"plain",West,3,1\n'
        'pending,2.50,"has, comma",East,4,2\n'
        'Cancelled,999.99,"x",West,1,3\n'
        ' cancelled ,5.00,"y",North,2,4\n'
        'shipped,15.00,"say ""hi"", ok",East,1,5\n'
        '\n'
        'shipped,0.10,"",Zeta,3,6\n'
        'returned,7.00,"",North,1,7\n'
    ),
    "quoted_region.csv": (
        'region,quantity,unit_price,status\r\n'
        '"North, West",2,1.10,shipped\r\n'
        '"North, West",1,0.80,shipped\r\n'
        'Alpha,3,1.00,shipped\r\n'
        'Beta,1,3.00,SHIPPED\r\n'
        '"Gamma ""G""",1,0.01,shipped\r\n'
        'Delta,5,1.00,cancelled\r\n'
    ),
    "all_cancelled.csv": "region,quantity,unit_price,status\nOnly,1,1.00,cancelled\n",
}

failures = []
with tempfile.TemporaryDirectory() as tmp:
    for name, text in CASES.items():
        if text is None:
            failures.append("data/orders.csv was removed")
            continue
        path = os.path.join(tmp, name)
        with open(path, "w", encoding="utf-8", newline="") as handle:
            handle.write(text)
        result = subprocess.run([os.path.join(ws, "summarize.sh"), path], capture_output=True,
                                text=True, cwd=tmp, timeout=60)
        expected = oracle(text)
        got = result.stdout.replace("\r\n", "\n")
        if result.returncode != 0 or got.rstrip("\n") != expected.rstrip("\n"):
            failures.append(f"{name}: rc={result.returncode}\n--- expected\n{expected}--- got\n{result.stdout}{result.stderr}")
    missing = subprocess.run([os.path.join(ws, "summarize.sh"), os.path.join(tmp, "missing.csv")],
                             capture_output=True, text=True, cwd=tmp, timeout=60)
    if missing.returncode != 1 or not missing.stderr.strip():
        failures.append(f"missing file: expected exit 1 with stderr message, got rc={missing.returncode}")

if failures:
    print("\n\n".join(failures), file=sys.stderr)
    sys.exit(1)
print("ok")
PY
