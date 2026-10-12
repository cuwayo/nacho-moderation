"""SQLite connection, migrations, and typed repository methods.

All queries use bound parameters; no SQL is ever built by string concatenation.
Timestamps are Unix seconds (integers) so they survive restarts, matching the
original schema's absolute ``expires_at`` semantics.
"""

from __future__ import annotations

from dataclasses import dataclass
from importlib import resources
from pathlib import Path

import aiosqlite

from nacho_moderator.constants import (
    DEFAULT_CAPTCHA_TIMEOUT,
    DEFAULT_FLOOD_WINDOW,
    DEFAULT_WARN_ACTION,
    DEFAULT_WARN_LIMIT,
)
from nacho_moderator.errors import DatabaseError
from nacho_moderator.logging_setup import get_logger

_log = get_logger("db")

SCHEMA_VERSION = 1

_PRAGMAS = (
    "PRAGMA journal_mode=WAL;",
    "PRAGMA synchronous=NORMAL;",
    "PRAGMA foreign_keys=ON;",
    "PRAGMA busy_timeout=5000;",
)


@dataclass(frozen=True)
class ChatSettings:
    """A chat's moderation policy."""

    chat_id: int
    warn_limit: int = DEFAULT_WARN_LIMIT
    warn_action: str = DEFAULT_WARN_ACTION
    welcome_enabled: bool = False
    welcome_text: str = ""
    rules_text: str = ""
    log_chat_id: int = 0
    captcha_enabled: bool = False
    captcha_timeout: int = DEFAULT_CAPTCHA_TIMEOUT
    flood_limit: int = 0
    flood_window: int = DEFAULT_FLOOD_WINDOW
    locks: int = 0


@dataclass(frozen=True)
class WarnRow:
    """One stored warning."""

    id: int
    admin_id: int
    created_at: int
    reason: str


@dataclass(frozen=True)
class ActionRow:
    """One recorded moderation action that can expire."""

    id: int
    chat_id: int
    user_id: int
    action: str


@dataclass(frozen=True)
class BlacklistEntry:
    """One blacklist pattern."""

    pattern: str
    is_regex: bool


@dataclass(frozen=True)
class PendingCaptcha:
    """One pending join-verification challenge."""

    chat_id: int
    user_id: int
    message_id: int
    nonce: str
    deadline: int


