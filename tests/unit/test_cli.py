"""Tests for the CLI entry point and argument parsing."""

from __future__ import annotations

from pathlib import Path

import pytest

from nacho_moderator.config import parse_cli
from nacho_moderator.main import main


def test_version_flag(capsys) -> None:
    assert main(["--version"]) == 0
    assert "nacho-moderator" in capsys.readouterr().out


def test_check_config_ok(tmp_path: Path, capsys) -> None:
    path = tmp_path / "config.toml"
    path.write_text('bot_token = "1:A"\nmode = "polling"\n', encoding="utf-8")
    path.chmod(0o600)
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


def test_parse_cli_allow_insecure_flag() -> None:
    args, _ = parse_cli(["--allow-insecure-config"])
    assert args.allow_insecure_config is True


def test_check_config_insecure_file_refused(tmp_path: Path, capsys) -> None:
    path = tmp_path / "config.toml"
    path.write_text('bot_token = "123456:SECRETTOKENVALUE"\n', encoding="utf-8")
    path.chmod(0o644)
    assert main(["--check-config", str(path)]) == 1
    assert "chmod 600" in capsys.readouterr().err


def test_check_config_insecure_file_allowed_by_flag(tmp_path: Path, capsys) -> None:
    path = tmp_path / "config.toml"
    path.write_text('bot_token = "123456:SECRETTOKENVALUE"\n', encoding="utf-8")
    path.chmod(0o644)
    assert main(["--check-config", str(path), "--allow-insecure-config"]) == 0
    assert "ok" in capsys.readouterr().out


def test_check_config_insecure_file_allowed_by_env(
    tmp_path: Path, capsys, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setenv("ALLOW_INSECURE_CONFIG", "1")
    path = tmp_path / "config.toml"
    path.write_text('bot_token = "123456:SECRETTOKENVALUE"\n', encoding="utf-8")
    path.chmod(0o644)
    assert main(["--check-config", str(path)]) == 0
    assert "ok" in capsys.readouterr().out
