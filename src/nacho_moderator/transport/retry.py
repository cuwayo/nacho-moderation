"""Retry policy: which methods are idempotent, and the backoff schedule.

Mirrors the original ``api_methods.def``: each Bot API method maps to a
rate-limit bucket and an idempotency flag. Only idempotent methods are retried
after a 5xx or network error, with an exponential backoff capped at 30 seconds
and a maximum of 5 retries. HTTP 429 is always retried (after the pause), and
non-429 4xx errors are never retried.
"""

from __future__ import annotations

from nacho_moderator.constants import (
    BACKOFF_SCHEDULE_SECONDS,
    MAX_RETRIES,
    Bucket,
)

# method name -> (bucket, idempotent)
_METHODS: dict[str, tuple[Bucket, bool]] = {
    "getMe": (Bucket.GLOBAL, True),
    "getUpdates": (Bucket.GLOBAL, True),
    "getChatAdministrators": (Bucket.GLOBAL, True),
    "setMyCommands": (Bucket.GLOBAL, True),
    "getChatMemberCount": (Bucket.GLOBAL, True),
    "answerCallbackQuery": (Bucket.GLOBAL, True),
    "sendMessage": (Bucket.PER_CHAT, False),
    "editMessageText": (Bucket.PER_CHAT, False),
    "banChatMember": (Bucket.BULK, True),
    "unbanChatMember": (Bucket.BULK, True),
    "restrictChatMember": (Bucket.BULK, True),
    "deleteMessage": (Bucket.BULK, True),
    "deleteMessages": (Bucket.BULK, True),
    "pinChatMessage": (Bucket.BULK, True),
    "unpinChatMessage": (Bucket.BULK, True),
}

_DEFAULT = (Bucket.GLOBAL, False)


def bucket_for(method: str) -> Bucket:
    """Return the rate-limit bucket for a Bot API method name."""
    return _METHODS.get(method, _DEFAULT)[0]


def is_idempotent(method: str) -> bool:
    """Return whether a method is safe to retry after a transient failure."""
    return _METHODS.get(method, _DEFAULT)[1]


def backoff_seconds(attempt: int) -> int:
    """Return the backoff delay for a zero-based retry attempt.

    The schedule is 1, 2, 4, 8, 16, then a 30-second cap.
    """
    if attempt < len(BACKOFF_SCHEDULE_SECONDS):
        return BACKOFF_SCHEDULE_SECONDS[attempt]
    return BACKOFF_SCHEDULE_SECONDS[-1]


def has_budget(attempt: int) -> bool:
    """Return whether another retry is allowed at ``attempt`` (0-based)."""
    return attempt < MAX_RETRIES
