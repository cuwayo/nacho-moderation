"""Configuration loading, validation, and precedence.

Values come from four sources, highest priority first: CLI flags, environment
variables, the TOML config file, then built-in defaults. The ``--config`` file
is optional; a missing default file is not an error, but an explicitly requested
file that does not exist is.

Precedence is implemented once, here, from an explicit flat map of environment
variable names to dotted config keys, so no nested model can accidentally pick
up an unrelated environment variable.

Secrets (``bot_token``, ``webhook.secret``) are never logged and are redacted by
:meth:`Settings.redacted`.
"""

from __future__ import annotations

import argparse
import os
import tomllib
from enum import StrEnum
from pathlib import Path
from typing import Any

from pydantic import BaseModel, ConfigDict, Field, field_validator, model_validator

from nacho_moderator.errors import ConfigError

DEFAULT_CONFIG_PATH = Path("config.toml")
WEBHOOK_SECRET_MIN_LEN = 16


class Mode(StrEnum):
    """The update source the bot uses."""

    POLLING = "polling"
    WEBHOOK = "webhook"


class LogLevelName(StrEnum):
    """Accepted ``log.level`` values."""

    ERROR = "error"
    WARN = "warn"
    INFO = "info"
    DEBUG = "debug"


class WebhookSettings(BaseModel):
    """Webhook-mode settings."""

    model_config = ConfigDict(extra="forbid")

    listen: str = "127.0.0.1:8080"
    secret: str = ""

    @property
    def host(self) -> str:
        """Return the listen host part."""
        return self.listen.rsplit(":", 1)[0]

    @property
    def port(self) -> int:
        """Return the listen port part."""
        return int(self.listen.rsplit(":", 1)[1])


class DbSettings(BaseModel):
    """Database settings."""

    model_config = ConfigDict(extra="forbid")

    path: str = "./bot.db"


class ApiSettings(BaseModel):
    """Bot API client settings."""

    model_config = ConfigDict(extra="forbid")

    pool_size: int = 4
    base: str = "https://api.telegram.org"
    request_timeout_ms: int = 15000

    @property
    def request_timeout_seconds(self) -> float:
        """Return the request timeout in seconds."""
        return self.request_timeout_ms / 1000.0


class PollingSettings(BaseModel):
    """Long-poll settings."""

    model_config = ConfigDict(extra="forbid")

    timeout: int = 30


class LogSettings(BaseModel):
    """Logging settings."""

    model_config = ConfigDict(extra="forbid")

    level: LogLevelName = LogLevelName.INFO


class Settings(BaseModel):
    """The fully-resolved bot configuration."""

    model_config = ConfigDict(extra="forbid")

    bot_token: str = ""
    mode: Mode = Mode.POLLING
    sudo_ids: list[int] = Field(default_factory=list)

    webhook: WebhookSettings = Field(default_factory=WebhookSettings)
    db: DbSettings = Field(default_factory=DbSettings)
    api: ApiSettings = Field(default_factory=ApiSettings)
    polling: PollingSettings = Field(default_factory=PollingSettings)
    log: LogSettings = Field(default_factory=LogSettings)

    @field_validator("sudo_ids", mode="before")
    @classmethod
    def _split_sudo_ids(cls, value: Any) -> Any:
        """Accept a comma-separated string as well as a list of ints."""
        if isinstance(value, str):
            if not value.strip():
                return []
            try:
                return [int(part) for part in value.split(",") if part.strip()]
            except ValueError as exc:
                raise ValueError(f"invalid sudo_ids: {value!r}") from exc
        return value

    @model_validator(mode="after")
    def _validate(self) -> Settings:
        """Enforce the required keys and the mode-dependent webhook secret."""
        if not self.bot_token:
            raise ValueError("bot_token is required")
        if self.mode is Mode.WEBHOOK and len(self.webhook.secret) < WEBHOOK_SECRET_MIN_LEN:
            raise ValueError(
                f"webhook.secret must be at least {WEBHOOK_SECRET_MIN_LEN} "
                "characters in webhook mode"
            )
        if self.api.pool_size < 1:
            raise ValueError("api.pool_size must be at least 1")
        if self.polling.timeout < 0:
            raise ValueError("polling.timeout must not be negative")
        if self.api.request_timeout_ms <= 0:
            raise ValueError("api.request_timeout_ms must be positive")
        return self

    def redacted(self) -> dict[str, Any]:
        """Return a log-safe view with secret values masked."""
        return {
            "bot_token": "***" if self.bot_token else "",
            "mode": self.mode.value,
            "sudo_ids": self.sudo_ids,
            "webhook": {
                "listen": self.webhook.listen,
                "secret": "***" if self.webhook.secret else "",
            },
            "db": {"path": self.db.path},
            "api": {
                "pool_size": self.api.pool_size,
                "base": self.api.base,
                "request_timeout_ms": self.api.request_timeout_ms,
            },
            "polling": {"timeout": self.polling.timeout},
            "log": {"level": self.log.level.value},
        }


