"""Database access layer built on :mod:`aiosqlite`.

The bot keeps all persistent state in SQLite: chat settings, users, warnings,
moderation actions (including expiring temp bans/mutes), the blacklist, and
pending captcha rows. This package owns the schema, the migration runner, and
typed repository methods over that schema.
"""

from __future__ import annotations

from nacho_moderator.db.database import (
    ActionRow,
    BlacklistEntry,
    ChatSettings,
    Database,
    PendingCaptcha,
    WarnRow,
)

__all__ = [
    "ActionRow",
    "BlacklistEntry",
    "ChatSettings",
    "Database",
    "PendingCaptcha",
    "WarnRow",
]
