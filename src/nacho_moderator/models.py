"""Pure data helpers: duration parsing, HTML escaping, and target references.

These mirror the C ``util`` module's duration parser and HTML escaper, plus the
small value objects the router uses to describe a resolved command invocation.
"""

from __future__ import annotations

from dataclasses import dataclass

from nacho_moderator.errors import ParseError

# Duration suffix -> multiplier in seconds.
_DURATION_UNITS: dict[str, int] = {
    "s": 1,
    "m": 60,
    "h": 60 * 60,
    "d": 24 * 60 * 60,
    "w": 7 * 24 * 60 * 60,
}

# Largest value we accept before treating the input as an overflow.
_MAX_DURATION_SECONDS = 2**63 - 1


def parse_duration(text: str) -> int:
    """Parse a duration such as ``30s``, ``10m``, ``2h``, ``7d`` or ``1w``.

    A bare integer is interpreted as seconds; ``0`` is valid and means
    "permanent" to the callers. The value must start with a digit and may carry
    a single trailing unit letter (case-insensitive).

    Args:
        text: The token to parse.

    Returns:
        The duration in seconds.

    Raises:
        ParseError: If the token is empty, does not start with a digit, carries
            an unknown unit, has trailing characters, or overflows.
    """
    if not text or not text[0].isdigit():
        raise ParseError(f"invalid duration: {text!r}")

    unit = text[-1].lower()
    if unit in _DURATION_UNITS:
        digits = text[:-1]
        multiplier = _DURATION_UNITS[unit]
    else:
        digits = text
        multiplier = 1

    if not digits.isdigit():
        raise ParseError(f"invalid duration: {text!r}")

    value = int(digits)
    if value > _MAX_DURATION_SECONDS:
        raise ParseError(f"duration overflows: {text!r}")
    total = value * multiplier
    if total > _MAX_DURATION_SECONDS:
        raise ParseError(f"duration overflows: {text!r}")
    return total


def html_escape(text: str) -> str:
    """Escape the characters Telegram's HTML parse mode treats as markup.

    Only ``&``, ``<`` and ``>`` are escaped; UTF-8 passes through unchanged.
    """
    return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


@dataclass(frozen=True)
class TargetRef:
    """A command target token as written by the caller.

    Exactly one of ``user_id`` (a numeric id) or ``username`` (without the
    leading ``@``) is set.
    """

    user_id: int | None = None
    username: str | None = None

    @property
    def is_username(self) -> bool:
        """Return ``True`` if the target is an ``@username`` reference."""
        return self.username is not None
