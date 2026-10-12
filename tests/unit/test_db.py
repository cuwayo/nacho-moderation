"""Unit tests for the database layer: schema, warns, actions, blacklist, captcha."""

from __future__ import annotations

from pathlib import Path

import pytest

from nacho_moderator.db import Database
from nacho_moderator.errors import DatabaseError


async def test_connect_creates_parent_directory(tmp_path: Path) -> None:
    """A file path whose parent does not exist is created on connect."""
    db_path = tmp_path / "nested" / "sub" / "bot.db"
    db = await Database.connect(str(db_path))
    try:
        assert db_path.is_file()
    finally:
        await db.close()


async def test_migrate_is_idempotent(tmp_path: Path) -> None:
    """Re-opening an existing database does not re-run the migration."""
    db_path = tmp_path / "bot.db"
    first = await Database.connect(str(db_path))
    await first.close()
    second = await Database.connect(str(db_path))  # exercises the version>=SCHEMA_VERSION return
    try:
        assert (await second.get_settings(-1)).warn_limit == 3
    finally:
        await second.close()


async def test_connect_to_bad_path_raises(tmp_path: Path) -> None:
    """An unopenable path surfaces as a DatabaseError."""
    # A directory is not a valid SQLite file.
    bad = tmp_path / "adir"
    bad.mkdir()
    with pytest.raises(DatabaseError):
        await Database.connect(str(bad))


async def test_failed_migration_closes_the_connection(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """A setup failure must not leak the aiosqlite connection or its thread."""
    import aiosqlite

    opened: list[aiosqlite.Connection] = []
    real_connect = aiosqlite.connect

    async def tracking_connect(*args, **kwargs):
        conn = await real_connect(*args, **kwargs)
        opened.append(conn)
        return conn

    original = Database._migrate

    async def failing_migrate(self: Database) -> None:
        await original(self)
        raise RuntimeError("migration failed")

    monkeypatch.setattr(aiosqlite, "connect", tracking_connect)
    monkeypatch.setattr(Database, "_migrate", failing_migrate)
    with pytest.raises(RuntimeError):
        await Database.connect(str(tmp_path / "bot.db"))

    assert opened, "the connection was opened"
    # A closed aiosqlite connection rejects further use ("no active connection").
    with pytest.raises(ValueError, match="no active connection"):
        await opened[0].execute("SELECT 1")


async def test_upsert_chat_stores_title_and_type(db: Database) -> None:
    await db.upsert_chat(-100, "Testers", "supergroup")
    settings = await db.get_settings(-100)
    assert settings.chat_id == -100


async def test_schema_migrates_and_defaults(db: Database) -> None:
    settings = await db.get_settings(-100)
    assert settings.warn_limit == 3
    assert settings.warn_action == "mute"
    assert settings.captcha_timeout == 120
    assert settings.flood_window == 10
    assert settings.locks == 0


async def test_upsert_and_find_user(db: Database) -> None:
    await db.upsert_user(5, "Alice", "Al", 1000)
    assert await db.find_user_by_username("alice") == 5
    assert await db.find_user_by_username("ALICE") == 5
    assert await db.find_user_by_username("bob") is None


async def test_warn_add_count_remove_reset(db: Database) -> None:
    assert await db.add_warn(-100, 5, 2, "spam", 1000) == 1
    assert await db.add_warn(-100, 5, 2, "flood", 1001) == 2
    assert await db.count_warns(-100, 5) == 2
    rows = await db.list_warns(-100, 5, 16)
    assert [r.reason for r in rows] == ["flood", "spam"]  # newest first
    assert await db.remove_last_warn(-100, 5) is True
    assert await db.count_warns(-100, 5) == 1
    await db.reset_warns(-100, 5)
    assert await db.count_warns(-100, 5) == 0
    assert await db.remove_last_warn(-100, 5) is False


async def test_settings_set_and_read(db: Database) -> None:
    await db.set_setting(-100, "warn_limit", 7)
    await db.set_setting(-100, "welcome_enabled", 1)
    settings = await db.get_settings(-100)
    assert settings.warn_limit == 7
    assert settings.welcome_enabled is True


async def test_set_setting_rejects_unknown_column(db: Database) -> None:
    import pytest

    from nacho_moderator.errors import DatabaseError

    with pytest.raises(DatabaseError):
        await db.set_setting(-100, "not_a_column", 1)


async def test_expiring_actions(db: Database) -> None:
    await db.record_action(-100, 5, 2, "ban", "spam", 1000, 1060)
    await db.record_action(-100, 6, 2, "ban", "permanent", 1000, None)
    assert await db.due_actions(1059, 32) == []
    due = await db.due_actions(1060, 32)
    assert len(due) == 1
    assert due[0].action == "ban"
    await db.lift_action(due[0].id)
    assert await db.due_actions(2000, 32) == []


async def test_blacklist_crud(db: Database) -> None:
    await db.blacklist_add(-100, "badword", is_regex=False)
    await db.blacklist_add(-100, r"sp\w+", is_regex=True)
    entries = await db.blacklist_list(-100, 16)
    assert {e.pattern for e in entries} == {"badword", r"sp\w+"}
    assert await db.blacklist_remove(-100, "badword") is True
    assert await db.blacklist_remove(-100, "missing") is False


async def test_captcha_put_get_delete(db: Database) -> None:
    await db.captcha_put(-100, 5, 42, "abcd1234", 1200)
    row = await db.captcha_get(-100, 5)
    assert row is not None
    assert row.message_id == 42
    assert row.nonce == "abcd1234"
    await db.captcha_delete(-100, 5)
    assert await db.captcha_get(-100, 5) is None


async def test_captcha_due(db: Database) -> None:
    await db.captcha_put(-100, 5, 42, "aaaa", 1000)
    await db.captcha_put(-100, 6, 43, "bbbb", 2000)
    due = await db.captcha_due(1500, 16)
    assert [r.user_id for r in due] == [5]
