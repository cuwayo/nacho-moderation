"""Tests for /promote, /demote and /start (P4).

These call the handlers directly with a fake bot and assert the calls *we*
decide to make, then exercise the permission matrix and target guards through the
router.
"""

from __future__ import annotations

from aiogram.exceptions import TelegramBadRequest

from nacho_moderator.clock import FakeClock
from nacho_moderator.commands import help_cmd, promote
from nacho_moderator.constants import Right, Role
from nacho_moderator.db import Database
from nacho_moderator.services.perms import Subject
from nacho_moderator.services.settings import SettingsService
from tests.conftest import ADMIN_ID, BOT_ID, TARGET_ID, FakeBot
from tests.helpers import context, message

OWNER_ID = 100


def _ctx(
    bot: FakeBot,
    db: Database,
    clock: FakeClock,
    *,
    args: str = "",
    target_id: int | None = TARGET_ID,
    admins: dict[int, Subject] | None = None,
    role: Role = Role.ADMIN,
    rights: Right = Right.PROMOTE,
    is_group: bool = True,
):
    msg = message(11, text="/promote", user_id=ADMIN_ID, username="admin")
    return context(
        bot=bot,
        message=msg,
        args=args,
        target_id=target_id,
        target_label="user",
        caller=Subject(ADMIN_ID, role, rights),
        db=db,
        clock=clock,
        settings=SettingsService(db=db, clock=clock),
        admins=admins or {},
        bot_id=BOT_ID,
        is_group=is_group,
    )


# --- /promote --------------------------------------------------------------


