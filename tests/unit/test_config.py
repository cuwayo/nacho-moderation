"""Unit tests for config loading and precedence."""

from __future__ import annotations

from pathlib import Path

import pytest

from nacho_moderator.config import Mode, load_settings
from nacho_moderator.errors import ConfigError


def _write(tmp_path: Path, text: str) -> Path:
    path = tmp_path / "config.toml"
    path.write_text(text, encoding="utf-8")
    # A secret-bearing file must be 0600 or the loader refuses it (P2); these
    # tests exercise loading, not the permission check.
    path.chmod(0o600)
    return path


def test_minimal_polling_config(tmp_path: Path) -> None:
    path = _write(tmp_path, 'bot_token = "123:ABC"\nmode = "polling"\n')
    settings = load_settings(path, environ={})
    assert settings.bot_token == "123:ABC"
    assert settings.mode is Mode.POLLING
    assert settings.api.pool_size == 4
    assert settings.polling.timeout == 30


def test_env_overrides_file(tmp_path: Path) -> None:
    path = _write(tmp_path, 'bot_token = "123:ABC"\n[api]\npool_size = 2\n')
    settings = load_settings(path, environ={"API_POOL_SIZE": "8"})
    assert settings.api.pool_size == 8


def test_cli_overrides_env_and_file(tmp_path: Path) -> None:
    path = _write(tmp_path, 'bot_token = "123:ABC"\n[log]\nlevel = "error"\n')
    settings = load_settings(
        path, environ={"LOG_LEVEL": "warn"}, cli_overrides={"log.level": "debug"}
    )
    assert settings.log.level.value == "debug"


def test_env_bot_token(tmp_path: Path) -> None:
    path = _write(tmp_path, 'mode = "polling"\n')
    settings = load_settings(path, environ={"BOT_TOKEN": "999:XYZ"})
    assert settings.bot_token == "999:XYZ"


def test_missing_bot_token_is_error(tmp_path: Path) -> None:
    path = _write(tmp_path, 'mode = "polling"\n')
    with pytest.raises(ConfigError):
        load_settings(path, environ={})


def test_webhook_requires_secret(tmp_path: Path) -> None:
    path = _write(tmp_path, 'bot_token = "1:A"\nmode = "webhook"\n')
    with pytest.raises(ConfigError):
        load_settings(path, environ={})


def test_webhook_secret_min_length(tmp_path: Path) -> None:
    path = _write(
        tmp_path,
        'bot_token = "1:A"\nmode = "webhook"\n[webhook]\nsecret = "short"\n',
    )
    with pytest.raises(ConfigError):
        load_settings(path, environ={})


def test_webhook_valid_secret(tmp_path: Path) -> None:
    path = _write(
        tmp_path,
        'bot_token = "1:A"\nmode = "webhook"\n[webhook]\nsecret = "0123456789abcdef"\n',
    )
    settings = load_settings(path, environ={})
    assert settings.webhook.secret == "0123456789abcdef"


def test_sudo_ids_comma_separated(tmp_path: Path) -> None:
    path = _write(tmp_path, 'bot_token = "1:A"\nsudo_ids = "11, 22,33"\n')
    settings = load_settings(path, environ={})
    assert settings.sudo_ids == [11, 22, 33]


def test_missing_default_file_is_ok_when_not_required(tmp_path: Path) -> None:
    settings = load_settings(
        tmp_path / "absent.toml", required_file=False, environ={"BOT_TOKEN": "1:A"}
    )
    assert settings.bot_token == "1:A"


def test_missing_required_file_is_error(tmp_path: Path) -> None:
    with pytest.raises(ConfigError):
        load_settings(tmp_path / "absent.toml", required_file=True, environ={})


def test_redacted_masks_secrets(tmp_path: Path) -> None:
    path = _write(tmp_path, 'bot_token = "123:ABC"\n')
    settings = load_settings(path, environ={})
    redacted = settings.redacted()
    assert redacted["bot_token"] == "***"
    assert "ABC" not in str(redacted)


def test_unknown_key_is_rejected(tmp_path: Path) -> None:
    path = _write(tmp_path, 'bot_token = "1:A"\nbogus = 1\n')
    with pytest.raises(ConfigError):
        load_settings(path, environ={})
