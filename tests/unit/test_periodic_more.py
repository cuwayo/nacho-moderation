"""Extra unit tests for the periodic task lifecycle.

Covers the two gaps in ``test_periodic_webhook.py``: the ``CancelledError``
re-raise inside ``_run`` and the idempotent ``start()`` guard.
"""

from __future__ import annotations

import asyncio

import pytest

from nacho_moderator.services.periodic import PeriodicTask


class _CountingTask(PeriodicTask):
    def __init__(self, interval: float) -> None:
        super().__init__(interval, "test")
        self.ticks = 0

    async def _tick(self) -> None:
        self.ticks += 1


async def test_start_is_idempotent() -> None:
    """Calling start() twice must not spawn a second background task."""
    task = _CountingTask(interval=0.01)
    task.start()
    task.start()
    await asyncio.sleep(0.02)
    await task.stop()
    assert task.ticks >= 1


async def test_cancelled_tick_propagates() -> None:
    """A CancelledError from _tick is re-raised, not swallowed as a failure."""

    class _Cancel(PeriodicTask):
        def __init__(self) -> None:
            super().__init__(0.0, "cancel")

        async def _tick(self) -> None:
            raise asyncio.CancelledError

    task = _Cancel()
    with pytest.raises(asyncio.CancelledError):
        await task._run()