async def test_promote_grants_only_limited_rights(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    ctx = _ctx(fake_bot, db, fake_clock)
    await promote.cmd_promote(ctx)
    call = fake_bot.called("promote_chat_member")[0]
    assert call.kwargs["can_delete_messages"] is True
    assert call.kwargs["can_restrict_members"] is True
    assert call.kwargs["can_pin_messages"] is True
    # The dangerous rights are never granted.
    assert call.kwargs["can_promote_members"] is False
    assert call.kwargs["can_change_info"] is False
    assert call.kwargs["can_manage_chat"] is False
    assert fake_bot.texts() == ["Promoted."]


async def test_promote_with_title_calls_custom_title(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    ctx = _ctx(fake_bot, db, fake_clock, args="Moderator")
    await promote.cmd_promote(ctx)
    title_call = fake_bot.called("set_chat_administrator_custom_title")[0]
    assert title_call.kwargs["custom_title"] == "Moderator"
    assert "Promoted with title" in fake_bot.texts()[-1]


async def test_promote_title_is_truncated(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    ctx = _ctx(fake_bot, db, fake_clock, args="a" * 40)
    await promote.cmd_promote(ctx)
    assert (
        fake_bot.called("set_chat_administrator_custom_title")[0].kwargs["custom_title"] == "a" * 16
    )


async def test_promote_title_is_html_escaped(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    ctx = _ctx(fake_bot, db, fake_clock, args="<b>bad</b>")
    await promote.cmd_promote(ctx)
    # The title sent to Telegram is raw; the echo is escaped.
    sent = fake_bot.called("set_chat_administrator_custom_title")[0].kwargs["custom_title"]
    assert sent == "<b>bad</b>"
    assert "&lt;b&gt;bad&lt;/b&gt;" in fake_bot.texts()[-1]


async def test_promote_without_target_replies_usage(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    ctx = _ctx(fake_bot, db, fake_clock, target_id=None)
    await promote.cmd_promote(ctx)
    assert fake_bot.called("promote_chat_member") == []
    assert "Reply to a user" in fake_bot.texts()[-1]


async def test_promote_refuses_bot_itself(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    ctx = _ctx(fake_bot, db, fake_clock, target_id=BOT_ID)
    await promote.cmd_promote(ctx)
    assert fake_bot.called("promote_chat_member") == []
    assert "my own rights" in fake_bot.texts()[-1]


async def test_promote_refuses_owner(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    admins = {OWNER_ID: Subject(OWNER_ID, Role.OWNER)}
    ctx = _ctx(fake_bot, db, fake_clock, target_id=OWNER_ID, admins=admins)
    await promote.cmd_promote(ctx)
    assert fake_bot.called("promote_chat_member") == []
    assert "owner" in fake_bot.texts()[-1]


async def _actions(db: Database, chat_id: int) -> list[str]:
    """Return the recorded action names for a chat, newest last."""
    async with db._conn.execute(
        "SELECT action FROM actions WHERE chat_id=? ORDER BY id", (chat_id,)
    ) as cur:
        rows = await cur.fetchall()
    return [str(r["action"]) for r in rows]


async def test_promote_records_action(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    ctx = _ctx(fake_bot, db, fake_clock)
    await promote.cmd_promote(ctx)
    assert "promote" in await _actions(db, ctx.chat_id)


async def test_promote_maps_telegram_error(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    fake_bot.fail = {"promote_chat_member"}
    fake_bot.error = TelegramBadRequest(method=None, message="CHAT_ADMIN_REQUIRED")  # type: ignore[arg-type]
    ctx = _ctx(fake_bot, db, fake_clock)
    await promote.cmd_promote(ctx)
    assert "Telegram rejected" in fake_bot.texts()[-1]


async def test_promote_records_when_only_the_title_fails(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    """A failed title must not lose the promotion or say the whole change failed."""
    fake_bot.fail = {"set_chat_administrator_custom_title"}
    fake_bot.error = TelegramBadRequest(method=None, message="TITLE_INVALID")  # type: ignore[arg-type]
    ctx = _ctx(fake_bot, db, fake_clock, args="Moderator")
    await promote.cmd_promote(ctx)
    # The promotion call still went out and is recorded.
    assert fake_bot.called("promote_chat_member")
    assert "promote" in await _actions(db, ctx.chat_id)
    # The reply says the promotion stuck but the title did not.
    assert fake_bot.texts()[-1] == "Promoted, but the title could not be set."
    assert "Telegram rejected" not in fake_bot.texts()[-1]


async def test_promote_title_failure_does_not_claim_the_title(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    """The success reply never claims a title that Telegram refused."""
    fake_bot.fail = {"set_chat_administrator_custom_title"}
    fake_bot.error = TelegramBadRequest(method=None, message="TITLE_INVALID")  # type: ignore[arg-type]
    ctx = _ctx(fake_bot, db, fake_clock, args="Moderator")
    await promote.cmd_promote(ctx)
    assert all("Promoted with title" not in text for text in fake_bot.texts())


# --- /demote ---------------------------------------------------------------


async def test_demote_clears_every_right(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    admins = {TARGET_ID: Subject(TARGET_ID, Role.ADMIN, Right.RESTRICT)}
    ctx = _ctx(fake_bot, db, fake_clock, admins=admins)
    await promote.cmd_demote(ctx)
    call = fake_bot.called("promote_chat_member")[0]
    assert call.kwargs["can_delete_messages"] is False
    assert call.kwargs["can_restrict_members"] is False
    assert call.kwargs["can_pin_messages"] is False
    assert call.kwargs["can_promote_members"] is False
    assert call.kwargs["can_change_info"] is False
    assert call.kwargs["can_manage_chat"] is False
    assert fake_bot.texts() == ["Demoted."]


async def test_demote_refuses_owner(fake_bot: FakeBot, db: Database, fake_clock: FakeClock) -> None:
    admins = {OWNER_ID: Subject(OWNER_ID, Role.OWNER)}
    ctx = _ctx(fake_bot, db, fake_clock, target_id=OWNER_ID, admins=admins)
    await promote.cmd_demote(ctx)
    assert fake_bot.called("promote_chat_member") == []
    assert "owner" in fake_bot.texts()[-1]


async def test_demote_refuses_bot_itself(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    ctx = _ctx(fake_bot, db, fake_clock, target_id=BOT_ID)
    await promote.cmd_demote(ctx)
    assert fake_bot.called("promote_chat_member") == []
    assert "my own rights" in fake_bot.texts()[-1]


async def test_demote_maps_telegram_rejection(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    """An admin the bot did not promote is rejected by Telegram; we say so."""
    fake_bot.fail = {"promote_chat_member"}
    fake_bot.error = TelegramBadRequest(method=None, message="CHAT_ADMIN_REQUIRED")  # type: ignore[arg-type]
    admins = {TARGET_ID: Subject(TARGET_ID, Role.ADMIN, Right.RESTRICT)}
    ctx = _ctx(fake_bot, db, fake_clock, admins=admins)
    await promote.cmd_demote(ctx)
    assert "only demote administrators I promoted" in fake_bot.texts()[-1]


async def test_demote_without_target_replies_usage(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    ctx = _ctx(fake_bot, db, fake_clock, target_id=None)
    await promote.cmd_demote(ctx)
    assert fake_bot.called("promote_chat_member") == []
    assert "Reply to a user" in fake_bot.texts()[-1]


async def test_demote_records_action(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    ctx = _ctx(fake_bot, db, fake_clock)
    await promote.cmd_demote(ctx)
    assert "demote" in await _actions(db, ctx.chat_id)


# --- /start ----------------------------------------------------------------


async def test_start_replies_like_help(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    ctx = _ctx(fake_bot, db, fake_clock)
    await help_cmd.cmd_start(ctx)
    text = fake_bot.texts()[-1]
    assert "Available commands:" in text
    assert "/ban" in text  # admin caller


async def test_start_works_in_private_chat(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    ctx = _ctx(fake_bot, db, fake_clock, is_group=False, role=Role.ANYONE, rights=Right.NONE)
    await help_cmd.cmd_start(ctx)
    text = fake_bot.texts()[-1]
    assert "/start" in text
    assert "/ban" not in text  # group-only command is hidden in private


def test_parse_title() -> None:
    assert promote.parse_title("") == ""
    assert promote.parse_title("  Moderator  ") == "Moderator"
    assert promote.parse_title("x" * 20) == "x" * 16
