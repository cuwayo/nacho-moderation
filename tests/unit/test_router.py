"""Unit tests for command parsing and target resolution."""

from __future__ import annotations

from typing import Any, cast

import pytest

from nacho_moderator.commands.registry import CommandSpec
from nacho_moderator.constants import CommandFlag, Right, Role
from nacho_moderator.errors import BotLacksRightError, PermissionDeniedError
from nacho_moderator.router import (
    check_permission,
    parse_command,
    resolve_target,
    target_is_protected,
)
from nacho_moderator.services.perms import Subject
from tests.helpers import message


def test_plain_command() -> None:
    parsed = parse_command("/help", want_target=False, bot_username="mybot")
    assert parsed is not None
    assert parsed.name == "help"
    assert parsed.args == ""
    assert parsed.target is None
    assert parsed.addressed_to_other_bot is False


def test_command_is_lowercased() -> None:
    parsed = parse_command("/BAN", want_target=False, bot_username="mybot")
    assert parsed is not None and parsed.name == "ban"


def test_bot_suffix_matching_own_username() -> None:
    parsed = parse_command("/ban@mybot 5", want_target=False, bot_username="mybot")
    assert parsed is not None
    assert parsed.name == "ban"
    assert parsed.addressed_to_other_bot is False


def test_bot_suffix_for_other_bot_is_flagged() -> None:
    parsed = parse_command("/ban@otherbot", want_target=False, bot_username="mybot")
    assert parsed is not None and parsed.addressed_to_other_bot is True


def test_numeric_target_consumed() -> None:
    parsed = parse_command("/ban 123456789 10m spam", want_target=True, bot_username="mybot")
    assert parsed is not None
    assert parsed.target is not None and parsed.target.user_id == 123456789
    assert parsed.args == "10m spam"


def test_username_target_consumed() -> None:
    parsed = parse_command("/ban @spammer 10m", want_target=True, bot_username="mybot")
    assert parsed is not None
    assert parsed.target is not None and parsed.target.username == "spammer"
    assert parsed.args == "10m"


def test_duration_is_not_taken_as_target() -> None:
    parsed = parse_command("/ban 10m spam", want_target=True, bot_username="mybot")
    assert parsed is not None
    assert parsed.target is None
    assert parsed.args == "10m spam"


def test_numeric_arg_kept_when_no_target_flag() -> None:
    parsed = parse_command("/setwarnlimit 5", want_target=False, bot_username="mybot")
    assert parsed is not None
    assert parsed.target is None
    assert parsed.args == "5"


def test_non_command_returns_none() -> None:
    assert parse_command("hello world", want_target=False, bot_username="mybot") is None


def test_bare_slash_is_not_a_command() -> None:
    assert parse_command("/", want_target=False, bot_username="mybot") is None


def test_overlong_name_rejected() -> None:
    assert parse_command("/" + "a" * 40, want_target=False, bot_username="mybot") is None


# --- target resolution -----------------------------------------------------


async def test_resolve_target_from_reply_wins() -> None:
    reply = message(10, text="hi", user_id=42)
    msg = message(11, text="/ban 99", user_id=1, reply_to=reply)
    # The reply sender wins over an explicit numeric target.
    assert await resolve_target(msg, None, {}, object()) == 42  # type: ignore[arg-type]


async def test_resolve_target_numeric() -> None:
    from nacho_moderator.models import TargetRef

    msg = message(11, text="/ban 99", user_id=1)
    assert await resolve_target(msg, TargetRef(user_id=99), {}, object()) == 99  # type: ignore[arg-type]


async def test_resolve_target_none_without_reply_or_token() -> None:
    msg = message(11, text="/ban", user_id=1)
    assert await resolve_target(msg, None, {}, object()) is None  # type: ignore[arg-type]


async def test_resolve_target_username_from_admins() -> None:
    from nacho_moderator.models import TargetRef

    admins = {7: Subject(7, Role.ADMIN, Right.RESTRICT, username="Alice")}
    msg = message(11, text="/ban @alice", user_id=1)
    assert await resolve_target(msg, TargetRef(username="alice"), admins, object()) == 7  # type: ignore[arg-type]


async def test_resolve_target_username_from_db() -> None:
    from nacho_moderator.models import TargetRef

    class _Db:
        async def find_user_by_username(self, username: str) -> int | None:
            return 555 if username == "bob" else None

    msg = message(11, text="/ban @bob", user_id=1)
    assert await resolve_target(cast("Any", msg), TargetRef(username="bob"), {}, _Db()) == 555


async def test_resolve_target_username_db_without_lookup() -> None:
    from nacho_moderator.models import TargetRef

    msg = message(11, text="/ban @bob", user_id=1)
    assert await resolve_target(msg, TargetRef(username="bob"), {}, object()) is None  # type: ignore[arg-type]


# --- permission gating -----------------------------------------------------


def _spec(min_role: Role, user_right: Right, bot_right: Right) -> CommandSpec:
    """Build a throwaway CommandSpec for permission tests."""

    async def _handler(ctx: object) -> None:  # pragma: no cover - never called
        return None

    return CommandSpec(
        "t", "t", min_role, user_right, bot_right, CommandFlag.NONE, "", "b", _handler
    )


def test_bot_lacking_right_raises() -> None:
    spec = _spec(Role.ADMIN, Right.RESTRICT, Right.RESTRICT)
    caller = Subject(1, Role.ADMIN, Right.RESTRICT)
    bot = Subject(2, Role.ADMIN, Right.NONE)
    with pytest.raises(BotLacksRightError):
        check_permission(spec, caller, bot)


def test_owner_bypasses_caller_check() -> None:
    spec = _spec(Role.ADMIN, Right.RESTRICT, Right.NONE)
    check_permission(spec, Subject(1, Role.OWNER), Subject(2, Role.ADMIN, Right.NONE))


def test_sudo_bypasses_caller_check() -> None:
    spec = _spec(Role.ADMIN, Right.RESTRICT, Right.NONE)
    check_permission(spec, Subject(1, Role.SUDO, is_sudo=True), Subject(2, Role.ADMIN, Right.NONE))


def test_owner_only_command_denied_for_admin() -> None:
    spec = _spec(Role.OWNER, Right.NONE, Right.NONE)
    with pytest.raises(PermissionDeniedError):
        check_permission(spec, Subject(1, Role.ADMIN), Subject(2, Role.ADMIN))


def test_admin_without_right_denied() -> None:
    spec = _spec(Role.ADMIN, Right.RESTRICT, Right.NONE)
    with pytest.raises(PermissionDeniedError):
        check_permission(spec, Subject(1, Role.ADMIN, Right.DELETE), Subject(2, Role.ADMIN))


def test_member_denied_for_admin_command() -> None:
    spec = _spec(Role.ADMIN, Right.RESTRICT, Right.NONE)
    with pytest.raises(PermissionDeniedError):
        check_permission(spec, Subject(1, Role.ANYONE), Subject(2, Role.ADMIN))


def test_target_is_protected() -> None:
    admins = {7: Subject(7, Role.ADMIN, Right.RESTRICT), 8: Subject(8, Role.OWNER)}
    assert target_is_protected(5, admins, bot_id=5) is True  # the bot itself
    assert target_is_protected(7, admins, bot_id=5) is True  # an admin
    assert target_is_protected(8, admins, bot_id=5) is True  # the owner
    assert target_is_protected(123, admins, bot_id=5) is False  # a plain member
