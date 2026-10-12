"""Shared pytest fixtures and test doubles.

These tests exercise *this project's* logic (parsers, services, handlers, the
permission matrix, the rate limiter) — never the aiogram library. Where a
handler needs a ``Bot``, it gets a :class:`FakeBot` that records the calls it
makes, so assertions are about our decisions, not about HTTP.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any

import pytest

from nacho_moderator.clock import FakeClock
from nacho_moderator.db import Database
from nacho_moderator.services.settings import SettingsService

CHAT_ID = -1001234567890
BOT_ID = 123456789
ADMIN_ID = 200
TARGET_ID = 300


@dataclass
class Call:
    """One recorded bot call."""

    name: str
    kwargs: dict[str, Any]


@dataclass
class FakeAdmin:
    """A minimal ``ChatMemberOwner``/``ChatMemberAdministrator`` stand-in.

    Carries just the fields :meth:`AdminCache._fetch` reads, so tests can seed
    the fake bot's admin list without constructing real aiogram models.
    """

    user: Any
    status: str = "administrator"
    can_restrict_members: bool = True
    can_delete_messages: bool = True
    can_pin_messages: bool = True
    can_promote_members: bool = False
    can_change_info: bool = True


def fake_admin(
    user_id: int,
    *,
    status: str = "administrator",
    username: str | None = None,
    rights: str = "all",
    **flags: bool,
) -> FakeAdmin:
    """Build a :class:`FakeAdmin`.

    ``rights="all"`` grants every mapped right; otherwise pass explicit
    ``can_*`` flags.
    """
    granted = {
        "can_restrict_members": True,
        "can_delete_messages": True,
        "can_pin_messages": True,
        "can_promote_members": True,
        "can_change_info": True,
    }
    if rights != "all":
        granted = dict.fromkeys(granted, False)
    granted.update(flags)
    user = type("U", (), {"id": user_id, "username": username})()
    return FakeAdmin(user=user, status=status, **granted)


def _snake(name: str) -> str:
    """Convert a camelCase Bot API method name to snake_case."""
    out: list[str] = []
    for ch in name:
        if ch.isupper():
            out.append("_")
            out.append(ch.lower())
        else:
            out.append(ch)
    return "".join(out).lstrip("_")


@dataclass
class FakeBot:
    """A stand-in for ``aiogram.Bot`` that records the calls made to it.

    Implements both spellings the code under test uses: the named shortcuts
    (``bot.send_message(...)``) and the callable form aiogram's ``Message.reply``
    shortcuts use (``bot(method)``). Every call is recorded as a :class:`Call`.
    """

    calls: list[Call] = field(default_factory=list)
    admins: list[Any] = field(default_factory=list)
    member_count: int = 42
    chat_title: str = "Testers"
    fail: set[str] = field(default_factory=set)
    error: BaseException | None = None

    async def __call__(self, method: Any) -> Any:
        """Handle the callable form ``bot(TelegramMethod)``."""
        name = _snake(method.__api_method__)
        fields = type(method).model_fields
        kwargs = {key: getattr(method, key) for key in fields if getattr(method, key) is not None}
        return await self._record(name, **kwargs)

    async def _record(self, name: str, **kwargs: Any) -> Any:
        if name in self.fail:
            if self.error is not None:
                raise self.error
            raise RuntimeError(f"injected failure in {name}")
        self.calls.append(Call(name, kwargs))
        if name in ("send_message", "edit_message_text"):
            return type("Msg", (), {"message_id": 1})()
        return True

    # Methods the handlers/services call, in aiogram's snake_case spelling.
    async def send_message(self, **kwargs: Any) -> Any:
        return await self._record("send_message", **kwargs)

    async def edit_message_text(self, **kwargs: Any) -> Any:
        return await self._record("edit_message_text", **kwargs)

    async def delete_message(self, **kwargs: Any) -> Any:
        return await self._record("delete_message", **kwargs)

    async def delete_messages(self, **kwargs: Any) -> Any:
        return await self._record("delete_messages", **kwargs)

    async def ban_chat_member(self, **kwargs: Any) -> Any:
        return await self._record("ban_chat_member", **kwargs)

    async def unban_chat_member(self, **kwargs: Any) -> Any:
        return await self._record("unban_chat_member", **kwargs)

    async def restrict_chat_member(self, **kwargs: Any) -> Any:
        return await self._record("restrict_chat_member", **kwargs)

    async def promote_chat_member(self, **kwargs: Any) -> Any:
        return await self._record("promote_chat_member", **kwargs)

    async def set_chat_administrator_custom_title(self, **kwargs: Any) -> Any:
        return await self._record("set_chat_administrator_custom_title", **kwargs)

    async def get_chat_member(self, **kwargs: Any) -> Any:
        return await self._record("get_chat_member", **kwargs)

    async def pin_chat_message(self, **kwargs: Any) -> Any:
        return await self._record("pin_chat_message", **kwargs)

    async def unpin_chat_message(self, **kwargs: Any) -> Any:
        return await self._record("unpin_chat_message", **kwargs)

    async def answer_callback_query(self, **kwargs: Any) -> Any:
        return await self._record("answer_callback_query", **kwargs)

    async def get_chat_member_count(self, chat_id: int) -> int:
        await self._record("get_chat_member_count", chat_id=chat_id)
        return self.member_count

    async def get_chat(self, chat_id: int) -> Any:
        await self._record("get_chat", chat_id=chat_id)
        return type("Chat", (), {"title": self.chat_title})()

    async def get_chat_administrators(self, chat_id: int) -> list[Any]:
        await self._record("get_chat_administrators", chat_id=chat_id)
        return self.admins

    def called(self, name: str) -> list[Call]:
        """Return the recorded calls for one method."""
        return [c for c in self.calls if c.name == name]

    def names(self) -> list[str]:
        """Return the recorded method names in order."""
        return [c.name for c in self.calls]

    def texts(self) -> list[str]:
        """Return the ``text`` argument of every ``send_message`` call."""
        return [c.kwargs.get("text", "") for c in self.called("send_message")]


@pytest.fixture
def fake_bot() -> FakeBot:
    """A fresh fake bot."""
    return FakeBot()


@pytest.fixture
async def db() -> Database:
    """An in-memory database."""
    database = await Database.connect(":memory:")
    try:
        yield database
    finally:
        await database.close()


@pytest.fixture
def fake_clock() -> FakeClock:
    """A deterministic clock starting at a fixed wall time."""
    return FakeClock()


@pytest.fixture
async def settings_service(db: Database, fake_clock: FakeClock) -> SettingsService:
    """A settings service over the in-memory database."""
    return SettingsService(db=db, clock=fake_clock)
