"""Shared argument parsing for the target-taking commands.

A command's arguments are ``[target] [duration] [reason]``. The target token is
consumed by the router; this module handles the ``[duration] [reason]`` tail and
the target guard.
"""

from __future__ import annotations

from nacho_moderator.errors import ParseError
from nacho_moderator.models import parse_duration


def parse_duration_reason(args: str) -> tuple[int | None, str]:
    """Split an argument tail into an optional duration and a reason.

    The first whitespace-delimited token is treated as a duration only if it
    parses as one; otherwise the whole tail is the reason.

    Args:
        args: The argument text after the target.

    Returns:
        A ``(duration_seconds, reason)`` tuple; the duration is ``None`` when no
        leading duration token was present.
    """
    stripped = args.strip()
    if not stripped:
        return None, ""
    token, _, rest = stripped.partition(" ")
    if len(token) <= 31:
        try:
            return parse_duration(token), rest.strip()
        except ParseError:
            pass
    return None, stripped


class TargetError(Exception):
    """Raised when a target is missing or may not be acted upon."""


def require_target(target_id: int | None) -> int:
    """Return the resolved target id, or raise :class:`TargetError`."""
    if target_id is None:
        raise TargetError("Reply to a user, or give an @username or numeric user id.")
    return target_id
