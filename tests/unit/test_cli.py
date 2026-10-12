"""Tests for the CLI entry point and argument parsing."""

from __future__ import annotations

from pathlib import Path

from nacho_moderator.config import parse_cli
from nacho_moderator.main import main


def test_version_flag(capsys) -> None:
    assert main(["--version"]) == 0
    assert "nacho-moderator" in capsys.readouterr().out


def test_check_config_ok(tmp_path: Path, capsys) -> None:
    path = tmp_path / "config.toml"
    path.write_text('bot_token = "1:A"\nmode = "polling"\n', encoding="utf-8")
    assert main(["--check-config", str(path)]) == 0
    assert "ok" in capsys.readouterr().out


def test_check_config_invalid(tmp_path: Path, capsys) -> None:
    path = tmp_path / "config.toml"
    path.write_text('mode = "polling"\n', encoding="utf-8")  # missing bot_token
    assert main(["--check-config", str(path)]) == 1
    assert "error" in capsys.readouterr().err


def test_missing_config_file_is_error(capsys) -> None:
    assert main(["--check-config", "/nonexistent/config.toml"]) == 1
    assert "error" in capsys.readouterr().err


def test_parse_cli_log_level_override() -> None:
    args, overrides = parse_cli(["--log-level", "debug"])
    assert args.log_level == "debug"
    assert overrides["log.level"] == "debug"


def test_parse_cli_defaults() -> None:
    args, overrides = parse_cli([])
    assert args.config is None
    assert overrides == {}
