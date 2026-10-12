"""Tests for CommandContext helpers: reply escaping, now(), and action recording."""

from __future__ import annotations

from nacho_moderator.db import Database
from tests.conftest import CHAT_ID, TARGET_ID, FakeBot
from tests.helpers import context, message


def _ctx(bot: FakeBot, db: Database, *, log_chat_id: int = 0, expires: bool = False):
    msg = message(11, text="/ban", user_id=1)
    return context(
        bot=bot,
        message=msg,
        target_id=TARGET_ID,
        db=db,
        clock=None,
        log_chat_id=log_chat_id,
    )


async def test_reply_escapes_user_text(fake_bot: FakeBot, db: Database) -> None:
    ctx = _ctx(fake_bot, db)
    await ctx.reply("<b>hi</b> & bye")
    assert fake_bot.texts()[-1] == "&lt;b&gt;hi&lt;/b&gt; &amp; bye"


async def test_reply_html_is_verbatim(fake_bot: FakeBot, db: Database) -> None:
    ctx = _ctx(fake_bot, db)
    await ctx.reply_html("<b>trusted</b>")
    assert fake_bot.texts()[-1] == "<b>trusted</b>"


async def test_record_action_writes_a_row(fake_bot: FakeBot, db: Database) -> None:
    ctx = _ctx(fake_bot, db)
    await ctx.record_action("ban", TARGET_ID, "spam", 1000, None)
    rows = await db.due_actions(10**12, 10)
    assert rows == []  # permanent (expires_at None) is never due


async def test_record_action_mirrors_to_log_channel(fake_bot: FakeBot, db: Database) -> None:
    ctx = _ctx(fake_bot, db, log_chat_id=-555)
    await ctx.record_action("ban", TARGET_ID, "spam <b>", 1000, 2000, target_label="Spammer")
    log_sends = [c for c in fake_bot.called("send_message") if c.kwargs.get("chat_id") == -555]
    assert log_sends
    text = log_sends[0].kwargs["text"]
    assert "action=ban" in text
    assert "until=2000" in text
    assert "&lt;b&gt;" in text  # reason escaped


async def test_record_action_survives_log_failure(fake_bot: FakeBot, db: Database) -> None:
    """A failing log mirror must not break the command."""
    fake_bot.fail = {"send_message"}
    ctx = _ctx(fake_bot, db, log_chat_id=-555)
    await ctx.record_action("ban", TARGET_ID, "spam", 1000, None)  # no raise


async def test_text_and_chat_id(fake_bot: FakeBot, db: Database) -> None:
    ctx = _ctx(fake_bot, db)
    assert ctx.text == "/ban"
    assert ctx.chat_id == CHAT_ID
