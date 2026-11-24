Create an executable script `summarize.sh` in the workspace root. `./summarize.sh FILE`
reads an orders CSV file (see `data/orders.csv` for an example) and prints a per-region
summary to stdout. You may implement the logic in any language available here (bash,
awk, python3, ...) as long as `./summarize.sh FILE` works from any current directory.

Input rules:

- The first line is a header. Columns can appear in **any order**; the relevant ones are
  `region`, `quantity`, `unit_price` and `status` (other columns must be ignored).
- Fields follow standard CSV quoting (RFC 4180): fields may be wrapped in double quotes,
  quoted fields may contain commas and escaped quotes (`""`).
- Ignore blank lines. Line endings may be LF or CRLF. Trim surrounding whitespace from
  `region` and `status` values.
- Skip rows whose `status` is `cancelled` (case-insensitive).
- Amount of a row = `quantity * unit_price`.

Output format:

- First line exactly `region,orders,total`.
- Then one line per region: `<region>,<number of counted rows>,<sum of amounts with exactly 2 decimals>`.
  Print the region name as-is (trimmed, unquoted), even if it contains commas or quotes.
- Sort by total descending (compare totals rounded to cents, so `3.0000000001` ties with
  `3.00`); ties by region name ascending (plain string order).
- Regions whose rows were all cancelled do not appear.
- Exit status 0 on success. If FILE is missing or unreadable, print an error to stderr and
  exit with status 1.

Example: for `data/orders.csv` the expected output is in `data/expected.txt`.
