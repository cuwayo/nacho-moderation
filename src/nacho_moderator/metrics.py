"""Runtime health metrics for the soak test: handler latency and event-loop lag.

The bot records how long each update's handler takes and how far the event loop
drifts from its intended schedule. Both are kept in bounded histograms so memory
does not grow with the run, and a summary can be dumped on ``SIGUSR1`` (or at
shutdown) as one structured log line that ``scripts/soak_check.py`` parses.

The percentile from a histogram is the *upper edge* of the bucket the quantile
falls in, which is the conservative (worst-case) reading — the right direction
for a pass/fail gate.
"""

from __future__ import annotations

import asyncio
import contextlib
from collections.abc import Awaitable, Callable
from dataclasses import dataclass

from nacho_moderator.clock import Clock
from nacho_moderator.logging_setup import get_logger

_log = get_logger("metrics")

#: Upper edges, in milliseconds, for the latency/loop-lag histograms. Dense
#: around the 50 ms latency and 100 ms loop-lag budgets so a failure is not
#: hidden by a coarse bucket.
_BUCKET_EDGES_MS: tuple[float, ...] = (
    0.5,
    1.0,
    2.0,
    3.0,
    5.0,
    7.0,
    10.0,
    15.0,
    20.0,
    30.0,
    40.0,
    50.0,
    75.0,
    100.0,
    150.0,
    250.0,
    500.0,
    1000.0,
    float("inf"),
)


class Histogram:
    """A fixed-bucket histogram with cumulative percentiles.

    Memory is bounded by the number of buckets, so it is safe to record one
    sample per update for the whole run.
    """

    def __init__(self, edges: tuple[float, ...] = _BUCKET_EDGES_MS) -> None:
        self._edges = edges
        self._counts = [0] * len(edges)
        self.count = 0
        self.total = 0.0
        self.maximum = 0.0

    def record(self, value: float) -> None:
        """Add one sample."""
        self.count += 1
        self.total += value
        self.maximum = max(self.maximum, value)
        for index, edge in enumerate(self._edges):
            if value <= edge:
                self._counts[index] += 1
                return

    def percentile(self, quantile: float) -> float:
        """Return the conservative value at ``quantile`` (0-100)."""
        if self.count == 0:
            return 0.0
        target = (quantile / 100.0) * self.count
        cumulative = 0
        for edge, count in zip(self._edges, self._counts, strict=True):
            cumulative += count
            if cumulative >= target:
                return edge
        return self._edges[-1]

    @property
    def mean(self) -> float:
        """Return the mean of every recorded sample."""
        return self.total / self.count if self.count else 0.0


@dataclass(frozen=True)
class MetricsSnapshot:
    """A point-in-time summary of the recorded metrics."""

    latency_count: int
    latency_p50_ms: float
    latency_p99_ms: float
    latency_max_ms: float
    loop_lag_p99_ms: float
    loop_lag_max_ms: float

    def format_line(self) -> str:
        """Render the structured, single-line summary the soak parser reads."""
        return (
            f"soak-metrics latency_count={self.latency_count} "
            f"latency_p50_ms={self.latency_p50_ms:.3f} "
            f"latency_p99_ms={self.latency_p99_ms:.3f} "
            f"latency_max_ms={self.latency_max_ms:.3f} "
            f"loop_lag_p99_ms={self.loop_lag_p99_ms:.3f} "
            f"loop_lag_max_ms={self.loop_lag_max_ms:.3f}"
        )


class Metrics:
    """Collects handler-latency and event-loop-lag samples."""

    def __init__(self) -> None:
        self._latency = Histogram()
        self._loop_lag = Histogram()

    def record_handler_latency(self, seconds: float) -> None:
        """Record one handler's wall duration, in seconds."""
        self._latency.record(max(seconds, 0.0) * 1000.0)

    def record_loop_lag(self, seconds: float) -> None:
        """Record one event-loop scheduling delay, in seconds."""
        self._loop_lag.record(max(seconds, 0.0) * 1000.0)

    def snapshot(self) -> MetricsSnapshot:
        """Return the current summary."""
        return MetricsSnapshot(
            latency_count=self._latency.count,
            latency_p50_ms=self._latency.percentile(50),
            latency_p99_ms=self._latency.percentile(99),
            latency_max_ms=self._latency.maximum,
            loop_lag_p99_ms=self._loop_lag.percentile(99),
            loop_lag_max_ms=self._loop_lag.maximum,
        )

    def dump(self) -> None:
        """Log the current summary as one structured line."""
        _log.info("%s", self.snapshot().format_line())


class LoopLagMonitor:
    """Measures how late the event loop runs a fixed-interval timer.

    Each cycle measures how much the sleep overshot its requested duration: the
    loop lag is ``actual - interval``. This is a per-cycle scheduling delay, so
    a small constant overshoot (a few ms) does not accumulate into a runaway
    reading — only a genuinely blocked loop produces a large value.
    """

    def __init__(
        self,
        metrics: Metrics,
        clock: Clock,
        *,
        interval: float = 0.1,
        sleep: Callable[[float], Awaitable[None]] = asyncio.sleep,
    ) -> None:
        self._metrics = metrics
        self._clock = clock
        self._interval = interval
        self._sleep = sleep
        self._task: asyncio.Task[None] | None = None

    async def _run(self) -> None:
        while True:
            started = self._clock.monotonic()
            await self._sleep(self._interval)
            elapsed = self._clock.monotonic() - started
            self._metrics.record_loop_lag(elapsed - self._interval)

    def start(self) -> None:
        """Start the monitor if it is not already running."""
        if self._task is None:
            self._task = asyncio.create_task(self._run())

    async def stop(self) -> None:
        """Cancel and await the monitor."""
        if self._task is not None:
            self._task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await self._task
            self._task = None
