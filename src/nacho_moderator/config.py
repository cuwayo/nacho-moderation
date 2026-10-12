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
import stat
import tomllib
from collections.abc import Iterator
from enum import StrEnum
from pathlib import Path
from typing import Any

from pydantic import BaseModel, ConfigDict, Field, ValidationError, field_validator, model_validator

from nacho_moderator.errors import ConfigError

DEFAULT_CONFIG_PATH = Path("config.toml")
WEBHOOK_SECRET_MIN_LEN = 16

#: Replacement shown wherever a secret value would otherwise be printed.
MASKED = "***"

#: Keys whose values are secrets: never rendered, never echoed in an error.
_SECRET_KEYS = frozenset({"bot_token", "secret"})


class _MaskedReprModel(BaseModel):
    """A pydantic model whose ``repr``/``str`` mask secret-valued fields.

    Pydantic renders a model by iterating :meth:`__repr_args__`; masking the
    secret keys there keeps a stray ``log.info("%s", settings)`` from leaking the
    token or webhook secret.
    """

    def __repr_args__(self) -> Iterator[tuple[str | None, Any]]:
        """Yield field pairs, masking the value of every secret key."""
        for key, value in super().__repr_args__():
            yield (key, MASKED if key in _SECRET_KEYS else value)


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


class WebhookSettings(_MaskedReprModel):
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


class Settings(_MaskedReprModel):
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
            "bot_token": MASKED if self.bot_token else "",
            "mode": self.mode.value,
            "sudo_ids": self.sudo_ids,
            "webhook": {
                "listen": self.webhook.listen,
                "secret": MASKED if self.webhook.secret else "",
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


def _safe_validation_message(exc: ValidationError) -> str:
    """Render a validation error without echoing any input value.

    Pydantic includes the offending input in its default message; for a secret
    field that would print the token. ``include_input=False`` keeps the field
    path and the reason while dropping the value.

    Args:
        exc: The pydantic validation error to render.

    Returns:
        A human-readable, secret-free description of the failure.
    """
    lines = [f"{len(exc.errors())} validation error(s) for Settings:"]
    for error in exc.errors(include_url=False, include_input=False):
        location = ".".join(str(part) for part in error["loc"]) or "<root>"
        lines.append(f"  {location}: {error['msg']}")
    return "\n".join(lines)


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


def _contains_secret(data: dict[str, Any]) -> bool:
    """Return whether a parsed config mapping holds a secret value.

    A secret is present when ``bot_token`` is non-empty or ``webhook.secret`` is
    non-empty; an empty placeholder does not count, so a tokenless CI config is
    not treated as secret-bearing.

    Args:
        data: The parsed TOML mapping.

    Returns:
        ``True`` if a secret key carries a value.
    """
    if str(data.get("bot_token", "") or "").strip():
        return True
    webhook = data.get("webhook")
    return bool(isinstance(webhook, dict) and str(webhook.get("secret", "") or "").strip())


def check_file_permissions(path: Path) -> None:
    """Refuse a config file that holds a secret and is group/other accessible.

    The file is resolved first (so a symlink is checked by its target's mode),
    then its permission bits are inspected. Any group or other bit
    (``mode & 0o077``) is refused: a secret-bearing file readable by another
    user is a leak. An environment-only configuration is unaffected because
    there is no file to check.

    Args:
        path: The config file to inspect.

    Raises:
        ConfigError: If the file holds a secret and is group/other accessible.
    """
    if not path.exists():
        return
    resolved = path.resolve()
    try:
        mode = stat.S_IMODE(resolved.stat().st_mode)
    except OSError as exc:  # pragma: no cover - stat on a resolved existing path
        raise ConfigError(f"cannot stat config file {path}: {exc}") from exc
    if mode & 0o077:
        raise ConfigError(
            f"{path} contains a secret but is group/other accessible "
            f"(mode {mode:04o}); run `chmod 600 {path}` or pass "
            "--allow-insecure-config (env ALLOW_INSECURE_CONFIG=1) to override"
        )


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
    allow_insecure_config: bool = False,
) -> Settings:
    """Load and validate settings from all four sources.

    Args:
        config_path: The TOML file to read (defaults to ``./config.toml``).
        required_file: If ``True``, a missing file is an error.
        environ: Environment mapping (defaults to :data:`os.environ`).
        cli_overrides: Highest-priority dotted-key overrides from the CLI.
        allow_insecure_config: Skip the secret-bearing file permission check.

    Returns:
        The validated :class:`Settings`.

    Raises:
        ConfigError: If a file is missing/invalid, holds a secret while being
            group/other accessible (and ``allow_insecure_config`` is false), or
            validation fails.
    """
    path = config_path if config_path is not None else DEFAULT_CONFIG_PATH
    env = environ if environ is not None else dict(os.environ)

    merged: dict[str, Any] = _read_toml(path, required=required_file)
    if not allow_insecure_config and _contains_secret(merged):
        check_file_permissions(path)
    for env_name, key in _ENV_MAP.items():
        value = env.get(env_name)
        if value is not None and value != "":
            _set_nested(merged, key, value)
    for key, value in (cli_overrides or {}).items():
        if value is not None:
            _set_nested(merged, key, value)

    try:
        return Settings.model_validate(merged)
    except ValidationError as exc:
        raise ConfigError(_safe_validation_message(exc)) from exc


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
        "--allow-insecure-config",
        action="store_true",
        help=(
            "start even if the config file holds a secret and is group/other "
            "accessible (for containers/CI; env ALLOW_INSECURE_CONFIG=1)"
        ),
    )
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


#: Values of ``ALLOW_INSECURE_CONFIG`` that count as "on".
_TRUTHY = frozenset({"1", "true", "yes", "on"})


def resolve_allow_insecure(args: argparse.Namespace, environ: dict[str, str] | None = None) -> bool:
    """Resolve the insecure-config override from the flag or the environment.

    Args:
        args: The parsed CLI namespace (its ``allow_insecure_config`` flag).
        environ: Environment mapping (defaults to :data:`os.environ`).

    Returns:
        ``True`` when the override is requested by the flag or by
        ``ALLOW_INSECURE_CONFIG`` being set to a truthy value.
    """
    if getattr(args, "allow_insecure_config", False):
        return True
    env = environ if environ is not None else dict(os.environ)
    return env.get("ALLOW_INSECURE_CONFIG", "").strip().lower() in _TRUTHY
