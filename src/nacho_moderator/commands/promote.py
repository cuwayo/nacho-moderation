"""Administrator-management command handlers: ``/promote`` and ``/demote``.

Both go through ``promoteChatMember``. ``/promote`` grants only a limited,
explicitly safe set of rights (delete messages, restrict members, pin messages)
and never ``can_promote_members``, ``can_change_info`` or ``can_manage_chat``;
an optional custom title is length-limited and sent through a separate
``setChatAdministratorCustomTitle`` call (Bot API 9.3 moved it out of
``promoteChatMember``). ``/demote`` clears every right.

Telegram rejects promoting/demoting the chat owner, the bot itself, or an
administrator the bot did not promote; those cases are refused locally with a
clear message rather than retried.
"""

from __future__ import annotations

from aiogram.exceptions import TelegramAPIError

from nacho_moderator.commands.context import CommandContext
from nacho_moderator.constants import TELEGRAM_MAX_ADMIN_TITLE_LEN, Role
from nacho_moderator.logging_setup import get_logger
from nacho_moderator.models import html_escape

_log = get_logger("cmd_promote")

_NO_TARGET = "Reply to a user, or give an @username or numeric user id."
_OWNER = "I cannot change the owner's rights."
_SELF = "I cannot change my own rights."
_NOT_MINE = "I can only demote administrators I promoted myself; Telegram rejects the rest."
_TELEGRAM_REJECTED = "Telegram rejected that change."

#: The only rights /promote ever grants. Deliberately excludes
#: can_promote_members, can_change_info and can_manage_chat.
_PROMOTION_RIGHTS: dict[str, bool] = {
    "can_delete_messages": True,
    "can_restrict_members": True,
    "can_pin_messages": True,
    "can_manage_chat": False,
    "can_change_info": False,
    "can_promote_members": False,
    "can_invite_users": False,
    "can_manage_video_chats": False,
    "can_manage_topics": False,
    "can_manage_direct_messages": False,
    "can_manage_tags": False,
    "can_post_messages": False,
    "can_edit_messages": False,
    "can_delete_stories": False,
    "can_post_stories": False,
    "can_edit_stories": False,
    "can_send_welcome_messages": False,
}

#: The rights /demote clears (all of them).
_DEMOTION_RIGHTS: dict[str, bool] = dict.fromkeys(_PROMOTION_RIGHTS, False)


def parse_title(args: str) -> str:
    """Return the optional custom title from a ``[target] [title]`` tail.

    Args:
        args: The argument text after the target has been consumed.

    Returns:
        The title, trimmed to the Telegram limit of 16 characters; empty when
        none was given.
    """
    return args.strip()[:TELEGRAM_MAX_ADMIN_TITLE_LEN]


def _target_role(ctx: CommandContext, user_id: int) -> Role | None:
    """Return the target's role from the cached admin map, or ``None``."""
    admins = ctx.admins or {}
    subject = admins.get(user_id)
    return subject.role if subject is not None else None


async def cmd_promote(ctx: CommandContext) -> None:
    """Grant a user the limited moderator rights, with an optional title."""
    if ctx.target_id is None:
        await ctx.reply(_NO_TARGET)
        return
    if ctx.target_id == ctx.bot_id:
        await ctx.reply(_SELF)
        return
    if _target_role(ctx, ctx.target_id) is Role.OWNER:
        await ctx.reply(_OWNER)
        return

    title = parse_title(ctx.invocation.args)
    try:
        await ctx.bot.promote_chat_member(
            chat_id=ctx.chat_id,
            user_id=ctx.target_id,
            **_PROMOTION_RIGHTS,
        )
        if title:
            await ctx.bot.set_chat_administrator_custom_title(
                chat_id=ctx.chat_id,
                user_id=ctx.target_id,
                custom_title=title,
            )
    except TelegramAPIError as exc:
        _log.warning("promote failed for %s: %s", ctx.target_id, exc)
        await ctx.reply(_TELEGRAM_REJECTED)
        return

    await ctx.record_action("promote", ctx.target_id, title, ctx.now(), None, ctx.target_label)
    if title:
        await ctx.reply_html(f"Promoted with title “{html_escape(title)}”.")
    else:
        await ctx.reply("Promoted.")


async def cmd_demote(ctx: CommandContext) -> None:
    """Remove every administrator right from a user."""
    if ctx.target_id is None:
        await ctx.reply(_NO_TARGET)
        return
    if ctx.target_id == ctx.bot_id:
        await ctx.reply(_SELF)
        return
    if _target_role(ctx, ctx.target_id) is Role.OWNER:
        await ctx.reply(_OWNER)
        return

    try:
        await ctx.bot.promote_chat_member(
            chat_id=ctx.chat_id,
            user_id=ctx.target_id,
            **_DEMOTION_RIGHTS,
        )
    except TelegramAPIError as exc:
        _log.warning("demote failed for %s: %s", ctx.target_id, exc)
        await ctx.reply(_NOT_MINE)
        return

    await ctx.record_action("demote", ctx.target_id, "", ctx.now(), None, ctx.target_label)
    await ctx.reply("Demoted.")
