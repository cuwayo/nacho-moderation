"""Unit tests for the pure helpers: duration parsing and HTML escaping."""

from __future__ import annotations

import pytest

from nacho_moderator.errors import ParseError
from nacho_moderator.models import html_escape, parse_duration


@pytest.mark.parametrize(
    ("text", "expected"),
    [
        ("30s", 30),
        ("10m", 600),
        ("2h", 7200),
        ("7d", 604800),
        ("1w", 604800),
        ("90", 90),
        ("0", 0),
        ("1D", 86400),
    ],
)
def test_parse_duration_accepts(text: str, expected: int) -> None:
    assert parse_duration(text) == expected


@pytest.mark.parametrize("text", ["", "m", "-5", "+5", "10x", "10m5", " 10m", "abc"])
def test_parse_duration_rejects(text: str) -> None:
    with pytest.raises(ParseError):
        parse_duration(text)


def test_parse_duration_rejects_overflow() -> None:
    with pytest.raises(ParseError):
        parse_duration("99999999999999999999999w")


def test_parse_duration_rejects_overflow_after_multiplier() -> None:
    """A value that fits but overflows once the unit multiplier applies."""
    with pytest.raises(ParseError):
        parse_duration("9223372036854775807m")


def test_html_escape_only_markup_chars() -> None:
    assert html_escape("<b>&</b>") == "&lt;b&gt;&amp;&lt;/b&gt;"
    assert html_escape("héllo") == "héllo"
