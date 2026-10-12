"""Tests for the shared argument parsing helpers."""

from __future__ import annotations

import pytest

from nacho_moderator.commands.args import TargetError, parse_duration_reason, require_target


def test_parse_duration_and_reason() -> None:
    assert parse_duration_reason("10m spamming hard") == (600, "spamming hard")


def test_parse_reason_without_duration() -> None:
    assert parse_duration_reason("just a reason") == (None, "just a reason")


def test_parse_empty() -> None:
    assert parse_duration_reason("") == (None, "")


def test_parse_leading_duration_only() -> None:
    assert parse_duration_reason("5m") == (300, "")


def test_bare_number_is_seconds() -> None:
    assert parse_duration_reason("90 reason") == (90, "reason")


def test_require_target_present() -> None:
    assert require_target(42) == 42


def test_require_target_missing() -> None:
    with pytest.raises(TargetError):
        require_target(None)
