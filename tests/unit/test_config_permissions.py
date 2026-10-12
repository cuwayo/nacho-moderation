"""Tests for the config-file permission check (P2).

A config file that carries a secret must not be readable by group or other; the
check resolves symlinks first, so a symlink to an insecure file is refused, and
it is skipped entirely for environment-only configuration. The
``--allow-insecure-config`` flag (or ``ALLOW_INSECURE_CONFIG=1``) overrides it.
"""

from __future__ import annotations

from pathlib import Path

import pytest

from nacho_moderator.config import load_settings, resolve_allow_insecure
from nacho_moderator.errors import ConfigError

_SECRET_CONFIG = 'bot_token = "123456:ABCsecretTOKENvalue"\nmode = "polling"\n'


def _write(tmp_path: Path, text: str, mode: int) -> Path:
    path = tmp_path / "config.toml"
    path.write_text(text, encoding="utf-8")
    path.chmod(mode)
    return path


@pytest.mark.parametrize("mode", [0o600])
def test_secure_mode_is_accepted(tmp_path: Path, mode: int) -> None:
    path = _write(tmp_path, _SECRET_CONFIG, mode)
    settings = load_settings(path, environ={})
    assert settings.bot_token == "123456:ABCsecretTOKENvalue"


@pytest.mark.parametrize("mode", [0o640, 0o644, 0o666, 0o604, 0o660])
def test_insecure_mode_with_secret_is_refused(tmp_path: Path, mode: int) -> None:
    path = _write(tmp_path, _SECRET_CONFIG, mode)
    with pytest.raises(ConfigError) as excinfo:
        load_settings(path, environ={})
    assert "chmod 600" in str(excinfo.value)


def test_insecure_mode_without_secret_is_allowed(tmp_path: Path) -> None:
    """A file with no secret value is not permission-checked."""
    path = _write(tmp_path, 'mode = "polling"\n', 0o644)
    settings = load_settings(path, environ={"BOT_TOKEN": "1:A"}, allow_insecure_config=False)
    assert settings.bot_token == "1:A"


def test_env_only_config_is_unaffected(tmp_path: Path) -> None:
    """No config file at all: nothing to check."""
    settings = load_settings(
        tmp_path / "absent.toml", required_file=False, environ={"BOT_TOKEN": "1:A"}
    )
    assert settings.bot_token == "1:A"


def test_missing_file_is_not_permission_checked(tmp_path: Path) -> None:
    settings = load_settings(
        tmp_path / "absent.toml",
        required_file=False,
        environ={"BOT_TOKEN": "1:A"},
    )
    assert settings.bot_token == "1:A"


def test_flag_override_allows_insecure(tmp_path: Path) -> None:
    path = _write(tmp_path, _SECRET_CONFIG, 0o644)
    settings = load_settings(path, environ={}, allow_insecure_config=True)
    assert settings.bot_token == "123456:ABCsecretTOKENvalue"


def test_symlink_to_insecure_file_is_refused(tmp_path: Path) -> None:
    """The symlink's target is resolved and its mode checked."""
    target = tmp_path / "real.toml"
    target.write_text(_SECRET_CONFIG, encoding="utf-8")
    target.chmod(0o644)
    link = tmp_path / "link.toml"
    link.symlink_to(target)
    with pytest.raises(ConfigError):
        load_settings(link, environ={})


def test_symlink_to_secure_file_is_accepted(tmp_path: Path) -> None:
    target = tmp_path / "real.toml"
    target.write_text(_SECRET_CONFIG, encoding="utf-8")
    target.chmod(0o600)
    link = tmp_path / "link.toml"
    link.symlink_to(target)
    settings = load_settings(link, environ={})
    assert settings.bot_token == "123456:ABCsecretTOKENvalue"


def test_webhook_secret_triggers_the_check(tmp_path: Path) -> None:
    path = _write(
        tmp_path,
        'mode = "webhook"\n[webhook]\nsecret = "0123456789abcdef"\n',
        0o644,
    )
    with pytest.raises(ConfigError):
        load_settings(path, environ={"BOT_TOKEN": "1:A"})


def test_resolve_allow_insecure_flag() -> None:
    import argparse

    args = argparse.Namespace(allow_insecure_config=True)
    assert resolve_allow_insecure(args, environ={}) is True


def test_resolve_allow_insecure_env() -> None:
    import argparse

    args = argparse.Namespace(allow_insecure_config=False)
    assert resolve_allow_insecure(args, environ={"ALLOW_INSECURE_CONFIG": "1"}) is True
    assert resolve_allow_insecure(args, environ={"ALLOW_INSECURE_CONFIG": "true"}) is True
    assert resolve_allow_insecure(args, environ={"ALLOW_INSECURE_CONFIG": "0"}) is False
    assert resolve_allow_insecure(args, environ={}) is False


def test_permission_check_runs_before_validation(tmp_path: Path) -> None:
    """An insecure file is refused even if the config is otherwise invalid."""
    path = _write(tmp_path, 'mode = "polling"\n', 0o644)  # no secret, no token
    # No secret -> no permission error; validation error instead.
    with pytest.raises(ConfigError) as excinfo:
        load_settings(path, environ={})
    assert "chmod" not in str(excinfo.value)


def test_insecure_message_names_the_path(tmp_path: Path) -> None:
    path = _write(tmp_path, _SECRET_CONFIG, 0o640)
    with pytest.raises(ConfigError) as excinfo:
        load_settings(path, environ={})
    assert str(path) in str(excinfo.value)


def test_no_check_on_windows_style_modes(tmp_path: Path) -> None:
    """Sanity: a 0600 file on a system without other users still passes."""
    path = _write(tmp_path, _SECRET_CONFIG, 0o600)
    assert path.stat().st_mode & 0o077 == 0
    load_settings(path, environ={})
