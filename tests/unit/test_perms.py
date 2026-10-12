"""Unit tests for the permission matrix."""

from __future__ import annotations

import pytest

from nacho_moderator.commands.registry import lookup
from nacho_moderator.constants import Right, Role
from nacho_moderator.errors import BotLacksRightError, PermissionDeniedError
from nacho_moderator.router import check_permission
from nacho_moderator.services.perms import Subject, subject_for_user


def _spec(name: str):
    spec = lookup(name)
    assert spec is not None
    return spec


def test_member_cannot_ban() -> None:
    caller = Subject(1, Role.ANYONE)
    bot = Subject(999, Role.OWNER, Right(0))
    with pytest.raises(PermissionDeniedError):
        check_permission(_spec("ban"), caller, bot)


def test_admin_with_right_can_ban() -> None:
    caller = Subject(1, Role.ADMIN, Right.RESTRICT)
    bot = Subject(999, Role.OWNER, Right(0))
    check_permission(_spec("ban"), caller, bot)


def test_admin_without_right_cannot_ban() -> None:
    caller = Subject(1, Role.ADMIN, Right.DELETE)
    bot = Subject(999, Role.OWNER, Right(0))
    with pytest.raises(PermissionDeniedError):
        check_permission(_spec("ban"), caller, bot)


def test_bot_lacking_right_is_reported_first() -> None:
    caller = Subject(1, Role.OWNER)
    bot = Subject(999, Role.ADMIN, Right.DELETE)  # no RESTRICT
    with pytest.raises(BotLacksRightError):
        check_permission(_spec("ban"), caller, bot)


def test_owner_bypasses_caller_right() -> None:
    caller = Subject(1, Role.OWNER)
    bot = Subject(999, Role.OWNER, Right(0))
    check_permission(_spec("ban"), caller, bot)


def test_sudo_bypasses_caller_right() -> None:
    caller = Subject(1, Role.SUDO, is_sudo=True)
    bot = Subject(999, Role.OWNER, Right(0))
    check_permission(_spec("ban"), caller, bot)


def test_owner_only_command_denied_for_admin() -> None:
    caller = Subject(1, Role.ADMIN, Right.CHANGE_INFO)
    bot = Subject(999, Role.OWNER, Right(0))
    with pytest.raises(PermissionDeniedError):
        check_permission(_spec("setlog"), caller, bot)


def test_anyone_command_allowed_for_member() -> None:
    caller = Subject(1, Role.ANYONE)
    bot = Subject(999, Role.OWNER, Right(0))
    check_permission(_spec("help"), caller, bot)


def test_subject_for_sudo_user() -> None:
    subject = subject_for_user(42, {}, {42})
    assert subject.role is Role.SUDO
    assert subject.is_sudo is True


def test_subject_for_admin_from_cache() -> None:
    admins = {7: Subject(7, Role.ADMIN, Right.RESTRICT, username="alice")}
    subject = subject_for_user(7, admins, set())
    assert subject.role is Role.ADMIN
    assert subject.has(Right.RESTRICT)


def test_right_flag_semantics() -> None:
    subject = Subject(1, Role.ADMIN, Right.RESTRICT | Right.DELETE)
    assert subject.has(Right.RESTRICT)
    assert subject.has(Right.RESTRICT | Right.DELETE)
    assert not subject.has(Right.PIN)
    assert subject.has(Right.NONE)
