"""Structured line logging to stderr.

The format is ``ts level module message`` and the bot token is redacted wherever
it could appear (a URL such as ``.../bot<token>/method``). Log verbosity is
controlled by :class:`~nacho_moderator.config.LogLevelName`.
"""

from __future__ import annotations

import logging
import re
import sys

from nacho_moderator.config import LogLevelName

_LEVELS = {
    LogLevelName.ERROR: logging.ERROR,
    LogLevelName.WARN: logging.WARNING,
    LogLevelName.INFO: logging.INFO,
    LogLevelName.DEBUG: logging.DEBUG,
}

# A token looks like "<digits>:<base64-ish>" and only ever appears in API URLs.
_TOKEN_RE = re.compile(r"\bbot\d+:[A-Za-z0-9_-]+\b")
_TOKEN_IN_URL_RE = re.compile(r"/bot\d+:[A-Za-z0-9_-]+/")


def redact(text: str) -> str:
    """Replace any bot-token-like substring with ``bot<redacted>``."""
    text = _TOKEN_IN_URL_RE.sub("/bot<redacted>/", text)
    return _TOKEN_RE.sub("bot<redacted>", text)


class _RedactingFilter(logging.Filter):
    """A logging filter that strips tokens from every record."""

    def filter(self, record: logging.LogRecord) -> bool:
        """Redact the rendered message and any string args."""
        record.msg = redact(str(record.msg))
        if record.args:
            record.args = tuple(redact(a) if isinstance(a, str) else a for a in record.args)
        return True


def configure_logging(level: LogLevelName) -> None:
    """Install the stderr handler and level for the whole process.

    Args:
        level: The desired verbosity.
    """
    handler = logging.StreamHandler(sys.stderr)
    handler.setFormatter(logging.Formatter("%(asctime)s %(levelname)s %(name)s %(message)s"))
    handler.addFilter(_RedactingFilter())

    root = logging.getLogger()
    root.handlers.clear()
    root.addHandler(handler)
    root.setLevel(_LEVELS[level])

    # aiogram logs every request at DEBUG; keep it at WARNING unless debugging.
    logging.getLogger("aiogram").setLevel(logging.WARNING)


def get_logger(name: str) -> logging.Logger:
    """Return a module logger under the ``nacho_moderator`` namespace."""
    return logging.getLogger(f"nacho_moderator.{name}")
