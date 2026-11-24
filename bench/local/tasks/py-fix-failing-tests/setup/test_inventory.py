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


if __name__ == "__main__":
    unittest.main()
