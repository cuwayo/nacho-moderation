"""A reusable periodic background task.

Both the expiry sweeper and the captcha sweeper run a coroutine every N seconds
on the event loop. This base class owns the task lifecycle so subclasses only
implement :meth:`_tick`.
"""

from __future__ import annotations

import asyncio
import contextlib

from nacho_moderator.logging_setup import get_logger

_log = get_logger("periodic")


class PeriodicTask:
    """Run :meth:`_tick` every ``interval`` seconds until cancelled."""

    def __init__(self, interval: float, name: str) -> None:
        self._interval = interval
        self._name = name
        self._task: asyncio.Task[None] | None = None

    async def _tick(self) -> None:  # pragma: no cover - overridden
        """Perform one pass of work."""
        raise NotImplementedError

    async def _run(self) -> None:
        while True:
            await asyncio.sleep(self._interval)
            try:
                await self._tick()
            except asyncio.CancelledError:
                raise
            except Exception as exc:
                _log.error("%s sweep failed: %s", self._name, exc)

    def start(self) -> None:
        """Start the background task if it is not already running."""
        if self._task is None:
            self._task = asyncio.create_task(self._run())

    async def stop(self) -> None:
        """Cancel and await the background task."""
        if self._task is not None:
            self._task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await self._task
            self._task = None
