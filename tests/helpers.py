"""Small builders for message/context objects used by the handler tests.

These construct lightweight stand-ins for the aiogram types the handlers read
(id, text, chat, from_user, reply_to_message). They are deliberately minimal so
a test asserts on *our* behaviour, not on aiogram's models.
"""

from __future__ import annotations

from dataclasses import dataclass
from datetime import UTC, datetime
from typing import Any

from nacho_moderator.commands.context import CommandContext, Invocation
from nacho_moderator.constants import Right, Role
from nacho_moderator.services.perms import Subject
from tests.conftest import ADMIN_ID, BOT_ID, CHAT_ID, TARGET_ID, FakeBot

#: A fixed timestamp for aiogram message/update objects. aiogram coerces a bare
#: int at runtime but annotates ``date`` as ``datetime``, so tests pass this.
FIXED_DATE = datetime(2023, 11, 14, 22, 13, 20, tzinfo=UTC)


@dataclass
class FakeUser:
    """A minimal user."""

    id: int
    first_name: str = "User"
    username: str | None = None
    is_bot: bool = False


@dataclass
class FakeChat:
    """A minimal chat."""

    id: int = CHAT_ID
    type: str = "supergroup"
    title: str = "Testers"


@dataclass
class FakeMessage:
    """A minimal message."""

    message_id: int
    text: str | None
    from_user: FakeUser
    chat: FakeChat
    reply_to_message: FakeMessage | None = None


def user(user_id: int, **kwargs: Any) -> FakeUser:
    """Build a fake user."""
    return FakeUser(id=user_id, **kwargs)


def message(
    message_id: int,
    *,
    text: str | None = None,
    user_id: int = TARGET_ID,
    username: str | None = None,
    reply_to: FakeMessage | None = None,
    is_bot: bool = False,
) -> FakeMessage:
    """Build a fake message."""
    return FakeMessage(
        message_id=message_id,
        text=text,
        from_user=FakeUser(user_id, username=username, is_bot=is_bot),
        chat=FakeChat(),
        reply_to_message=reply_to,
    )


def admin_message(
    message_id: int, text: str, *, reply_to: FakeMessage | None = None
) -> FakeMessage:
    """Build a message from the chat administrator."""
    return message(message_id, text=text, user_id=ADMIN_ID, username="admin", reply_to=reply_to)


def context(
    *,
    bot: FakeBot,
    message: FakeMessage,
    args: str = "",
    caller: Subject | None = None,
    target_id: int | None = None,
    target_label: str = "",
    is_group: bool = True,
    settings: Any = None,
    db: Any = None,
    clock: Any = None,
    automod: Any = None,
    invalidate_admins: Any = None,
    log_chat_id: int = 0,
    bot_id: int = 0,
    admins: dict[int, Subject] | None = None,
) -> CommandContext:
    """Build a :class:`CommandContext` around fakes."""
    return CommandContext(
        bot=bot,  # type: ignore[arg-type]
        db=db,
        settings=settings,
        clock=clock,
        message=message,  # type: ignore[arg-type]
        chat_id=message.chat.id,
        invocation=Invocation(name="test", args=args),
        caller=caller or Subject(ADMIN_ID, Role.ADMIN, Right.RESTRICT, username="admin"),
        target_id=target_id,
        target_label=target_label,
        bot_username="mock_bot",
        is_group=is_group,
        bot_id=bot_id,
        admins=admins,
        invalidate_admins=invalidate_admins,
        automod=automod,
        log_chat_id=log_chat_id,
    )


__all__ = [
    "ADMIN_ID",
    "BOT_ID",
    "CHAT_ID",
    "FIXED_DATE",
    "TARGET_ID",
    "FakeBot",
    "FakeChat",
    "FakeMessage",
    "FakeUser",
    "admin_message",
    "context",
    "message",
    "user",
]
