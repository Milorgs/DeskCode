#!/usr/bin/env bash
# Hidden verifier for spec-parse-duration. The tests live only here.
set -u
export PYTHONDONTWRITEBYTECODE=1
ws=$PWD
[ -f "$ws/durations.py" ] || { echo "durations.py missing" >&2; exit 1; }
exec python3 - "$ws" <<'PY'
import sys
import unittest

sys.path.insert(0, sys.argv[1])
from durations import format_duration, parse_duration  # noqa: E402


class Parse(unittest.TestCase):
    def test_valid(self):
        cases = {
            "1h30m": 5400, "2d": 172800, "45s": 45, "0m": 0, "1w": 604800,
            "1h 30m": 5400, "  1h   5m 3s  ": 3903, "1H30M": 5400, "1W2D3H4M5S": 788645,
            "90": 90, "  007 ": 7, "0": 0, "010m": 600, "1d\t2h\n": 93600,
            "1w1d1h1m1s": 694861, "100s": 100, "3h 0m 1s": 10801, "999999w": 999999 * 604800,
        }
        for text, expected in cases.items():
            with self.subTest(text=text):
                self.assertEqual(parse_duration(text), expected)

    def test_invalid(self):
        bad = ["", "   ", "5x", "1h1h", "30m1h", "1s1m", "-5m", "+5m", "1.5h", "1h30", "h", "1 h",
               "5m-", "1h,30m", "1hh", "m5", "１h", "٣s", "1d 1d", "s1", "1_000s", "0x10s", "5 5"]
        for text in bad:
            with self.subTest(text=text):
                with self.assertRaises(ValueError):
                    parse_duration(text)

    def test_type_error(self):
        for value in (None, 5, 1.5, b"1h"):
            with self.subTest(value=value):
                with self.assertRaises(TypeError):
                    parse_duration(value)


class Format(unittest.TestCase):
    def test_values(self):
        cases = {0: "0s", 1: "1s", 59: "59s", 60: "1m", 61: "1m1s", 3600: "1h", 5400: "1h30m",
                 86400: "1d", 90061: "1d1h1m1s", 604800: "1w", 694861: "1w1d1h1m1s",
                 1209600: "2w", 604801: "1w1s"}
        for seconds, expected in cases.items():
            with self.subTest(seconds=seconds):
                self.assertEqual(format_duration(seconds), expected)

    def test_errors(self):
        with self.assertRaises(ValueError):
            format_duration(-1)
        for value in (1.0, "60", None, True):
            with self.subTest(value=value):
                with self.assertRaises(TypeError):
                    format_duration(value)

    def test_round_trip(self):
        for n in list(range(0, 4000, 7)) + [86399, 86400, 86401, 604799, 10**9, 123456789]:
            self.assertEqual(parse_duration(format_duration(n)), n)
            self.assertEqual(parse_duration(format_duration(n).upper()), n)


suite = unittest.TestSuite()
for case in (Parse, Format):
    suite.addTests(unittest.defaultTestLoader.loadTestsFromTestCase(case))
result = unittest.TextTestRunner(stream=sys.stderr, verbosity=1).run(suite)
sys.exit(0 if result.wasSuccessful() else 1)
PY
