"""Shared constants: roles, rights, command flags, lock bits, and defaults.

These mirror the vocabularies that the original C implementation kept in its
``tables.h`` / ``locks.def`` headers, expressed as plain Python ``enum`` members
so they can be compared, serialised, and rendered without magic numbers.
"""

from __future__ import annotations

from enum import Enum, IntEnum, IntFlag

# --- Roles and rights ------------------------------------------------------


class Role(IntEnum):
    """The caller's resolved role in a chat.

    ``ANYONE`` covers ordinary members; ``SUDO`` is not a real Telegram role but
    a bot-level grant configured through ``sudo_ids``. Sudo and owner both
    bypass per-command right checks.
    """

    ANYONE = 0
    ADMIN = 1
    OWNER = 2
    SUDO = 3


class Right(IntFlag):
    """A Telegram administrator right the bot cares about.

    ``NONE`` means no right is required. The remaining bits mirror the
    ``can_*`` fields of a ``ChatMemberAdministrator`` object.
    """

    NONE = 0
    RESTRICT = 1 << 0  # can_restrict_members
    DELETE = 1 << 1  # can_delete_messages
    PIN = 1 << 2  # can_pin_messages
    PROMOTE = 1 << 3  # can_promote_members
    CHANGE_INFO = 1 << 4  # can_change_info


class CommandFlag(IntFlag):
    """Per-command behaviour flags, mirroring the old ``CMDF_*`` bitmask."""

    NONE = 0
    TARGET = 1 << 0  # consumes a leading [target] token
    REPLY_REQUIRED = 1 << 1  # requires the command to be a reply
    DURATION = 1 << 2  # accepts a duration suffix
    REASON = 1 << 3  # accepts a free-text reason
    GROUP_ONLY = 1 << 4  # unavailable in private chats
    HIDDEN = 1 << 5  # omitted from /help and setMyCommands
    ADMIN_TARGET = 1 << 6  # the target may be an administrator (own guards)


# --- Message locks ---------------------------------------------------------


class LockType(IntEnum):
    """A lockable message category, one bit in the ``chats.locks`` bitmask."""

    LINKS = 0
    MEDIA = 1
    STICKERS = 2
    FORWARDS = 3
    POLLS = 4
    INVITES = 5
    BOTS = 6
    COMMANDS = 7


LOCK_NAMES: dict[str, LockType] = {lt.name.lower(): lt for lt in LockType}


# --- API rate-limit buckets ------------------------------------------------


class Bucket(Enum):
    """Rate-limit class for a Bot API method."""

    GLOBAL = "global"
    PER_CHAT = "per_chat"
    BULK = "bulk"


# --- Defaults --------------------------------------------------------------


DEFAULT_WARN_LIMIT = 3
DEFAULT_WARN_ACTION = "mute"
DEFAULT_CAPTCHA_TIMEOUT = 120
DEFAULT_FLOOD_WINDOW = 10
DEFAULT_FLOOD_MUTE_SECONDS = 300

ADMIN_CACHE_TTL_SECONDS = 600
EXPIRY_INTERVAL_SECONDS = 30
CAPTCHA_SWEEP_INTERVAL_SECONDS = 15
BLACKLIST_CACHE_TTL_SECONDS = 300
FLOOD_WINDOW_TTL_SECONDS = 600
FLOOD_SLOTS = 16
MAX_BLACKLIST_PATTERNS = 16
MAX_WARNS_PAGE = 16

# Rate-limit parameters (mirroring SPEC section 4.2 / telegram-api-notes.md).
GLOBAL_RATE_PER_SECOND = 25.0
PER_CHAT_MESSAGE_RATE_PER_MINUTE = 15.0
PER_CHAT_BULK_RATE_PER_SECOND = 1.0

# Retry policy.
MAX_RETRIES = 5
BACKOFF_SCHEDULE_SECONDS = (1, 2, 4, 8, 16, 30)

# Telegram limits.
TELEGRAM_MAX_MESSAGE_LEN = 4096
TELEGRAM_MAX_COMMAND_NAME_LEN = 32
TELEGRAM_MAX_COMMAND_DESC_LEN = 256
TELEGRAM_MAX_ADMIN_TITLE_LEN = 16
DELETE_BATCH_SIZE = 100
PURGE_MAX_IDS = 1000
