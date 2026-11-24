`report.py` contains three functions that each re-implement the same text-table formatting
logic. Refactor it:

1. Create a new module `formatting.py` with
   - `format_row(cells, widths, aligns)` — `cells` is a sequence of values (converted with
     `str()`), `widths` a sequence of column widths, `aligns` a string with one character per
     column: `"l"` pads on the right (left-aligned), `"r"` pads on the left (right-aligned).
     Returns `"| " + " | ".join(padded cells) + " |"`. Cells longer than their width are not
     truncated. Raise `ValueError` if the three sequences have different lengths or an align
     character is not `l`/`r`.
   - `format_table(header, rows, aligns)` — computes each column width as the maximum `str()`
     length over the header and all rows, and returns the lines (joined with `"\n"`, no
     trailing newline): the header row (formatted with the same `aligns`), a separator line
     made of `"|-"`, then per column `"-" * width` joined by `"-|-"`, then `"-|"`, followed by
     one line per data row.
2. Rewrite `sales_table`, `inventory_table` and `staff_table` in `report.py` to use
   `format_table` so that `report.py` no longer contains any manual padding (no `ljust`,
   `rjust`, `center` or width format specs). Their output must stay byte-for-byte identical.

`test_report.py` must keep passing unchanged (`python3 -m unittest -v`).