# Environment variable name -> dotted config key. Flat and explicit on purpose.
_ENV_MAP: dict[str, str] = {
    "BOT_TOKEN": "bot_token",
    "MODE": "mode",
    "SUDO_IDS": "sudo_ids",
    "DB_PATH": "db.path",
    "API_POOL_SIZE": "api.pool_size",
    "API_BASE": "api.base",
    "API_REQUEST_TIMEOUT_MS": "api.request_timeout_ms",
    "POLLING_TIMEOUT": "polling.timeout",
    "LOG_LEVEL": "log.level",
    "WEBHOOK_LISTEN": "webhook.listen",
    "WEBHOOK_SECRET": "webhook.secret",
}


def _read_toml(path: Path, *, required: bool) -> dict[str, Any]:
    """Read a TOML config file, honouring the ``required`` flag."""
    if not path.exists():
        if required:
            raise ConfigError(f"config file not found: {path}")
        return {}
    try:
        with path.open("rb") as fh:
            return tomllib.load(fh)
    except tomllib.TOMLDecodeError as exc:
        raise ConfigError(f"invalid TOML in {path}: {exc}") from exc


def _set_nested(target: dict[str, Any], dotted: str, value: Any) -> None:
    """Assign ``value`` at a dotted key path, creating intermediate dicts."""
    head, _, tail = dotted.partition(".")
    if tail:
        nested = target.setdefault(head, {})
        _set_nested(nested, tail, value)
    else:
        target[head] = value


def load_settings(
    config_path: Path | None = None,
    *,
    required_file: bool = False,
    environ: dict[str, str] | None = None,
    cli_overrides: dict[str, Any] | None = None,
) -> Settings:
    """Load and validate settings from all four sources.

    Args:
        config_path: The TOML file to read (defaults to ``./config.toml``).
        required_file: If ``True``, a missing file is an error.
        environ: Environment mapping (defaults to :data:`os.environ`).
        cli_overrides: Highest-priority dotted-key overrides from the CLI.

    Returns:
        The validated :class:`Settings`.

    Raises:
        ConfigError: If a file is missing/invalid or validation fails.
    """
    path = config_path if config_path is not None else DEFAULT_CONFIG_PATH
    env = environ if environ is not None else dict(os.environ)

    merged: dict[str, Any] = _read_toml(path, required=required_file)
    for env_name, key in _ENV_MAP.items():
        value = env.get(env_name)
        if value is not None and value != "":
            _set_nested(merged, key, value)
    for key, value in (cli_overrides or {}).items():
        if value is not None:
            _set_nested(merged, key, value)

    try:
        return Settings.model_validate(merged)
    except Exception as exc:  # pydantic ValidationError
        raise ConfigError(str(exc)) from exc


def parse_cli(argv: list[str] | None = None) -> tuple[argparse.Namespace, dict[str, Any]]:
    """Parse the command-line arguments.

    Returns:
        A tuple of the parsed namespace and the dotted-key overrides to apply.
    """
    parser = argparse.ArgumentParser(
        prog="nacho-moderator",
        description="An asyncio Telegram admin/moderator bot built with aiogram",
    )
    parser.add_argument(
        "--config", default=None, help="path to config.toml (default: ./config.toml)"
    )
    parser.add_argument("--check-config", default=None, help="validate a config file and exit")
    parser.add_argument(
        "--log-level",
        default=None,
        choices=[level.value for level in LogLevelName],
        help="override the log level",
    )
    parser.add_argument("--version", "-V", action="store_true", help="show version and exit")
    args = parser.parse_args(argv)

    overrides: dict[str, Any] = {}
    if args.log_level is not None:
        overrides["log.level"] = args.log_level
    return args, overrides
