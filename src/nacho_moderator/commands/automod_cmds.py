"""Automod configuration commands.

``/lock`` and ``/unlock`` toggle the lock bitmask; ``/blacklist`` manages word
and regex patterns; ``/antiflood`` sets the sliding-window threshold; and
``/captcha`` toggles join verification.
"""

from __future__ import annotations

import regex

from nacho_moderator.commands.context import CommandContext
from nacho_moderator.constants import LOCK_NAMES, MAX_BLACKLIST_PATTERNS

_LOCK_HELP = (
    "Usage: /lock <type> [type...] "
    "(links, media, stickers, forwards, polls, invites, bots, commands)"
)


async def cmd_lock(ctx: CommandContext) -> None:
    """Lock one or more message types."""
    await _set_locks(ctx, locked=True)


async def cmd_unlock(ctx: CommandContext) -> None:
    """Unlock one or more message types."""
    await _set_locks(ctx, locked=False)


async def _set_locks(ctx: CommandContext, *, locked: bool) -> None:
    """Apply a lock/unlock request for each named type."""
    names = ctx.invocation.args.split()
    if not names:
        await ctx.reply(_LOCK_HELP)
        return
    settings = await ctx.settings.get(ctx.chat_id)
    locks = settings.locks
    for name in names:
        lock = LOCK_NAMES.get(name.lower())
        if lock is None:
            await ctx.reply(f"Unknown lock type '{name}'. See /help.")
            return
        if locked:
            locks |= 1 << int(lock)
        else:
            locks &= ~(1 << int(lock))
    await ctx.settings.set(ctx.chat_id, "locks", locks)
    await ctx.reply("Locked." if locked else "Unlocked.")


async def cmd_blacklist(ctx: CommandContext) -> None:
    """Manage the chat's word/regex blacklist."""
    parts = ctx.invocation.args.split(maxsplit=1)
    if not parts:
        await ctx.reply("Usage: /blacklist add|del <word|/regex/> | list")
        return
    sub = parts[0].lower()
    arg = parts[1].strip() if len(parts) > 1 else ""

    if sub == "list":
        entries = await ctx.db.blacklist_list(ctx.chat_id, MAX_BLACKLIST_PATTERNS)
        lines = [f"Blacklist ({len(entries)}):"]
        for i, entry in enumerate(entries, start=1):
            suffix = " (regex)" if entry.is_regex else ""
            lines.append(f"{i}. {entry.pattern}{suffix}")
        await ctx.reply("\n".join(lines))
        return

    if sub not in ("add", "del") or not arg:
        await ctx.reply("Usage: /blacklist add|del <word|/regex/> | list")
        return

    is_regex = arg.startswith("/") and arg.endswith("/") and len(arg) > 2
    pattern = arg[1:-1] if is_regex else arg
    if is_regex:
        try:
            regex.compile(pattern)
        except regex.error:
            await ctx.reply("That is not a valid regular expression.")
            return

    if sub == "add":
        await ctx.db.blacklist_add(ctx.chat_id, pattern, is_regex)
        _invalidate(ctx)
        await ctx.reply("Added to the blacklist.")
    else:
        removed = await ctx.db.blacklist_remove(ctx.chat_id, pattern)
        _invalidate(ctx)
        await ctx.reply("Removed." if removed else "That pattern is not on the blacklist.")


def _invalidate(ctx: CommandContext) -> None:
    """Drop the cached blacklist for the chat, if the service is reachable."""
    automod = getattr(ctx, "automod", None)
    if automod is not None:
        automod.invalidate_blacklist(ctx.chat_id)


async def cmd_antiflood(ctx: CommandContext) -> None:
    """Set the flood threshold, or turn antiflood off."""
    parts = ctx.invocation.args.split()
    if len(parts) == 1 and parts[0].lower() == "off":
        await ctx.settings.set(ctx.chat_id, "flood_limit", 0)
        await ctx.reply("Antiflood disabled.")
        return
    if len(parts) != 2 or not all(p.isdigit() for p in parts):
        await ctx.reply("Usage: /antiflood <messages> <seconds> | off")
        return
    limit, window = int(parts[0]), int(parts[1])
    if limit <= 0 or window <= 0:
        await ctx.reply("Usage: /antiflood <messages> <seconds> | off")
        return
    await ctx.settings.set(ctx.chat_id, "flood_limit", limit)
    await ctx.settings.set(ctx.chat_id, "flood_window", window)
    await ctx.reply(f"Antiflood: {limit} message(s) per {window} second(s).")


async def cmd_captcha(ctx: CommandContext) -> None:
    """Enable or disable join verification."""
    arg = ctx.invocation.args.strip().lower()
    if arg not in ("on", "off"):
        await ctx.reply("Usage: /captcha on|off")
        return
    await ctx.settings.set(ctx.chat_id, "captcha_enabled", 1 if arg == "on" else 0)
    await ctx.reply("Captcha enabled." if arg == "on" else "Captcha disabled.")