class Database:
    """An open SQLite database with repository methods.

    Instances are created via :meth:`connect` and closed with :meth:`close`.
    """

    def __init__(self, conn: aiosqlite.Connection) -> None:
        self._conn = conn

    @classmethod
    async def connect(cls, path: str) -> Database:
        """Open (creating if needed) the database at ``path`` and migrate it.

        Args:
            path: The SQLite file path, or ``:memory:`` for an in-memory database.

        Returns:
            A connected, migrated :class:`Database`.

        Raises:
            DatabaseError: If the database cannot be opened or prepared. The
                connection is closed before the error propagates, so no
                aiosqlite worker thread is left behind.
        """
        if path != ":memory:":
            Path(path).parent.mkdir(parents=True, exist_ok=True)
        try:
            conn = await aiosqlite.connect(path)
        except aiosqlite.Error as exc:
            raise DatabaseError(f"cannot open database {path}: {exc}") from exc
        conn.row_factory = aiosqlite.Row
        db = cls(conn)
        try:
            await db._apply_pragmas()
            await db._migrate()
        except Exception:
            # Do not leak the connection (and its worker thread) on a failed setup.
            await conn.close()
            raise
        return db

    async def close(self) -> None:
        """Close the underlying connection."""
        await self._conn.close()

    async def _apply_pragmas(self) -> None:
        for pragma in _PRAGMAS:
            await self._conn.execute(pragma)
        await self._conn.commit()

    async def _migrate(self) -> None:
        """Apply the schema, tracking the version in ``schema_version``."""
        await self._conn.execute("CREATE TABLE IF NOT EXISTS schema_version (v INTEGER);")
        async with self._conn.execute("SELECT v FROM schema_version LIMIT 1;") as cur:
            row = await cur.fetchone()
        version = int(row["v"]) if row is not None else 0
        if version >= SCHEMA_VERSION:
            return
        schema = resources.files("nacho_moderator.db").joinpath("schema.sql").read_text("utf-8")
        await self._conn.executescript(schema)
        await self._conn.execute("DELETE FROM schema_version;")
        await self._conn.execute("INSERT INTO schema_version(v) VALUES(?);", (SCHEMA_VERSION,))
        await self._conn.commit()
        _log.info("database migrated to schema v%d", SCHEMA_VERSION)

    # --- users / chats -----------------------------------------------------

    async def upsert_user(
        self, user_id: int, username: str | None, first_name: str | None, now: int
    ) -> None:
        """Insert or update a user record, refreshing ``last_seen``."""
        await self._conn.execute(
            "INSERT INTO users(user_id, username, first_name, last_seen) VALUES(?, ?, ?, ?) "
            "ON CONFLICT(user_id) DO UPDATE SET username=excluded.username, "
            "first_name=excluded.first_name, last_seen=excluded.last_seen",
            (user_id, username, first_name, now),
        )
        await self._conn.commit()

    async def upsert_chat(self, chat_id: int, title: str | None, chat_type: str | None) -> None:
        """Insert or update a chat record."""
        await self._conn.execute(
            "INSERT INTO chats(chat_id, title, type) VALUES(?, ?, ?) "
            "ON CONFLICT(chat_id) DO UPDATE SET title=excluded.title, type=excluded.type",
            (chat_id, title, chat_type),
        )
        await self._conn.commit()

    async def find_user_by_username(self, username: str) -> int | None:
        """Resolve a username (case-insensitive) to a user id, if known."""
        async with self._conn.execute(
            "SELECT user_id FROM users WHERE username=? COLLATE NOCASE LIMIT 1", (username,)
        ) as cur:
            row = await cur.fetchone()
        return int(row["user_id"]) if row is not None else None

    # --- settings ----------------------------------------------------------

    async def get_settings(self, chat_id: int) -> ChatSettings:
        """Return a chat's settings, or the defaults when no row exists."""
        async with self._conn.execute(
            "SELECT warn_limit, warn_action, welcome_enabled, welcome_text, rules_text, "
            "log_chat_id, captcha_enabled, captcha_timeout, flood_limit, flood_window, locks "
            "FROM chats WHERE chat_id=?",
            (chat_id,),
        ) as cur:
            row = await cur.fetchone()
        if row is None:
            return ChatSettings(chat_id=chat_id)
        return ChatSettings(
            chat_id=chat_id,
            warn_limit=int(row["warn_limit"]),
            warn_action=str(row["warn_action"]),
            welcome_enabled=bool(row["welcome_enabled"]),
            welcome_text=row["welcome_text"] or "",
            rules_text=row["rules_text"] or "",
            log_chat_id=int(row["log_chat_id"] or 0),
            captcha_enabled=bool(row["captcha_enabled"]),
            captcha_timeout=int(row["captcha_timeout"]),
            flood_limit=int(row["flood_limit"]),
            flood_window=int(row["flood_window"]),
            locks=int(row["locks"]),
        )

    async def set_setting(self, chat_id: int, column: str, value: int | str) -> None:
        """Set one settings column via an upsert.

        Args:
            chat_id: The chat to update.
            column: The column name; validated against an allowlist.
            value: The new value.
        """
        allowed = {
            "warn_limit",
            "warn_action",
            "welcome_enabled",
            "welcome_text",
            "rules_text",
            "log_chat_id",
            "captcha_enabled",
            "captcha_timeout",
            "flood_limit",
            "flood_window",
            "locks",
        }
        if column not in allowed:  # pragma: no cover - defensive
            raise DatabaseError(f"unknown settings column: {column}")
        await self._conn.execute(
            f"INSERT INTO chats(chat_id, {column}) VALUES(?, ?) "
            f"ON CONFLICT(chat_id) DO UPDATE SET {column}=excluded.{column}",
            (chat_id, value),
        )
        await self._conn.commit()

    # --- warns -------------------------------------------------------------

    async def add_warn(
        self, chat_id: int, user_id: int, admin_id: int, reason: str, now: int
    ) -> int:
        """Insert a warning and return the user's new warning count."""
        await self._conn.execute(
            "INSERT INTO warns(chat_id, user_id, admin_id, reason, created_at) "
            "VALUES(?, ?, ?, ?, ?)",
            (chat_id, user_id, admin_id, reason, now),
        )
        await self._conn.commit()
        return await self.count_warns(chat_id, user_id)

    async def count_warns(self, chat_id: int, user_id: int) -> int:
        """Count a user's warnings in a chat."""
        async with self._conn.execute(
            "SELECT count(*) AS n FROM warns WHERE chat_id=? AND user_id=?", (chat_id, user_id)
        ) as cur:
            row = await cur.fetchone()
        return int(row["n"]) if row is not None else 0

    async def remove_last_warn(self, chat_id: int, user_id: int) -> bool:
        """Delete the newest warning; return whether a row was removed."""
        cur = await self._conn.execute(
            "DELETE FROM warns WHERE id=(SELECT id FROM warns WHERE chat_id=? AND user_id=? "
            "ORDER BY id DESC LIMIT 1)",
            (chat_id, user_id),
        )
        await self._conn.commit()
        return cur.rowcount > 0

    async def reset_warns(self, chat_id: int, user_id: int) -> None:
        """Delete every warning for a user in a chat."""
        await self._conn.execute(
            "DELETE FROM warns WHERE chat_id=? AND user_id=?", (chat_id, user_id)
        )
        await self._conn.commit()

    async def list_warns(self, chat_id: int, user_id: int, limit: int) -> list[WarnRow]:
        """List a user's warnings, newest first, up to ``limit``."""
        async with self._conn.execute(
            "SELECT id, admin_id, created_at, reason FROM warns "
            "WHERE chat_id=? AND user_id=? ORDER BY id DESC LIMIT ?",
            (chat_id, user_id, limit),
        ) as cur:
            rows = await cur.fetchall()
        return [
            WarnRow(
                id=int(r["id"]),
                admin_id=int(r["admin_id"]),
                created_at=int(r["created_at"]),
                reason=r["reason"] or "",
            )
            for r in rows
        ]

    # --- actions / expiry --------------------------------------------------

    async def record_action(
        self,
        chat_id: int,
        user_id: int,
        admin_id: int,
        action: str,
        reason: str,
        created_at: int,
        expires_at: int | None,
    ) -> None:
        """Insert a moderation action row (``expires_at`` NULL = permanent)."""
        await self._conn.execute(
            "INSERT INTO actions(chat_id, user_id, admin_id, action, reason, created_at, "
            "expires_at) VALUES(?, ?, ?, ?, ?, ?, ?)",
            (chat_id, user_id, admin_id, action, reason, created_at, expires_at),
        )
        await self._conn.commit()

    async def due_actions(self, now: int, limit: int) -> list[ActionRow]:
        """Return un-lifted expiring actions whose deadline has passed."""
        async with self._conn.execute(
            "SELECT id, chat_id, user_id, action FROM actions "
            "WHERE lifted=0 AND expires_at IS NOT NULL AND expires_at > 0 AND expires_at <= ? "
            "ORDER BY expires_at LIMIT ?",
            (now, limit),
        ) as cur:
            rows = await cur.fetchall()
        return [
            ActionRow(
                id=int(r["id"]),
                chat_id=int(r["chat_id"]),
                user_id=int(r["user_id"]),
                action=str(r["action"]),
            )
            for r in rows
        ]

    async def lift_action(self, action_id: int) -> None:
        """Mark an action row as lifted so it is never applied twice."""
        await self._conn.execute("UPDATE actions SET lifted=1 WHERE id=?", (action_id,))
        await self._conn.commit()

    # --- blacklist ---------------------------------------------------------

    async def blacklist_add(self, chat_id: int, pattern: str, is_regex: bool) -> None:
        """Insert or update a blacklist pattern."""
        await self._conn.execute(
            "INSERT INTO blacklist(chat_id, pattern, is_regex) VALUES(?, ?, ?) "
            "ON CONFLICT(chat_id, pattern) DO UPDATE SET is_regex=excluded.is_regex",
            (chat_id, pattern, int(is_regex)),
        )
        await self._conn.commit()

    async def blacklist_remove(self, chat_id: int, pattern: str) -> bool:
        """Delete a blacklist pattern; return whether a row was removed."""
        cur = await self._conn.execute(
            "DELETE FROM blacklist WHERE chat_id=? AND pattern=?", (chat_id, pattern)
        )
        await self._conn.commit()
        return cur.rowcount > 0

    async def blacklist_list(self, chat_id: int, limit: int) -> list[BlacklistEntry]:
        """List a chat's blacklist patterns, ordered by pattern, up to ``limit``."""
        async with self._conn.execute(
            "SELECT pattern, is_regex FROM blacklist WHERE chat_id=? ORDER BY pattern LIMIT ?",
            (chat_id, limit),
        ) as cur:
            rows = await cur.fetchall()
        return [
            BlacklistEntry(pattern=str(r["pattern"]), is_regex=bool(r["is_regex"])) for r in rows
        ]

    # --- captcha -----------------------------------------------------------

    async def captcha_put(
        self, chat_id: int, user_id: int, message_id: int, nonce: str, deadline: int
    ) -> None:
        """Insert or replace a pending captcha row."""
        await self._conn.execute(
            "INSERT INTO pending_captcha(chat_id, user_id, message_id, nonce, deadline) "
            "VALUES(?, ?, ?, ?, ?) ON CONFLICT(chat_id, user_id) DO UPDATE SET "
            "message_id=excluded.message_id, nonce=excluded.nonce, deadline=excluded.deadline",
            (chat_id, user_id, message_id, nonce, deadline),
        )
        await self._conn.commit()

    async def captcha_get(self, chat_id: int, user_id: int) -> PendingCaptcha | None:
        """Read one pending captcha row, if present."""
        async with self._conn.execute(
            "SELECT message_id, nonce, deadline FROM pending_captcha WHERE chat_id=? AND user_id=?",
            (chat_id, user_id),
        ) as cur:
            row = await cur.fetchone()
        if row is None:
            return None
        return PendingCaptcha(
            chat_id=chat_id,
            user_id=user_id,
            message_id=int(row["message_id"]),
            nonce=str(row["nonce"]),
            deadline=int(row["deadline"]),
        )

    async def captcha_delete(self, chat_id: int, user_id: int) -> None:
        """Delete one pending captcha row."""
        await self._conn.execute(
            "DELETE FROM pending_captcha WHERE chat_id=? AND user_id=?", (chat_id, user_id)
        )
        await self._conn.commit()

    async def captcha_due(self, now: int, limit: int) -> list[PendingCaptcha]:
        """Return pending captcha rows whose deadline has passed."""
        async with self._conn.execute(
            "SELECT chat_id, user_id, message_id, nonce, deadline FROM pending_captcha "
            "WHERE deadline <= ? ORDER BY deadline LIMIT ?",
            (now, limit),
        ) as cur:
            rows = await cur.fetchall()
        return [
            PendingCaptcha(
                chat_id=int(r["chat_id"]),
                user_id=int(r["user_id"]),
                message_id=int(r["message_id"]),
                nonce=str(r["nonce"]),
                deadline=int(r["deadline"]),
            )
            for r in rows
        ]
