#!/usr/bin/env python3
"""Count words in a text file."""
import argparse
import re
import sys
from collections import Counter

WORD = re.compile(r"[A-Za-z']+")


def count_words(text):
    return Counter(WORD.findall(text))


def main(argv=None):
    parser = argparse.ArgumentParser(description="Count words in a text file.")
    parser.add_argument("file", help="text file to analyse")
    args = parser.parse_args(argv)

    try:
        with open(args.file, encoding="utf-8") as handle:
            text = handle.read()
    except OSError as exc:
        print(f"wordfreq: {exc}", file=sys.stderr)
        return 1

    counts = count_words(text)
    print(f"words: {sum(counts.values())}")
    print(f"unique: {len(counts)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
