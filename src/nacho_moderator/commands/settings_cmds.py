"""Settings, welcome, rules, log, and reload command handlers.

Also owns the ``/settings`` inline-keyboard panel: the callback payload scheme is
``set:<chat_id>:<user_id>:<key>`` and the only key is currently ``welcome``.
"""

from __future__ import annotations

from aiogram.types import InlineKeyboardButton, InlineKeyboardMarkup

from nacho_moderator.commands.context import CommandContext
from nacho_moderator.logging_setup import get_logger

_log = get_logger("cmd_settings")

SETTINGS_CB_PREFIX = "set:"
PANEL_TITLE = "Chat settings"


def parse_settings_callback(data: str) -> tuple[int, int, str] | None:
    """Parse a ``set:<chat_id>:<user_id>:<key>`` payload.

    Returns:
        A ``(chat_id, user_id, key)`` tuple, or ``None`` when the data is not a
        settings callback.
    """
    if not data.startswith(SETTINGS_CB_PREFIX):
        return None
    parts = data.split(":")
    if len(parts) != 4 or not parts[3]:
        return None
    try:
        return int(parts[1]), int(parts[2]), parts[3]
    except ValueError:
        return None


async def _panel_markup(ctx: CommandContext, welcome_enabled: bool) -> InlineKeyboardMarkup:
    label = "Welcome: ON" if welcome_enabled else "Welcome: OFF"
    data = f"{SETTINGS_CB_PREFIX}{ctx.chat_id}:{ctx.caller.user_id}:welcome"
    return InlineKeyboardMarkup(
        inline_keyboard=[[InlineKeyboardButton(text=label, callback_data=data)]]
    )


async def cmd_settings(ctx: CommandContext) -> None:
    """Open the inline settings panel."""
    settings = await ctx.settings.get(ctx.chat_id)
    markup = await _panel_markup(ctx, settings.welcome_enabled)
    await ctx.bot.send_message(chat_id=ctx.chat_id, text=PANEL_TITLE, reply_markup=markup)


async def render_panel_text(ctx: CommandContext) -> str:
    """Return the panel's message text (kept in one place for edits)."""
    return PANEL_TITLE


async def cmd_reload(ctx: CommandContext) -> None:
    """Invalidate the cached administrator list for this chat."""
    invalidate = ctx.invalidate_admins
    if callable(invalidate):
        invalidate(ctx.chat_id)
    await ctx.reply("Administrator list refreshed.")


async def cmd_setwelcome(ctx: CommandContext) -> None:
    """Store the join welcome message template."""
    text = ctx.invocation.args.strip()
    if not text:
        await ctx.reply("Usage: /setwelcome <text> ({first} {mention} {chat} {count})")
        return
    await ctx.settings.set(ctx.chat_id, "welcome_text", text)
    await ctx.reply("Welcome message saved. Use /welcome on to enable it.")


async def cmd_welcome(ctx: CommandContext) -> None:
    """Enable or disable the join welcome."""
    arg = ctx.invocation.args.strip().lower()
    if arg not in ("on", "off"):
        await ctx.reply("Usage: /welcome on|off")
        return
    await ctx.settings.set(ctx.chat_id, "welcome_enabled", 1 if arg == "on" else 0)
    await ctx.reply("Welcome enabled." if arg == "on" else "Welcome disabled.")


async def cmd_setrules(ctx: CommandContext) -> None:
    """Store the chat rules text."""
    text = ctx.invocation.args.strip()
    if not text:
        await ctx.reply("Usage: /setrules <text>")
        return
    await ctx.settings.set(ctx.chat_id, "rules_text", text)
    await ctx.reply("Rules saved.")


async def cmd_rules(ctx: CommandContext) -> None:
    """Show the stored chat rules."""
    settings = await ctx.settings.get(ctx.chat_id)
    if not settings.rules_text:
        await ctx.reply("No rules have been set for this chat.")
        return
    await ctx.reply(settings.rules_text)


async def cmd_setlog(ctx: CommandContext) -> None:
    """Set or clear the moderation log channel."""
    arg = ctx.invocation.args.strip().lower()
    if arg == "off":
        await ctx.settings.set(ctx.chat_id, "log_chat_id", 0)
        await ctx.reply("Moderation log disabled.")
        return
    try:
        chat_id = int(arg)
    except ValueError:
        await ctx.reply("Usage: /setlog <chat_id|off>")
        return
    await ctx.settings.set(ctx.chat_id, "log_chat_id", chat_id)
    await ctx.reply(f"Moderation log set to {chat_id}.")
