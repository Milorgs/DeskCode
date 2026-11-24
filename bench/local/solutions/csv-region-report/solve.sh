#!/usr/bin/env bash
# Reference solution for csv-region-report.
set -eu
cat > summarize.sh <<'SH'
#!/usr/bin/env bash
set -u
if [ $# -ne 1 ] || [ ! -r "$1" ] || [ -d "$1" ]; then
    echo "usage: summarize.sh FILE (file missing or unreadable: ${1:-})" >&2
    exit 1
fi
exec python3 - "$1" <<'PY'
import csv
import sys
from collections import defaultdict

with open(sys.argv[1], encoding="utf-8", newline="") as handle:
    rows = [r for r in csv.reader(handle) if r and any(c.strip() for c in r)]
header = [h.strip() for h in rows[0]]
col = {name: header.index(name) for name in ("region", "quantity", "unit_price", "status")}
totals, counts = defaultdict(float), defaultdict(int)
for row in rows[1:]:
    if row[col["status"]].strip().lower() == "cancelled":
        continue
    region = row[col["region"]].strip()
    totals[region] += float(row[col["quantity"]]) * float(row[col["unit_price"]])
    counts[region] += 1
print("region,orders,total")
for region in sorted(totals, key=lambda k: (-round(totals[k], 2), k)):
    print(f"{region},{counts[region]},{totals[region]:.2f}")
PY
SH
chmod +x summarize.sh
