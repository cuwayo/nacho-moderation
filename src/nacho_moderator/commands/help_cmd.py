"""The ``/help`` command handler."""

from __future__ import annotations

from nacho_moderator.commands.context import CommandContext


async def cmd_help(ctx: CommandContext) -> None:
    """Reply with the commands the caller is allowed to use."""
    # Imported lazily: the registry imports this module to build its table.
    from nacho_moderator.commands.registry import render_help

    text = render_help(ctx.caller.role, is_group=ctx.is_group)
    await ctx.reply(text)
