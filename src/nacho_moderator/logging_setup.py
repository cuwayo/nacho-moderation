"""Structured line logging to stderr with token/secret redaction.

The format is ``ts level module message``. Redaction happens in a custom
:class:`logging.Formatter` that rewrites the *final* rendered string, so it also
covers values that are only assembled when the record is formatted (a URL built
with ``%``-style args), the formatted exception and stack text, and every logger
that propagates to the root handler (``aiogram``, ``aiohttp``, ``asyncio``).

Secrets are matched two ways:

* by exact value, registered at startup through :func:`register_secret` (the
  configured token and webhook secret, plus the URL-quoted form and the part
  after the ``:`` of a token); and
* by shape, so a token that was never registered is still caught
  (``\\bbot\\d+:[A-Za-z0-9_-]+`` and a bare ``\\d{6,}:[A-Za-z0-9_-]{30,}``).

Log verbosity is controlled by :class:`~nacho_moderator.config.LogLevelName`.
"""

from __future__ import annotations

import logging
import re
import sys
from urllib.parse import quote

from nacho_moderator.config import LogLevelName

_LEVELS = {
    LogLevelName.ERROR: logging.ERROR,
    LogLevelName.WARN: logging.WARNING,
    LogLevelName.INFO: logging.INFO,
    LogLevelName.DEBUG: logging.DEBUG,
}

# A token looks like "<digits>:<base64-ish>"; it appears in API URLs and may be
# logged on its own. The bare form requires a long secret tail so ordinary
# "12:34" timestamps and short ids are not mangled.
_TOKEN_IN_URL_RE = re.compile(r"/bot\d+:[A-Za-z0-9_-]+/")
_TOKEN_RE = re.compile(r"\bbot\d+:[A-Za-z0-9_-]+")
_BARE_TOKEN_RE = re.compile(r"\b\d{6,}:[A-Za-z0-9_-]{30,}")

#: Minimum length a registered value must have before it is redacted. Short
#: strings (a one-character secret, an empty value) would match far too much.
_MIN_SECRET_LEN = 8

REDACTED = "<redacted>"

# Exact secret values registered at startup, longest first so that a value that
# contains another (a URL-quoted token vs. the raw token) is replaced whole.
_secrets: list[str] = []


def register_secret(value: str) -> None:
    """Register an exact secret value to redact from all log output.

    Values shorter than :data:`_MIN_SECRET_LEN` characters are ignored: they are
    too short to redact safely (they would match ordinary text). For a token of
    the form ``<id>:<secret>`` the part after the colon is registered too, as is
    the URL-quoted form of every candidate, so a token that reached a URL or a
    form body is still caught.

    Args:
        value: The secret to redact.
    """
    for candidate in _secret_candidates(value):
        if len(candidate) >= _MIN_SECRET_LEN and candidate not in _secrets:
            _secrets.append(candidate)
    # Longest first so a longer value is replaced before a shorter substring.
    _secrets.sort(key=len, reverse=True)


def _secret_candidates(value: str) -> list[str]:
    """Return the strings that should be redacted for one secret value."""
    if not value:
        return []
    candidates = [value]
    if ":" in value:
        tail = value.split(":", 1)[1]
        if tail:
            candidates.append(tail)
    for candidate in list(candidates):
        quoted = quote(candidate, safe="")
        if quoted != candidate:
            candidates.append(quoted)
    return candidates


def reset_secrets() -> None:
    """Forget every registered secret (used by tests)."""
    _secrets.clear()


def redact(text: str) -> str:
    """Replace any token or registered secret substring with ``<redacted>``.

    Registered exact values are replaced first, then the token-shaped patterns.

    Args:
        text: The string to sanitise.

    Returns:
        The string with secrets masked.
    """
    for secret in _secrets:
        if secret in text:
            text = text.replace(secret, REDACTED)
    text = _TOKEN_IN_URL_RE.sub("/bot<redacted>/", text)
    text = _TOKEN_RE.sub("bot<redacted>", text)
    return _BARE_TOKEN_RE.sub("bot<redacted>", text)


class RedactingFormatter(logging.Formatter):
    """A formatter that redacts the final rendered record, exceptions included.

    Redacting here — rather than mutating ``record.msg``/``record.args`` in a
    filter — means the value is only ever rewritten after ``%``-formatting, so
    the assembled ``.../bot<token>/getMe`` URL, the formatted traceback
    (``exc_info``), and ``stack_info`` are all covered, and a mapping passed as
    the sole argument is not corrupted.
    """

    def format(self, record: logging.LogRecord) -> str:
        """Render the record, then mask every secret in the result."""
        return redact(super().format(record))


def configure_logging(level: LogLevelName) -> None:
    """Install the stderr handler and level for the whole process.

    Args:
        level: The desired verbosity.
    """
    handler = logging.StreamHandler(sys.stderr)
    handler.setFormatter(RedactingFormatter("%(asctime)s %(levelname)s %(name)s %(message)s"))

    root = logging.getLogger()
    root.handlers.clear()
    root.addHandler(handler)
    root.setLevel(_LEVELS[level])

    # aiogram logs every request at DEBUG; keep it at WARNING unless debugging.
    logging.getLogger("aiogram").setLevel(logging.WARNING)


def get_logger(name: str) -> logging.Logger:
    """Return a module logger under the ``nacho_moderator`` namespace."""
    return logging.getLogger(f"nacho_moderator.{name}")
