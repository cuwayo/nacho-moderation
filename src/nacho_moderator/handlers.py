"""aiogram wiring: middlewares, message dispatch, joins, and callbacks.

The bot runs automod on every group message *before* command dispatch via an
outer middleware, which also resolves the caller's settings and permissions once
and stashes them in the handler data so the command handler does not refetch.
Join and leave updates drive the captcha flow and invalidate the admin cache;
callback queries are routed to the captcha and settings-panel handlers.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from aiogram import Bot, Dispatcher
from aiogram.enums import ChatType
from aiogram.types import (
    CallbackQuery,
    ChatMemberUpdated,
    Message,
    TelegramObject,
)

from nacho_moderator.clock import Clock
from nacho_moderator.commands.context import CommandContext, Invocation
from nacho_moderator.commands.registry import lookup, render_help
from nacho_moderator.commands.settings_cmds import parse_settings_callback
from nacho_moderator.config import Settings
from nacho_moderator.constants import CommandFlag, Right, Role
from nacho_moderator.db import Database
from nacho_moderator.errors import BotLacksRightError, PermissionDeniedError
from nacho_moderator.logging_setup import get_logger
from nacho_moderator.router import (
    check_permission,
    parse_command,
    resolve_target,
    target_is_protected,
)
from nacho_moderator.services.admin_actions import send_welcome_if_enabled
from nacho_moderator.services.automod import AutomodService, Verdict
from nacho_moderator.services.captcha import (
    CAPTCHA_CB_PREFIX,
    CaptchaService,
    parse_captcha_callback,
)
from nacho_moderator.services.expiry import CaptchaSweeper, ExpirySweeper
from nacho_moderator.services.perms import AdminCache, Subject, subject_for_user
from nacho_moderator.services.settings import SettingsService

_log = get_logger("handlers")

_DENIED = "You are not allowed to use that command."
_BOT_LACKS = "I lack the rights to do that in this chat."
_GROUP_ONLY = "That command is only available in groups."
_CANNOT_ACT = "I cannot act on the owner, an administrator, or myself."
_UNKNOWN_USERNAME = (
    "I do not know that username yet. Reply to one of their messages, or use their numeric user id."
)


@dataclass
class App:
    """The assembled bot: config, bot, dispatcher, storage, and services."""

    config: Settings
    bot: Bot
    dp: Dispatcher
    db: Database
    settings: SettingsService
    admins: AdminCache
    automod: AutomodService
    captcha: CaptchaService
    captcha_sweeper: CaptchaSweeper
    expiry: ExpirySweeper
    sudo_ids: set[int]
    clock: Clock
    bot_username: str | None = None
    bot_id: int = 0


class ContextMiddleware:
    """Resolve settings, permissions, and run automod before dispatch.

    Implemented as an aiogram outer message middleware so it runs before command
    filters; it stashes the resolved context in ``data`` for the command handler.
    """

    def __init__(self, app: App) -> None:
        self._app = app

    async def __call__(
        self,
        handler: Any,
        event: TelegramObject,
        data: dict[str, Any],
    ) -> Any:
        """Run automod, then continue to the handler unless the message is deleted.

        aiogram calls outer middlewares as ``middleware(handler, event, data)``
        with ``data`` a plain dict; the wrapped handler is called as
        ``handler(event, **data)``.
        """
        if not isinstance(event, Message) or event.from_user is None:
            return await handler(event, data)
        if event.chat.type not in (ChatType.GROUP, ChatType.SUPERGROUP):
            return await handler(event, data)

        app = self._app
        chat_id = event.chat.id
        try:
            settings = await app.settings.get(chat_id)
            admins = await app.admins.get(chat_id)
        except Exception as exc:
            _log.warning("context lookup failed for %s: %s", chat_id, exc)
            return await handler(event, data)

        caller = subject_for_user(event.from_user.id, admins, app.sudo_ids)
        data["settings"] = settings
        data["admins"] = admins
        data["caller"] = caller

        # Administrators, the owner, and sudo users are exempt from automod.
        if caller.role in (Role.OWNER, Role.ADMIN) or caller.is_sudo:
            return await handler(event, data)
        # Commands are dispatched by the command handler, not automod.
        text = event.text or ""
        if text.startswith("/"):
            return await handler(event, data)

        verdict = await self._scan(event, settings)
        if verdict.verdict is Verdict.DELETE:
            for message_id in verdict.delete_message_ids:
                await _safe_delete(app.bot, chat_id, message_id)
            await app.db.record_action(
                chat_id,
                event.from_user.id,
                app.bot_id,
                "lock",
                verdict.reason,
                int(app.clock.now()),
                None,
            )
            return None
        if verdict.verdict is Verdict.MUTE:
            from nacho_moderator.commands.moderation import _MUTED

            until = int(app.clock.now()) + verdict.mute_seconds
            await app.bot.restrict_chat_member(
                chat_id=chat_id,
                user_id=event.from_user.id,
                permissions=_MUTED,
                until_date=until,
                use_independent_chat_permissions=True,
            )
            for message_id in verdict.delete_message_ids:
                await _safe_delete(app.bot, chat_id, message_id)
            await app.db.record_action(
                chat_id,
                event.from_user.id,
                app.bot_id,
                "flood",
                verdict.reason,
                int(app.clock.now()),
                until,
            )
            return None
        return await handler(event, data)

    async def _scan(self, event: Message, settings: Any) -> Any:
        """Classify a message for the automod pipeline."""
        entity_types = {e.type for e in (event.entities or [])}
        has_media = any(
            getattr(event, attr, None) is not None
            for attr in ("photo", "video", "document", "audio", "voice", "video_note")
        )
        has_sticker = event.sticker is not None or event.animation is not None
        has_forward = event.forward_origin is not None
        has_poll = event.poll is not None
        body = event.text or event.caption or ""
        return await self._app.automod.scan(
            chat_id=event.chat.id,
            user_id=event.from_user.id if event.from_user else 0,
            message_id=event.message_id,
            text=body,
            locks=settings.locks,
            flood_limit=settings.flood_limit,
            flood_window=settings.flood_window,
            is_bot=bool(event.from_user and event.from_user.is_bot),
            has_media=has_media,
            has_sticker=has_sticker,
            has_forward=has_forward,
            has_poll=has_poll,
            entity_types=entity_types,
        )


async def _safe_delete(bot: Bot, chat_id: int, message_id: int) -> None:
    """Delete a message, ignoring failures (it may already be gone)."""
    try:
        await bot.delete_message(chat_id=chat_id, message_id=message_id)
    except Exception:
        _log.debug("could not delete message %s in %s", message_id, chat_id)


class MessageHandler:
    """Dispatch a group message to the matching command handler."""

    def __init__(self, app: App) -> None:
        self._app = app

    async def handle(self, message: Message, **data: Any) -> None:
        """Parse and run a command, applying permission and target guards."""
        app = self._app
        if message.from_user is None or message.from_user.is_bot:
            return
        text = message.text or ""
        if not text.startswith("/"):
            await self._maybe_welcome(app, message, data)
            return

        peek = parse_command(text, want_target=False, bot_username=app.bot_username)
        if peek is None or peek.addressed_to_other_bot:
            return
        spec = lookup(peek.name)
        if spec is None:
            return

        invocation = Invocation(name=spec.name)
        want_target = CommandFlag.TARGET in spec.flags
        parsed = parse_command(text, want_target=want_target, bot_username=app.bot_username)
        assert parsed is not None
        invocation.args = parsed.args
        invocation.target = parsed.target

        is_group = message.chat.type in (ChatType.GROUP, ChatType.SUPERGROUP)
        if CommandFlag.GROUP_ONLY in spec.flags and not is_group:
            await message.reply(_GROUP_ONLY)
            return

        caller: Subject = data.get("caller") or subject_for_user(
            message.from_user.id, {}, app.sudo_ids
        )
        admins = data.get("admins") or {}
        settings = data.get("settings") or await app.settings.get(message.chat.id)
        bot_subject = admins.get(app.bot_id, Subject(app.bot_id, Role.ADMIN, Right(0)))

        try:
            check_permission(spec, caller, bot_subject)
        except BotLacksRightError:
            await message.reply(_BOT_LACKS)
            return
        except PermissionDeniedError:
            await message.reply(_DENIED)
            return

        wants_target = CommandFlag.TARGET in spec.flags
        target_id = await resolve_target(message, parsed.target, admins, app.db)
        unresolved_username = (
            wants_target
            and target_id is None
            and parsed.target is not None
            and parsed.target.is_username
        )
        if unresolved_username:
            await message.reply(_UNKNOWN_USERNAME)
            return

        # The generic guard protects the owner, other admins, and the bot. A
        # command flagged ADMIN_TARGET (/demote) does its own, narrower guard
        # because it must be allowed to act on an administrator.
        if (
            wants_target
            and CommandFlag.ADMIN_TARGET not in spec.flags
            and target_id is not None
            and target_is_protected(target_id, admins, app.bot_id)
        ):
            await message.reply(_CANNOT_ACT)
            return

        ctx = CommandContext(
            bot=app.bot,
            db=app.db,
            settings=app.settings,
            clock=app.clock,
            message=message,
            chat_id=message.chat.id,
            invocation=invocation,
            caller=caller,
            target_id=target_id,
            target_label=_target_label(message),
            bot_username=app.bot_username,
            is_group=is_group,
            bot_id=app.bot_id,
            admins=admins,
            invalidate_admins=app.admins.invalidate,
            automod=app.automod,
            log_chat_id=settings.log_chat_id,
        )
        await spec.handler(ctx)

    async def _maybe_welcome(self, app: App, message: Message, data: dict[str, Any]) -> None:
        """Deliver the join welcome for legacy ``new_chat_members`` messages."""
        members = getattr(message, "new_chat_members", None)
        if not members:
            return
        settings = data.get("settings") or await app.settings.get(message.chat.id)
        for member in members:
            await send_welcome_if_enabled(app, message.chat.id, member, settings)


def _target_label(message: Message) -> str:
    """Derive a human-readable target label from a reply, if present."""
    reply = message.reply_to_message
    if reply is not None and reply.from_user is not None:
        user = reply.from_user
        return user.username or user.first_name or str(user.id)
    return ""


class JoinHandler:
    """Handle join/leave transitions: captcha, welcome, and cache invalidation."""

    def __init__(self, app: App) -> None:
        self._app = app

    async def handle(self, event: ChatMemberUpdated, **data: Any) -> None:
        """Invalidate the admin cache and, on a join, run the captcha or welcome."""
        app = self._app
        app.admins.invalidate(event.chat.id)
        old = event.old_chat_member.status
        new = event.new_chat_member.status
        joined = new in ("member", "administrator", "creator") and old not in (
            "member",
            "administrator",
            "creator",
        )
        if not joined:
            return
        settings = await app.settings.get(event.chat.id)
        user = event.new_chat_member.user
        if settings.captcha_enabled:
            await app.captcha.begin(event.chat.id, user.id, settings.captcha_timeout)
            return
        await send_welcome_if_enabled(app, event.chat.id, user, settings)


class CallbackHandler:
    """Route callback queries to the captcha and settings-panel handlers."""

    def __init__(self, app: App) -> None:
        self._app = app

    async def handle(self, query: CallbackQuery, **data: Any) -> None:
        """Handle a captcha press or a settings-panel press."""
        app = self._app
        data_str = query.data or ""
        if data_str.startswith(CAPTCHA_CB_PREFIX):
            callback = parse_captcha_callback(data_str)
            if callback is None:
                return
            answer = await app.captcha.verify(callback, query.from_user.id)
            await query.answer(answer)
            return

        parsed = parse_settings_callback(data_str)
        if parsed is None:
            return
        chat_id, owner_id, key = parsed
        if query.from_user.id != owner_id:
            await query.answer("This panel is not yours.")
            return
        if key != "welcome":
            await query.answer("Unknown setting.")
            return
        await _toggle_welcome(self._app, query, chat_id)


async def _toggle_welcome(app: App, query: CallbackQuery, chat_id: int) -> None:
    """Toggle the welcome flag and re-render the settings panel."""
    settings = await app.settings.get(chat_id)
    new_value = 0 if settings.welcome_enabled else 1
    await app.settings.set(chat_id, "welcome_enabled", new_value)
    label = "Welcome: ON" if new_value else "Welcome: OFF"
    from aiogram.types import InlineKeyboardButton, InlineKeyboardMarkup

    markup = InlineKeyboardMarkup(
        inline_keyboard=[
            [
                InlineKeyboardButton(
                    text=label, callback_data=f"set:{chat_id}:{query.from_user.id}:welcome"
                )
            ]
        ]
    )
    await query.answer("Welcome enabled." if new_value else "Welcome disabled.")
    if query.message is not None:
        try:
            await app.bot.edit_message_text(
                chat_id=chat_id,
                message_id=query.message.message_id,
                text="Chat settings",
                reply_markup=markup,
            )
        except Exception:
            _log.debug("could not edit settings panel")


def build_dispatcher(app: App) -> Dispatcher:
    """Register all middlewares, handlers, and lifecycle hooks on the dispatcher."""
    dp = app.dp
    context_mw = ContextMiddleware(app)
    dp.message.outer_middleware(context_mw)
    # Register the bound async methods: aiogram only awaits callbacks it can
    # recognise as coroutine functions, and a class instance with an async
    # __call__ is not detected as one.
    dp.message.register(MessageHandler(app).handle)

    join_handler = JoinHandler(app)
    dp.chat_member.register(join_handler.handle)
    dp.my_chat_member.register(join_handler.handle)

    dp.callback_query.register(CallbackHandler(app).handle)

    @dp.startup()
    async def _on_startup() -> None:
        app.expiry.start()
        app.captcha_sweeper.start()

    @dp.shutdown()
    async def _on_shutdown() -> None:
        await app.expiry.stop()
        await app.captcha_sweeper.stop()

    return dp


__all__ = ["App", "build_dispatcher", "render_help"]
