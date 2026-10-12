"""Moderation command handlers.

``/ban /unban /kick /mute /unmute`` share one issuance path that builds the
Telegram call, records the action, and replies with a past-tense verb.
``/del /pin /unpin /purge`` manage replied-to messages.
"""

from __future__ import annotations

from aiogram.types import ChatPermissions

from nacho_moderator.commands.args import parse_duration_reason
from nacho_moderator.commands.context import CommandContext
from nacho_moderator.constants import DELETE_BATCH_SIZE, PURGE_MAX_IDS
from nacho_moderator.logging_setup import get_logger

_log = get_logger("cmd_mod")

_MUTED = ChatPermissions(
    can_send_messages=False,
    can_send_audios=False,
    can_send_documents=False,
    can_send_photos=False,
    can_send_videos=False,
    can_send_video_notes=False,
    can_send_voice_notes=False,
    can_send_polls=False,
    can_send_other_messages=False,
    can_add_web_page_previews=False,
    can_change_info=False,
    can_invite_users=False,
    can_pin_messages=False,
)

_UNMUTED = ChatPermissions(
    can_send_messages=True,
    can_send_audios=True,
    can_send_documents=True,
    can_send_photos=True,
    can_send_videos=True,
    can_send_video_notes=True,
    can_send_voice_notes=True,
    can_send_polls=True,
    can_send_other_messages=True,
    can_add_web_page_previews=True,
    can_invite_users=True,
)

_CANNOT_ACT = "I cannot act on the owner, an administrator, or myself."


async def _deny_target(ctx: CommandContext) -> bool:
    """Reply when the target is missing; return ``True`` if the handler stops."""
    if ctx.target_id is None:
        await ctx.reply("Reply to a user, or give an @username or numeric user id.")
        return True
    return False


async def cmd_ban(ctx: CommandContext) -> None:
    """Ban a user, optionally for a duration."""
    if await _deny_target(ctx):
        return
    assert ctx.target_id is not None
    duration, reason = parse_duration_reason(ctx.invocation.args)
    until = _until(ctx, duration)
    await ctx.bot.ban_chat_member(
        chat_id=ctx.chat_id, user_id=ctx.target_id, until_date=until, revoke_messages=True
    )
    await ctx.record_action("ban", ctx.target_id, reason, _now(ctx), until, ctx.target_label)
    await ctx.reply("banned")


async def cmd_unban(ctx: CommandContext) -> None:
    """Lift a ban."""
    if await _deny_target(ctx):
        return
    assert ctx.target_id is not None
    await ctx.bot.unban_chat_member(chat_id=ctx.chat_id, user_id=ctx.target_id, only_if_banned=True)
    await ctx.record_action("unban", ctx.target_id, "", _now(ctx), None, ctx.target_label)
    await ctx.reply("unbanned")


async def cmd_kick(ctx: CommandContext) -> None:
    """Remove a user so they can rejoin (ban then unban)."""
    if await _deny_target(ctx):
        return
    assert ctx.target_id is not None
    _, reason = parse_duration_reason(ctx.invocation.args)
    await ctx.bot.ban_chat_member(chat_id=ctx.chat_id, user_id=ctx.target_id, revoke_messages=True)
    await ctx.bot.unban_chat_member(chat_id=ctx.chat_id, user_id=ctx.target_id, only_if_banned=True)
    await ctx.record_action("kick", ctx.target_id, reason, _now(ctx), None, ctx.target_label)
    await ctx.reply("kicked")


async def cmd_mute(ctx: CommandContext) -> None:
    """Restrict a user from sending messages, optionally for a duration."""
    if await _deny_target(ctx):
        return
    assert ctx.target_id is not None
    duration, reason = parse_duration_reason(ctx.invocation.args)
    until = _until(ctx, duration)
    await ctx.bot.restrict_chat_member(
        chat_id=ctx.chat_id,
        user_id=ctx.target_id,
        permissions=_MUTED,
        until_date=until,
        use_independent_chat_permissions=True,
    )
    await ctx.record_action("mute", ctx.target_id, reason, _now(ctx), until, ctx.target_label)
    await ctx.reply("muted")


async def cmd_unmute(ctx: CommandContext) -> None:
    """Restore a user's common group permissions."""
    if await _deny_target(ctx):
        return
    assert ctx.target_id is not None
    await ctx.bot.restrict_chat_member(
        chat_id=ctx.chat_id,
        user_id=ctx.target_id,
        permissions=_UNMUTED,
        use_independent_chat_permissions=True,
    )
    await ctx.record_action("unmute", ctx.target_id, "", _now(ctx), None, ctx.target_label)
    await ctx.reply("unmuted")


async def cmd_del(ctx: CommandContext) -> None:
    """Delete the replied-to message and the command message."""
    reply = ctx.message.reply_to_message
    if reply is None:
        await ctx.reply("Reply to a message to use this command.")
        return
    await ctx.bot.delete_message(chat_id=ctx.chat_id, message_id=reply.message_id)
    await ctx.bot.delete_message(chat_id=ctx.chat_id, message_id=ctx.message.message_id)


async def cmd_pin(ctx: CommandContext) -> None:
    """Pin the replied-to message."""
    reply = ctx.message.reply_to_message
    if reply is None:
        await ctx.reply("Reply to a message to use this command.")
        return
    await ctx.bot.pin_chat_message(
        chat_id=ctx.chat_id, message_id=reply.message_id, disable_notification=True
    )


async def cmd_unpin(ctx: CommandContext) -> None:
    """Unpin the most recent pinned message."""
    await ctx.bot.unpin_chat_message(chat_id=ctx.chat_id)


async def cmd_purge(ctx: CommandContext) -> None:
    """Delete every message from the replied-to message up to the command."""
    reply = ctx.message.reply_to_message
    if reply is None:
        await ctx.reply("Reply to a message to use this command.")
        return
    low = reply.message_id
    high = ctx.message.message_id
    ids = list(range(low, high + 1))[:PURGE_MAX_IDS]
    for start in range(0, len(ids), DELETE_BATCH_SIZE):
        batch = ids[start : start + DELETE_BATCH_SIZE]
        try:
            await ctx.bot.delete_messages(chat_id=ctx.chat_id, message_ids=batch)
        except Exception as exc:
            _log.warning("purge batch failed: %s", exc)
            await ctx.reply("That failed.")
            return


def _now(ctx: CommandContext) -> int:
    """Return the current wall-clock time from the context's clock."""
    return ctx.now()


def _until(ctx: CommandContext, duration: int | None) -> int | None:
    """Convert a duration into an absolute ``until_date``, or ``None``."""
    if not duration:
        return None
    return ctx.now() + duration
