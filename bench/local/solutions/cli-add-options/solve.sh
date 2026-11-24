#!/usr/bin/env bash
# Reference solution for cli-add-options.
set -eu
cat > wordfreq.py <<'PY'
#!/usr/bin/env python3
"""Count words in a text file."""
import argparse
import re
import sys
from collections import Counter

WORD = re.compile(r"[A-Za-z']+")


def count_words(text, ignore_case=False):
    words = WORD.findall(text)
    if ignore_case:
        words = [word.lower() for word in words]
    return Counter(words)


def positive_int(value):
    try:
        number = int(value)
    except ValueError:
        raise argparse.ArgumentTypeError(f"invalid positive integer: {value!r}")
    if number <= 0:
        raise argparse.ArgumentTypeError(f"must be a positive integer: {value!r}")
    return number


def main(argv=None):
    parser = argparse.ArgumentParser(description="Count words in a text file.")
    parser.add_argument("file", help="text file to analyse")
    parser.add_argument("--top", type=positive_int, metavar="N", help="show the N most frequent words")
    parser.add_argument("--ignore-case", action="store_true", help="lowercase words before counting")
    args = parser.parse_args(argv)

    try:
        with open(args.file, encoding="utf-8") as handle:
            text = handle.read()
    except OSError as exc:
        print(f"wordfreq: {exc}", file=sys.stderr)
        return 1

    counts = count_words(text, args.ignore_case)
    print(f"words: {sum(counts.values())}")
    print(f"unique: {len(counts)}")
    if args.top:
        for word, count in sorted(counts.items(), key=lambda kv: (-kv[1], kv[0]))[: args.top]:
            print(f"{word} {count}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
PY
cat >> README.md <<'MD'

## Options

- `--top N` — also print the N most frequent words (`word count`, ties sorted alphabetically).
- `--ignore-case` — lowercase words before counting.
MD
