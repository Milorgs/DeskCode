#!/usr/bin/env bash
# Reference solution for spec-parse-duration.
set -eu
cat > durations.py <<'PY'
"""Human-friendly duration strings such as "1h30m"."""
import re

UNITS = (("w", 604800), ("d", 86400), ("h", 3600), ("m", 60), ("s", 1))
_COMPONENT = re.compile(r"([0-9]+)([wdhms])", re.ASCII | re.IGNORECASE)


def parse_duration(text):
    if not isinstance(text, str):
        raise TypeError("text must be a str")
    stripped = text.strip()
    if not stripped:
        raise ValueError("empty duration")
    if re.fullmatch(r"[0-9]+", stripped, re.ASCII):
        return int(stripped)
    order = [unit for unit, _ in UNITS]
    sizes = dict(UNITS)
    total, position, last = 0, 0, -1
    while position < len(stripped):
        match = _COMPONENT.match(stripped, position)
        if not match:
            raise ValueError(f"invalid duration: {text!r}")
        unit = match.group(2).lower()
        index = order.index(unit)
        if index <= last:
            raise ValueError(f"units out of order or repeated: {text!r}")
        last = index
        total += int(match.group(1)) * sizes[unit]
        position = match.end()
        spaces = re.match(r"\s*", stripped[position:])
        position += spaces.end()
    return total


def format_duration(seconds):
    if isinstance(seconds, bool) or not isinstance(seconds, int):
        raise TypeError("seconds must be an int")
    if seconds < 0:
        raise ValueError("seconds must be non-negative")
    if seconds == 0:
        return "0s"
    parts = []
    for unit, size in UNITS:
        count, seconds = divmod(seconds, size)
        if count:
            parts.append(f"{count}{unit}")
    return "".join(parts)
PY
