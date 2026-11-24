Implement the two functions in `durations.py` according to this specification (the module
currently only contains stubs). Use only the Python standard library. A hidden test suite
will check every rule below, so consider writing your own tests.

### `parse_duration(text: str) -> int`

Returns the total number of seconds described by `text`.

- Units: `w` (week = 604800 s), `d` (86400 s), `h` (3600 s), `m` (60 s), `s` (1 s).
- A duration is one or more *components*; each component is a non-negative decimal integer
  (ASCII digits only, leading zeros allowed) immediately followed by a unit letter, e.g.
  `1h30m`, `2d`, `45s`, `0m`.
- Components may be separated by any amount of whitespace (`1h 30m`, `1h   5m 3s`), and
  leading/trailing whitespace is ignored. There must be no whitespace between a number and
  its unit.
- Unit letters are case-insensitive (`1H30M` equals `1h30m`).
- Each unit may appear at most once and units must appear in descending order
  (`w`, `d`, `h`, `m`, `s`). `30m1h` and `1h1h` are invalid.
- A string consisting only of an integer (after trimming whitespace), e.g. `"90"`, means
  that many seconds.
- Anything else raises `ValueError`: empty or whitespace-only strings, unknown units,
  signs (`-5m`, `+5m`), decimals (`1.5h`), a number without a unit following a component
  (`1h30`), a unit without a number (`h`), or non-ASCII digits.
- Non-`str` input raises `TypeError`.

### `format_duration(seconds: int) -> str`

The canonical inverse: the shortest string using the units above, largest first, without
spaces and omitting zero components, e.g. `5400 -> "1h30m"`, `694861 -> "1w1d1h1m1s"`.
`0 -> "0s"`. Negative numbers raise `ValueError`; non-`int` input (including `bool`) raises
`TypeError`. For every non-negative int `n`, `parse_duration(format_duration(n)) == n`.
