"""Tests for publishing the command menu via ``setMyCommands``.

The two scope payloads are derived from the registry, so these assert on *our*
filtering (member scope excludes admin-only commands; the admin scope is a
superset) and on the best-effort failure path: a ``set_my_commands`` error is
logged as a warning and never propagates.
"""

from __future__ import annotations

import logging
from dataclasses import replace

from nacho_moderator.commands import publish
from nacho_moderator.commands.publish import _bot_commands, publish_commands
from nacho_moderator.commands.registry import COMMANDS
from nacho_moderator.constants import CommandFlag, Role
from tests.conftest import Call, FakeBot


class _RecordingBot(FakeBot):
    """A fake bot that records ``set_my_commands`` calls (it has no ``_record``)."""

    async def set_my_commands(self, commands, **kwargs):
        self.calls.append(Call("set_my_commands", kwargs))


class _RaisingBot(FakeBot):
    """A fake bot whose ``set_my_commands`` always raises."""

    async def set_my_commands(self, commands, **kwargs):
        raise RuntimeError("boom")


def test_member_scope_excludes_admin_commands() -> None:
    names = {c.command for c in _bot_commands(admin=False)}
    assert "help" in names
    assert "warns" in names
    assert "ban" not in names
    assert "setlog" not in names


def test_admin_scope_is_superset_of_member_scope() -> None:
    member = {c.command for c in _bot_commands(admin=False)}
    admin = {c.command for c in _bot_commands(admin=True)}
    assert member <= admin
    assert "ban" in admin
    assert "warns" in admin


def test_bot_commands_have_command_and_description() -> None:
    for cmd in _bot_commands(admin=True):
        assert cmd.command
        assert cmd.description


async def test_publish_commands_registers_both_scopes() -> None:
    bot = _RecordingBot()
    await publish_commands(bot)  # type: ignore[arg-type]
    scopes = [c.kwargs["scope"].type.value for c in bot.called("set_my_commands")]
    assert scopes == ["all_group_chats", "all_chat_administrators"]


async def test_publish_commands_swallows_failure(caplog) -> None:
    bot = _RaisingBot()
    with caplog.at_level(logging.WARNING, logger="nacho_moderator.publish"):
        # Must not propagate.
        await publish_commands(bot)  # type: ignore[arg-type]
    assert "setMyCommands failed" in caplog.text


def test_visible_to_owner_always_true() -> None:
    # OWNER sees every command regardless of min_role.
    for spec in COMMANDS:
        assert spec.visible_to(Role.OWNER, is_group=True) is True


def test_bot_commands_skips_hidden(monkeypatch) -> None:
    # No command is hidden today, so inject one to exercise the skip branch.
    hidden = replace(COMMANDS[0], name="ghost", flags=CommandFlag.HIDDEN)
    monkeypatch.setattr(publish, "COMMANDS", [*COMMANDS, hidden])
    names = {c.command for c in _bot_commands(admin=True)}
    assert "ghost" not in names
