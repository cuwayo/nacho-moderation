"""Warning command handlers.

``/warn`` adds a warning and, at the configured limit, applies the chat's
``warn_action`` (ban, mute, or kick). ``/unwarn`` removes the newest warning,
``/warns`` lists warnings, and ``/resetwarns`` clears them. ``/setwarnlimit`` and
``/setwarnaction`` configure the policy.
"""

from __future__ import annotations

from nacho_moderator.commands.context import CommandContext
from nacho_moderator.constants import MAX_WARNS_PAGE, Role

_WARN_ACTIONS = {"ban", "mute", "kick"}


async def _deny_target(ctx: CommandContext) -> bool:
    """Reply when the target is missing; return ``True`` if the handler stops."""
    if ctx.target_id is None:
        await ctx.reply("Reply to a user, or give an @username or numeric user id.")
        return True
    return False


async def cmd_warn(ctx: CommandContext) -> None:
    """Add a warning; auto-act when the limit is reached."""
    if await _deny_target(ctx):
        return
    assert ctx.target_id is not None
    reason = ctx.invocation.args.strip()
    count = await ctx.db.add_warn(ctx.chat_id, ctx.target_id, ctx.caller.user_id, reason, ctx.now())
    label = ctx.target_label or "the user"
    settings = await ctx.settings.get(ctx.chat_id)
    if settings.warn_limit > 0 and count >= settings.warn_limit:
        await ctx.reply(
            f"Warning limit ({settings.warn_limit}) reached; applying '{settings.warn_action}'."
        )
        await _apply_warn_action(ctx, settings.warn_action)
        await ctx.db.reset_warns(ctx.chat_id, ctx.target_id)
    else:
        await ctx.reply(f"Warned {label} ({count}/{settings.warn_limit}).")


async def _apply_warn_action(ctx: CommandContext, action: str) -> None:
    """Apply the configured warn-limit action to the target."""
    assert ctx.target_id is not None
    if action == "ban":
        await ctx.bot.ban_chat_member(
            chat_id=ctx.chat_id, user_id=ctx.target_id, revoke_messages=True
        )
    elif action == "kick":
        await ctx.bot.ban_chat_member(
            chat_id=ctx.chat_id, user_id=ctx.target_id, revoke_messages=True
        )
        await ctx.bot.unban_chat_member(
            chat_id=ctx.chat_id, user_id=ctx.target_id, only_if_banned=True
        )
    else:  # mute
        from nacho_moderator.commands.moderation import _MUTED

        await ctx.bot.restrict_chat_member(
            chat_id=ctx.chat_id,
            user_id=ctx.target_id,
            permissions=_MUTED,
            use_independent_chat_permissions=True,
        )
    await ctx.record_action(action, ctx.target_id, "warn limit", ctx.now(), None, ctx.target_label)


async def cmd_unwarn(ctx: CommandContext) -> None:
    """Remove a user's newest warning."""
    if await _deny_target(ctx):
        return
    assert ctx.target_id is not None
    removed = await ctx.db.remove_last_warn(ctx.chat_id, ctx.target_id)
    await ctx.reply("Removed the newest warning." if removed else "That failed.")


async def cmd_warns(ctx: CommandContext) -> None:
    """List a user's warnings (your own, or another user's as an admin)."""
    target_id = ctx.target_id if ctx.target_id is not None else ctx.caller.user_id
    is_plain_member = ctx.caller.role is Role.ANYONE and not ctx.caller.is_sudo
    if target_id != ctx.caller.user_id and is_plain_member:
        await ctx.reply("Only administrators can list another user's warnings.")
        return
    rows = await ctx.db.list_warns(ctx.chat_id, target_id, MAX_WARNS_PAGE)
    if target_id == ctx.caller.user_id:
        header = f"You have {len(rows)} warning(s):"
    else:
        header = f"{ctx.target_label or 'the user'} has {len(rows)} warning(s):"
    lines = [header]
    for i, row in enumerate(rows, start=1):
        lines.append(f"{i}. {row.reason or '(no reason)'}")
    await ctx.reply("\n".join(lines))


async def cmd_resetwarns(ctx: CommandContext) -> None:
    """Clear all of a user's warnings."""
    if await _deny_target(ctx):
        return
    assert ctx.target_id is not None
    await ctx.db.reset_warns(ctx.chat_id, ctx.target_id)
    await ctx.reply("Cleared all warnings.")


async def cmd_setwarnlimit(ctx: CommandContext) -> None:
    """Set how many warnings trigger the auto-action (0 disables it)."""
    text = ctx.invocation.args.strip()
    if not text.isdigit():
        await ctx.reply("Usage: /setwarnlimit <n> (0 disables auto-action).")
        return
    limit = int(text)
    if limit > 1000:
        await ctx.reply("Usage: /setwarnlimit <n> (0 disables auto-action).")
        return
    await ctx.settings.set(ctx.chat_id, "warn_limit", limit)
    await ctx.reply(f"Warn limit set to {limit}.")


async def cmd_setwarnaction(ctx: CommandContext) -> None:
    """Set the action taken when the warning limit is reached."""
    action = ctx.invocation.args.strip().lower()
    if action not in _WARN_ACTIONS:
        await ctx.reply("Usage: /setwarnaction ban|mute|kick")
        return
    await ctx.settings.set(ctx.chat_id, "warn_action", action)
    await ctx.reply(f"Warn action set to '{action}'.")
