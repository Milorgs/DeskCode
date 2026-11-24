#!/usr/bin/env python3
"""Render one or more DeskCode benchmark JSON reports into a Markdown leaderboard table.

Usage:
    python3 bench/render_leaderboard.py bench/results/*.json [--out bench/LEADERBOARD.md]
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path


def load_row(path: Path) -> dict:
    data = json.loads(path.read_text(encoding="utf-8"))
    summary = data.get("summary") or {}
    meta = data.get("environment") or {}
    passed = int(summary.get("passed") or 0)
    total = int(summary.get("total_runs") or summary.get("total") or 0)
    rate = float(summary.get("pass_rate") or (passed / total if total else 0.0)) * 100.0
    return {
        "label": data.get("label") or meta.get("model") or path.stem,
        "model": meta.get("model") or data.get("label") or "-",
        "protocol": meta.get("protocol") or "openai",
        "desk_version": meta.get("desk_version") or "-",
        "passed": passed,
        "total": total,
        "rate": rate,
        "avg_s": float(summary.get("mean_wall_seconds") or summary.get("avg_seconds") or 0.0),
        "tokens": int(summary.get("total_tokens") or 0),
        "date": (data.get("started_at") or "")[:10] or "-",
    }


def render_table(rows: list[dict]) -> str:
    rows = sorted(rows, key=lambda r: (-r["rate"], r["avg_s"]))
    lines = [
        "| Model | Protocol | DeskCode | Pass Rate | Passed | Avg Time | Total Tokens | Date |",
        "| :--- | :--- | :--- | :---: | :---: | :---: | :---: | :---: |",
    ]
    for r in rows:
        lines.append(
            f"| `{r['model']}` | `{r['protocol']}` | `{r['desk_version']}` | "
            f"**{r['rate']:.1f}%** | {r['passed']}/{r['total']} | {r['avg_s']:.1f}s | "
            f"{r['tokens']:,} | {r['date']} |"
        )
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reports", nargs="+", type=Path, help="bench/results/*.json files")
    parser.add_argument("--out", type=Path, default=None, help="Optional Markdown file to write")
    args = parser.parse_args(argv)

    rows = [load_row(p) for p in args.reports if p.is_file()]
    if not rows:
        print("No valid benchmark JSON files provided.", file=sys.stderr)
        return 1
    md = render_table(rows)
    if args.out:
        args.out.write_text(md, encoding="utf-8")
    else:
        sys.stdout.write(md)
    return 0


if __name__ == "__main__":
    sys.exit(main())
