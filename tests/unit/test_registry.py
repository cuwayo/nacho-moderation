"""Unit tests for the command registry: /help and setMyCommands consistency."""

from __future__ import annotations

from nacho_moderator.commands.publish import _bot_commands
from nacho_moderator.commands.registry import COMMANDS, lookup, render_help
from nacho_moderator.constants import CommandFlag, Role

_NAME_RE = __import__("re").compile(r"^[a-z0-9_]{1,32}$")


def test_all_command_names_valid() -> None:
    for spec in COMMANDS:
        assert _NAME_RE.match(spec.name), spec.name
        assert 1 <= len(spec.brief) <= 256, spec.name


def test_command_names_unique() -> None:
    names = [spec.name for spec in COMMANDS]
    assert len(names) == len(set(names))


def test_lookup_finds_and_misses() -> None:
    assert lookup("ban") is not None
    assert lookup("nope") is None


def test_help_hides_group_only_in_private() -> None:
    group = render_help(Role.ADMIN, is_group=True)
    private = render_help(Role.ADMIN, is_group=False)
    assert "/ban" in group
    assert "/ban" not in private
    assert "/help" in private


def test_help_filters_by_role() -> None:
    member = render_help(Role.ANYONE, is_group=True)
    admin = render_help(Role.ADMIN, is_group=True)
    assert "/ban" not in member
    assert "/ban" in admin
    assert "/help" in member


def test_hidden_commands_never_listed() -> None:
    # No command is hidden today, but the invariant must hold.
    for spec in COMMANDS:
        if CommandFlag.HIDDEN in spec.flags:
            assert f"/{spec.name}" not in render_help(Role.OWNER, is_group=True)


def test_admin_scope_superset_of_group_scope() -> None:
    group = {c.command for c in _bot_commands(admin=False)}
    admin = {c.command for c in _bot_commands(admin=True)}
    assert group <= admin


def test_bot_commands_description_lengths() -> None:
    for cmd in _bot_commands(admin=True):
        assert 1 <= len(cmd.command) <= 32
        assert 1 <= len(cmd.description) <= 256


def test_member_only_commands_in_group_scope() -> None:
    group = {c.command for c in _bot_commands(admin=False)}
    assert "help" in group
    assert "ban" not in group
