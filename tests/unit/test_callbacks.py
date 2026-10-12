"""Unit tests for the captcha and settings callback parsers, and the welcome renderer."""

from __future__ import annotations

from aiogram.types import User

from nacho_moderator.commands.settings_cmds import parse_settings_callback
from nacho_moderator.services.captcha import parse_captcha_callback
from nacho_moderator.services.welcome import render_welcome


def test_captcha_callback_round_trip() -> None:
    parsed = parse_captcha_callback("cap:-1001234567890:300:deadbeef")
    assert parsed is not None
    assert parsed.chat_id == -1001234567890
    assert parsed.user_id == 300
    assert parsed.nonce == "deadbeef"


def test_captcha_callback_rejects_other_data() -> None:
    assert parse_captcha_callback("set:-100:1:welcome") is None
    assert parse_captcha_callback("cap:bad") is None
    assert parse_captcha_callback("cap:1:2:") is None


def test_settings_callback_round_trip() -> None:
    parsed = parse_settings_callback("set:-100123:300:welcome")
    assert parsed == (-100123, 300, "welcome")


def test_settings_callback_rejects_other_data() -> None:
    assert parse_settings_callback("cap:1:2:3") is None
    assert parse_settings_callback("set:1:2") is None
    assert parse_settings_callback("set:x:2:welcome") is None


def _user(first: str = "Alice", username: str | None = "alice") -> User:
    return User(id=1, is_bot=False, first_name=first, username=username)


def test_welcome_substitutes_all_placeholders() -> None:
    text = render_welcome("{first} {mention} {chat} {count}", _user(), "Testers", 42)
    assert "Alice" in text
    assert '<a href="https://t.me/alice">Alice</a>' in text
    assert "Testers" in text
    assert "42" in text


def test_welcome_first_defaults_to_there() -> None:
    user = User(id=1, is_bot=False, first_name="")
    assert render_welcome("{first}", user, "T", 0) == "there"


def test_welcome_escapes_non_markup_values() -> None:
    text = render_welcome("{chat}", _user(), "<Testers>", 0)
    assert text == "&lt;Testers&gt;"


def test_welcome_mention_without_username_is_plain() -> None:
    text = render_welcome("{mention}", _user(username=None), "T", 0)
    assert text == "Alice"


def test_welcome_unknown_placeholder_passes_through() -> None:
    assert render_welcome("{nope}", _user(), "T", 0) == "{nope}"
