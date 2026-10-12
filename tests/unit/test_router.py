"""Unit tests for command parsing and target resolution."""

from __future__ import annotations

from nacho_moderator.router import parse_command


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
