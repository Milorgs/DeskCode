`wordfreq.py` is a small command-line tool: `python3 wordfreq.py FILE` prints the number of
words and of distinct words in FILE (a word is a maximal run of ASCII letters and
apostrophes, matched by the regex `[A-Za-z']+`).

Add two options, keeping the existing behaviour unchanged when they are not used:

1. `--top N` — after the existing two lines, print the N most frequent words, one per line,
   formatted as `<word> <count>` (single space). Order by count descending, and break ties
   alphabetically (plain string order) ascending. If fewer than N distinct words exist,
   print all of them. N must be a positive integer: for `0`, negative or non-integer values
   the program must exit with status 2 and print an error to stderr (argparse's standard
   error handling is fine).
2. `--ignore-case` — lowercase words before counting. This affects the distinct count and
   the `--top` listing. Without it, `The` and `the` are different words.

Both options can be combined and may appear before or after FILE. Also add a short section
describing the options to `README.md`.
