"""Error classes with an attached log level and retry policy.

Mirrors the original ``errors.def`` table: each class carries a log level and
whether it is safe to retry. Handlers raise these; the central error handler and
the request middleware consult the metadata.
"""

from __future__ import annotations

import enum


class LogLevel(enum.IntEnum):
    """Severity for a logged event, ordered from least to most verbose."""

    ERROR = 0
    WARN = 1
    INFO = 2
    DEBUG = 3


class ErrorClass(enum.Enum):
    """A category of failure, carrying its log level and retry policy."""

    def __init__(self, log_level: LogLevel, retry: bool, brief: str) -> None:
        self.log_level = log_level
        self.retry = retry
        self.brief = brief

    INVAL = (LogLevel.ERROR, False, "Invalid argument or malformed input")
    NOMEM = (LogLevel.ERROR, False, "Out of memory")
    NET = (LogLevel.WARN, True, "Network failure (connect, reset, timeout)")
    TLS = (LogLevel.ERROR, False, "TLS handshake or certificate verification failure")
    HTTP = (LogLevel.WARN, True, "Unexpected HTTP status from the Bot API")
    TG_API = (LogLevel.WARN, False, "Bot API returned ok=false with a description")
    RATE_LIMIT = (LogLevel.INFO, True, "HTTP 429; honour retry_after before retrying")
    DB = (LogLevel.ERROR, False, "Database error")
    PARSE = (LogLevel.WARN, False, "JSON or response parsing failure")
    PROTO = (LogLevel.ERROR, False, "Protocol violation")
    PERM = (LogLevel.INFO, False, "Caller or bot lacks the required permission")


class BotError(Exception):
    """Base class for expected, handled failures raised by the bot."""

    error_class: ErrorClass = ErrorClass.INVAL

    def __init__(self, message: str = "") -> None:
        super().__init__(message or self.error_class.brief)
        self.message = message or self.error_class.brief


class ConfigError(BotError):
    """A configuration file, environment, or CLI value is invalid."""

    error_class = ErrorClass.INVAL


class DatabaseError(BotError):
    """A database operation failed."""

    error_class = ErrorClass.DB


class PermissionDeniedError(BotError):
    """The caller or the bot lacks a required right."""

    error_class = ErrorClass.PERM


class BotLacksRightError(PermissionDeniedError):
    """The bot itself does not hold the right needed for the command."""


class ParseError(BotError):
    """Untrusted input could not be parsed."""

    error_class = ErrorClass.PARSE
