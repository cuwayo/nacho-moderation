"""Tests for the moderation command handlers (ban/unban/kick/mute/unmute/del/pin/purge).

These call the handler functions directly with a fake bot and in-memory database,
asserting the calls *we* decide to make.
"""

from __future__ import annotations

from nacho_moderator.clock import FakeClock
from nacho_moderator.commands import moderation
from nacho_moderator.db import Database
from nacho_moderator.services.settings import SettingsService
from tests.conftest import ADMIN_ID, TARGET_ID, FakeBot
from tests.helpers import context, message

NOW = int(FakeClock().now())


def _ctx(
    bot: FakeBot,
    db: Database,
    clock: FakeClock,
    *,
    args: str = "",
    target_id: int | None = TARGET_ID,
    reply_to=None,
):
    msg = message(11, text="/ban", user_id=ADMIN_ID, username="admin", reply_to=reply_to)
    return context(
        bot=bot,
        message=msg,
        args=args,
        target_id=target_id,
        target_label="spammer",
        db=db,
        clock=clock,
        settings=SettingsService(db=db, clock=clock),
    )


async def test_ban_permanent(fake_bot: FakeBot, db: Database, fake_clock: FakeClock) -> None:
    ctx = _ctx(fake_bot, db, fake_clock)
    await moderation.cmd_ban(ctx)
    calls = fake_bot.called("ban_chat_member")
    assert len(calls) == 1
    assert calls[0].kwargs["user_id"] == TARGET_ID
    assert calls[0].kwargs["until_date"] is None
    assert fake_bot.texts() == ["banned"]


async def test_ban_with_duration_sets_until(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    ctx = _ctx(fake_bot, db, fake_clock, args="10m spamming")
    await moderation.cmd_ban(ctx)
    call = fake_bot.called("ban_chat_member")[0]
    assert call.kwargs["until_date"] == int(fake_clock.now()) + 600


async def test_ban_records_action(fake_bot: FakeBot, db: Database, fake_clock: FakeClock) -> None:
    ctx = _ctx(fake_bot, db, fake_clock, args="1h reason")
    await moderation.cmd_ban(ctx)
    due = await db.due_actions(int(fake_clock.now()) + 3601, 10)
    assert len(due) == 1
    assert due[0].action == "ban"


async def test_ban_without_target_replies_usage(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    ctx = _ctx(fake_bot, db, fake_clock, target_id=None)
    await moderation.cmd_ban(ctx)
    assert fake_bot.called("ban_chat_member") == []
    assert "Reply to a user" in fake_bot.texts()[-1]


async def test_unban(fake_bot: FakeBot, db: Database, fake_clock: FakeClock) -> None:
    ctx = _ctx(fake_bot, db, fake_clock)
    await moderation.cmd_unban(ctx)
    assert fake_bot.called("unban_chat_member")[0].kwargs["only_if_banned"] is True
    assert fake_bot.texts() == ["unbanned"]


async def test_kick_bans_then_unbans(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    ctx = _ctx(fake_bot, db, fake_clock)
    await moderation.cmd_kick(ctx)
    assert fake_bot.names()[:2] == ["ban_chat_member", "unban_chat_member"]
    assert fake_bot.texts() == ["kicked"]


async def test_mute_sets_all_send_permissions_false(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    ctx = _ctx(fake_bot, db, fake_clock, args="5m")
    await moderation.cmd_mute(ctx)
    call = fake_bot.called("restrict_chat_member")[0]
    assert call.kwargs["permissions"].can_send_messages is False
    assert call.kwargs["until_date"] == int(fake_clock.now()) + 300


async def test_unmute_grants_permissions(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    ctx = _ctx(fake_bot, db, fake_clock)
    await moderation.cmd_unmute(ctx)
    call = fake_bot.called("restrict_chat_member")[0]
    assert call.kwargs["permissions"].can_send_messages is True


async def test_del_requires_reply(fake_bot: FakeBot, db: Database, fake_clock: FakeClock) -> None:
    ctx = _ctx(fake_bot, db, fake_clock)
    await moderation.cmd_del(ctx)
    assert fake_bot.called("delete_message") == []
    assert "Reply to a message" in fake_bot.texts()[-1]


async def test_del_deletes_reply_and_command(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    reply = message(10, text="spam", user_id=TARGET_ID)
    ctx = _ctx(fake_bot, db, fake_clock, reply_to=reply)
    await moderation.cmd_del(ctx)
    ids = {c.kwargs["message_id"] for c in fake_bot.called("delete_message")}
    assert ids == {10, 11}


async def test_pin(fake_bot: FakeBot, db: Database, fake_clock: FakeClock) -> None:
    reply = message(10, text="spam", user_id=TARGET_ID)
    ctx = _ctx(fake_bot, db, fake_clock, reply_to=reply)
    await moderation.cmd_pin(ctx)
    assert fake_bot.called("pin_chat_message")[0].kwargs["message_id"] == 10


async def test_unpin(fake_bot: FakeBot, db: Database, fake_clock: FakeClock) -> None:
    ctx = _ctx(fake_bot, db, fake_clock)
    await moderation.cmd_unpin(ctx)
    assert fake_bot.called("unpin_chat_message")


async def test_purge_batches(fake_bot: FakeBot, db: Database, fake_clock: FakeClock) -> None:
    reply = message(10, text="spam", user_id=TARGET_ID)
    ctx = _ctx(fake_bot, db, fake_clock, reply_to=reply)
    await moderation.cmd_purge(ctx)
    batches = fake_bot.called("delete_messages")
    assert len(batches) == 1
    assert batches[0].kwargs["message_ids"] == [10, 11]


async def test_purge_over_batch_size_splits(
    fake_bot: FakeBot, db: Database, fake_clock: FakeClock
) -> None:
    reply = message(1, text="spam", user_id=TARGET_ID)
    msg = message(250, text="/purge", user_id=ADMIN_ID, username="admin", reply_to=reply)
    ctx = context(
        bot=fake_bot,
        message=msg,
        target_id=None,
        db=db,
        clock=fake_clock,
        settings=SettingsService(db=db, clock=fake_clock),
    )
    await moderation.cmd_purge(ctx)
    batches = fake_bot.called("delete_messages")
    assert len(batches) == 3  # 250 ids -> 100 + 100 + 50
    assert sum(len(b.kwargs["message_ids"]) for b in batches) == 250
