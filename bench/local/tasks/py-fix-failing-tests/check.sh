#!/usr/bin/env bash
# Hidden verifier: runs a pristine copy of the visible tests plus extra cases.
# Runs with the task workspace as the current directory.
set -u
export PYTHONDONTWRITEBYTECODE=1
ws=$PWD
[ -f "$ws/inventory.py" ] || { echo "inventory.py is missing" >&2; exit 1; }
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat > "$tmp/test_visible.py" <<'PY'
import unittest

from inventory import load_items, parse_line, restock_list, total_value


class ParseLineTest(unittest.TestCase):
    def test_plain(self):
        self.assertEqual(parse_line("apple,3,0.5"), ("apple", 3, 0.5))

    def test_whitespace_is_ignored(self):
        self.assertEqual(parse_line("  pear , 4 , 1.25 \n"), ("pear", 4, 1.25))

    def test_malformed(self):
        with self.assertRaises(ValueError):
            parse_line("apple,3")


class LoadItemsTest(unittest.TestCase):
    def test_skips_comments_and_blanks(self):
        lines = ["# header", "apple,3,0.5", "", "   # indented comment", "kiwi,10,0.2\n"]
        self.assertEqual(load_items(lines), [("apple", 3, 0.5), ("kiwi", 10, 0.2)])

    def test_total_value(self):
        lines = ["apple,3,0.5", "  # note", "kiwi,10,0.2", "melon,1,2.99"]
        self.assertEqual(total_value(lines), 6.49)


class RestockTest(unittest.TestCase):
    def test_strictly_below_threshold(self):
        items = [("apple", 5, 0.5), ("kiwi", 2, 0.2), ("plum", 6, 1.0)]
        self.assertEqual(restock_list(items, 5), ["kiwi"])

    def test_ties_sorted_by_name(self):
        items = [("pear", 1, 1.0), ("fig", 3, 2.0), ("apple", 1, 0.5)]
        self.assertEqual(restock_list(items, 4), ["apple", "pear", "fig"])
PY

cat > "$tmp/test_hidden.py" <<'PY'
import unittest

from inventory import load_items, parse_line, restock_list, total_value


class Hidden(unittest.TestCase):
    def test_tab_whitespace(self):
        self.assertEqual(parse_line("\tdate ,\t7, 3.5\t"), ("date", 7, 3.5))

    def test_too_many_fields(self):
        with self.assertRaises(ValueError):
            parse_line("a,1,2,3")

    def test_bad_number(self):
        with self.assertRaises(ValueError):
            parse_line("a,x,2")

    def test_tab_comment_and_whitespace_only(self):
        lines = ["\t# tabbed comment", "   ", "lime, 2, 0.25", "\n"]
        self.assertEqual(load_items(lines), [("lime", 2, 0.25)])

    def test_total_value_empty(self):
        self.assertEqual(total_value([]), 0)

    def test_total_value_rounding(self):
        self.assertEqual(total_value(["a,3,0.1", "  b , 2 , 0.25"]), 0.8)

    def test_restock_equal_threshold_excluded(self):
        items = [("x", 3, 1.0), ("y", 2, 1.0), ("z", 3, 1.0)]
        self.assertEqual(restock_list(items, 3), ["y"])

    def test_restock_many_ties(self):
        items = [("delta", 0, 1), ("alpha", 2, 1), ("charlie", 0, 1), ("bravo", 2, 1)]
        self.assertEqual(restock_list(items, 10), ["charlie", "delta", "alpha", "bravo"])

    def test_restock_does_not_mutate(self):
        items = [("b", 1, 1.0), ("a", 1, 1.0)]
        restock_list(items, 5)
        self.assertEqual(items, [("b", 1, 1.0), ("a", 1, 1.0)])
PY

cd "$tmp" || exit 1
PYTHONPATH="$ws" python3 -m unittest -q test_visible test_hidden
