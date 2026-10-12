"""Automated moderation: locks, blacklist, and antiflood.

Every group message runs through this service *before* command dispatch, in the
fixed order locks -> blacklist -> antiflood. Administrators, the owner, and sudo
users are exempt; messages from other bots are skipped unless the ``bots`` lock
is set. Locks and blacklist matches are terminal (the message is deleted);
antiflood counts messages in a sliding window and mutes on overflow.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum

import regex

from nacho_moderator.clock import Clock
from nacho_moderator.constants import (
    BLACKLIST_CACHE_TTL_SECONDS,
    DEFAULT_FLOOD_MUTE_SECONDS,
    DEFAULT_FLOOD_WINDOW,
    FLOOD_SLOTS,
    FLOOD_WINDOW_TTL_SECONDS,
    MAX_BLACKLIST_PATTERNS,
    LockType,
)
from nacho_moderator.db import BlacklistEntry, Database
from nacho_moderator.logging_setup import get_logger
from nacho_moderator.services.cache import TtlCache

_log = get_logger("automod")

# Regex match limits (the Python stand-in for PCRE2's match/depth limits).
_REGEX_TIMEOUT_SECONDS = 0.05

_INVITE_MARKERS = ("t.me/joinchat", "t.me/+", "telegram.me/joinchat")


class Verdict(Enum):
    """The outcome of scanning a message."""

    ALLOW = "allow"
    DELETE = "delete"
    MUTE = "mute"


@dataclass
class ModerationResult:
    """What automod decided to do with a message."""

    verdict: Verdict = Verdict.ALLOW
    reason: str = ""
    #: For DELETE, the message ids to remove (the offending message, plus any
    #: burst ids for antiflood).
    delete_message_ids: list[int] = field(default_factory=list)
    #: For MUTE, the number of seconds the mute should last.
    mute_seconds: int = 0


@dataclass
class _FloodWindow:
    """A sliding window of recent message timestamps and ids for one user."""

    timestamps: list[float] = field(default_factory=list)
    message_ids: list[int] = field(default_factory=list)


class AutomodService:
    """Locks, blacklist, and antiflood over group messages."""

    def __init__(self, db: Database, clock: Clock) -> None:
        self._db = db
        self._clock = clock
        self._blacklist = TtlCache[list[BlacklistEntry]](
            clock, BLACKLIST_CACHE_TTL_SECONDS, capacity=512
        )
        self._flood = TtlCache[_FloodWindow](clock, FLOOD_WINDOW_TTL_SECONDS, capacity=4096)

    # --- lock classification ----------------------------------------------

    @staticmethod
    def locked_content(
        locks: int,
        *,
        text: str,
        has_media: bool,
        has_sticker: bool,
        has_forward: bool,
        has_poll: bool,
        from_bot: bool,
        entity_types: set[str],
    ) -> set[LockType]:
        """Return the lock types a message matches, restricted to active locks.

        Args:
            locks: The chat's lock bitmask.
            text: The message text (or caption).
            has_media: Whether the message carries a media attachment.
            has_sticker: Whether the message is a sticker or animation.
            has_forward: Whether the message is forwarded.
            has_poll: Whether the message is a poll.
            from_bot: Whether the sender is a bot.
            entity_types: The message's entity types (e.g. ``{"url"}``).

        Returns:
            The set of active lock types the message violates.
        """
        hits: set[LockType] = set()
        if has_media:
            hits.add(LockType.MEDIA)
        if has_sticker:
            hits.add(LockType.STICKERS)
        if has_forward:
            hits.add(LockType.FORWARDS)
        if has_poll:
            hits.add(LockType.POLLS)
        if from_bot:
            hits.add(LockType.BOTS)
        if entity_types & {"url", "text_link", "mention"}:
            hits.add(LockType.LINKS)
        if "bot_command" in entity_types:
            hits.add(LockType.COMMANDS)
        lowered = text.lower()
        if any(marker in lowered for marker in _INVITE_MARKERS):
            hits.add(LockType.INVITES)
        return {lt for lt in hits if locks & (1 << int(lt))}

    # --- blacklist ---------------------------------------------------------

    async def _blacklist_patterns(self, chat_id: int) -> list[BlacklistEntry]:
        key = str(chat_id)
        cached = self._blacklist.get(key)
        if cached is not None:
            return cached
        entries = await self._db.blacklist_list(chat_id, MAX_BLACKLIST_PATTERNS)
        self._blacklist.set(key, entries)
        return entries

    def invalidate_blacklist(self, chat_id: int) -> None:
        """Drop the cached blacklist for a chat after a write."""
        self._blacklist.invalidate(str(chat_id))

    @staticmethod
    def _blacklist_match(entries: list[BlacklistEntry], body: str) -> str | None:
        """Return the first matching pattern, or ``None``."""
        lowered = body.lower()
        for entry in entries:
            if entry.is_regex:
                try:
                    compiled = regex.compile(entry.pattern, regex.IGNORECASE)
                    if compiled.search(body, timeout=_REGEX_TIMEOUT_SECONDS):
                        return entry.pattern
                except (regex.error, TimeoutError):
                    # An invalid or catastrophic pattern never matches.
                    continue
            elif entry.pattern.lower() in lowered:
                return entry.pattern
        return None

    # --- antiflood ---------------------------------------------------------

    def _track_flood(
        self, chat_id: int, user_id: int, message_id: int, window_seconds: int
    ) -> tuple[int, list[int]]:
        """Record a message and return the in-window count and message ids."""
        key = f"{chat_id}:{user_id}"
        window = self._flood.get(key)
        if window is None:
            window = _FloodWindow()
        now = self._clock.monotonic()
        cutoff = now - window_seconds
        # Drop entries older than the window.
        kept_ts = [t for t in window.timestamps if t >= cutoff]
        kept_ids = window.message_ids[len(window.message_ids) - len(kept_ts) :]
        if len(kept_ts) >= FLOOD_SLOTS:
            kept_ts = kept_ts[1:]
            kept_ids = kept_ids[1:]
        kept_ts.append(now)
        kept_ids.append(message_id)
        window.timestamps = kept_ts
        window.message_ids = kept_ids
        self._flood.set(key, window)
        return len(kept_ts), list(kept_ids)

    # --- entry point -------------------------------------------------------

    async def scan(
        self,
        *,
        chat_id: int,
        user_id: int,
        message_id: int,
        text: str,
        locks: int,
        flood_limit: int,
        flood_window: int,
        is_bot: bool,
        has_media: bool = False,
        has_sticker: bool = False,
        has_forward: bool = False,
        has_poll: bool = False,
        entity_types: set[str] | None = None,
    ) -> ModerationResult:
        """Run the full locks -> blacklist -> antiflood pipeline.

        Returns:
            A :class:`ModerationResult` describing any action to take.
        """
        entities = entity_types or set()

        # Bots are skipped entirely unless the bots lock is active.
        bots_locked = bool(locks & (1 << int(LockType.BOTS)))
        if is_bot and not bots_locked:
            return ModerationResult()

        matched_locks = self.locked_content(
            locks,
            text=text,
            has_media=has_media,
            has_sticker=has_sticker,
            has_forward=has_forward,
            has_poll=has_poll,
            from_bot=is_bot,
            entity_types=entities,
        )
        if matched_locks:
            reason = sorted(lt.name.lower() for lt in matched_locks)[0]
            return ModerationResult(Verdict.DELETE, reason, [message_id])

        if text:
            pattern = self._blacklist_match(await self._blacklist_patterns(chat_id), text)
            if pattern is not None:
                return ModerationResult(Verdict.DELETE, pattern, [message_id])

        if flood_limit > 0:
            window = flood_window if flood_window > 0 else DEFAULT_FLOOD_WINDOW
            count, ids = self._track_flood(chat_id, user_id, message_id, window)
            if count > flood_limit:
                return ModerationResult(
                    Verdict.MUTE,
                    "antiflood",
                    ids,
                    DEFAULT_FLOOD_MUTE_SECONDS,
                )

        return ModerationResult()
