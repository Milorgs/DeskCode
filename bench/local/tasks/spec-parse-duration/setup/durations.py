"""Human-friendly duration strings such as "1h30m"."""


def parse_duration(text):
    """Return the number of seconds described by ``text`` (see the task specification)."""
    raise NotImplementedError


def format_duration(seconds):
    """Return the canonical duration string for ``seconds``."""
    raise NotImplementedError
