"""asyncio hygiene (P7): tracked tasks, no leaks, cancellation not swallowed.

Every ``asyncio.create_task`` in the package is owned by an object with a
``start``/``stop`` pair; this test asserts that starting and stopping the
background tasks leaves no pending task behind, and that the periodic task
re-raises ``CancelledError`` rather than swallowing it.
"""

from __future__ import annotations

import asyncio

from nacho_moderator.clock import FakeClock
from nacho_moderator.metrics import LoopLagMonitor, Metrics
from nacho_moderator.services.periodic import PeriodicTask


class _Task(PeriodicTask):
    """A periodic task that records its ticks."""

    def __init__(self) -> None:
        super().__init__(0.01, "hygiene")
        self.ticks = 0

    async def _tick(self) -> None:
        self.ticks += 1


async def test_start_stop_leaves_no_pending_tasks() -> None:
    """A periodic task's task is fully awaited on stop."""
    task = _Task()
    task.start()
    await asyncio.sleep(0.03)
    await task.stop()
    await asyncio.sleep(0)  # let the loop settle
    pending = [t for t in asyncio.all_tasks() if t is not asyncio.current_task()]
    assert pending == []


async def test_loop_lag_monitor_leaves_no_pending_tasks() -> None:
    monitor = LoopLagMonitor(Metrics(), FakeClock())
    monitor.start()
    await asyncio.sleep(0.02)
    await monitor.stop()
    await asyncio.sleep(0)
    pending = [t for t in asyncio.all_tasks() if t is not asyncio.current_task()]
    assert pending == []


async def test_periodic_task_propagates_cancellation() -> None:
    """Cancelling the runner raises CancelledError out of _run (not swallowed)."""
    task = _Task()
    runner = asyncio.create_task(task._run())
    await asyncio.sleep(0.02)
    runner.cancel()
    try:
        await runner
    except asyncio.CancelledError:
        pass
    else:  # pragma: no cover - only on failure
        raise AssertionError("CancelledError was swallowed")
    assert runner.cancelled()
