"""Runtime health metrics for the soak test: handler latency and event-loop lag.

The bot records how long each update's handler takes and how far the event loop
drifts from its intended schedule. Latency is kept in a bounded histogram so
memory does not grow with the run; loop lag is kept as timestamped samples (10 Hz,
bounded) so it can be split into a warm-up window and steady state and the
largest events reported with their elapsed time. A summary can be dumped on
``SIGUSR1`` (or at shutdown) as one structured log line (plus the top lag events)
that ``scripts/soak_check.py`` parses.

The percentile from a histogram is the *upper edge* of the bucket the quantile
falls in, which is the conservative (worst-case) reading — the right direction
for a pass/fail gate. The open-ended top bucket has no finite edge, so it reports
the observed maximum instead of infinity.
"""

from __future__ import annotations

import asyncio
import contextlib
import math
from collections.abc import Awaitable, Callable
from dataclasses import dataclass

from nacho_moderator.clock import Clock
from nacho_moderator.logging_setup import get_logger

_log = get_logger("metrics")

#: Upper edges, in milliseconds, for the latency histogram. Dense around the
#: 50 ms latency budget so a failure is not hidden by a coarse bucket.
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

#: The lag warm-up window, in seconds. Lag samples in the first window are the
#: process/allocator warm-up (DB open, the first 100-update batch); the
#: steady-state budgets apply only after it. Matches the RSS warm-up in
#: ``scripts/soak_check.py``.
LAG_WARMUP_SECONDS = 60.0

#: How many of the largest lag events to report (with elapsed time), so a
#: post-warm-up stall is visible instead of hidden inside a max.
LAG_TOP_EVENTS = 10

#: Upper bound on retained lag samples. At the monitor's 10 Hz this is ~5.5 hours,
#: so a soak never hits it, but a pathological run stays bounded.
MAX_LAG_SAMPLES = 200_000


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
        """Return the conservative value at ``quantile`` (0-100).

        The open-ended top bucket (``inf``) reports the observed maximum, so a
        p99 that lands there is a finite, actionable number rather than infinity.
        """
        if self.count == 0:
            return 0.0
        target = (quantile / 100.0) * self.count
        cumulative = 0
        for edge, count in zip(self._edges, self._counts, strict=True):
            cumulative += count
            if cumulative >= target:
                return self.maximum if math.isinf(edge) else edge
        return self.maximum

    @property
    def mean(self) -> float:
        """Return the mean of every recorded sample."""
        return self.total / self.count if self.count else 0.0


@dataclass(frozen=True)
class LagEvent:
    """One event-loop-lag sample, with the elapsed time it was taken at."""

    elapsed_seconds: float
    lag_ms: float


def percentile_ms(values: list[float], quantile: float) -> float:
    """Return the nearest-rank percentile of ``values`` (0-100), or 0 if empty."""
    if not values:
        return 0.0
    ordered = sorted(values)
    rank = math.ceil((quantile / 100.0) * len(ordered))
    index = max(0, min(len(ordered) - 1, rank - 1))
    return ordered[index]


@dataclass(frozen=True)
class MetricsSnapshot:
    """A point-in-time summary of the recorded metrics."""

    latency_count: int
    latency_p50_ms: float
    latency_p99_ms: float
    latency_max_ms: float
    #: Whole-run loop lag (both windows), reported as a note.
    loop_lag_p99_ms: float
    loop_lag_max_ms: float
    #: Warm-up window (first ``LAG_WARMUP_SECONDS``): only a startup hang fails.
    loop_lag_warmup_max_ms: float
    #: Steady-state window (after the warm-up): the real budgets apply here.
    loop_lag_steady_p99_ms: float
    loop_lag_steady_max_ms: float
    loop_lag_warmup_seconds: float = LAG_WARMUP_SECONDS
    #: Sum of every handler's wall duration. Divided by ``uptime_seconds`` it is
    #: an aggregate handler-time fraction (handlers run as concurrent tasks, so it
    #: can exceed 1 — it is an aggregate, not wall-clock loop-busy time).
    latency_total_seconds: float = 0.0
    #: The largest lag events, newest-largest first, for the report.
    lag_events: tuple[LagEvent, ...] = ()
    #: Seconds from the first recorded sample to this snapshot. The soak gate
    #: divides ``latency_count`` by this to get the *achieved* update rate, so the
    #: denominator is the bot's own processing window rather than the harness's
    #: wall-clock (which includes startup and shutdown drain).
    uptime_seconds: float = 0.0

    def format_line(self) -> str:
        """Render the structured, single-line summary the soak parser reads."""
        return (
            f"soak-metrics latency_count={self.latency_count} "
            f"latency_p50_ms={self.latency_p50_ms:.3f} "
            f"latency_p99_ms={self.latency_p99_ms:.3f} "
            f"latency_max_ms={self.latency_max_ms:.3f} "
            f"loop_lag_p99_ms={self.loop_lag_p99_ms:.3f} "
            f"loop_lag_max_ms={self.loop_lag_max_ms:.3f} "
            f"loop_lag_warmup_max_ms={self.loop_lag_warmup_max_ms:.3f} "
            f"loop_lag_steady_p99_ms={self.loop_lag_steady_p99_ms:.3f} "
            f"loop_lag_steady_max_ms={self.loop_lag_steady_max_ms:.3f} "
            f"loop_lag_warmup_seconds={self.loop_lag_warmup_seconds:.3f} "
            f"latency_total_seconds={self.latency_total_seconds:.3f} "
            f"uptime_seconds={self.uptime_seconds:.3f}"
        )

    def format_lag_events(self) -> list[str]:
        """Render the largest lag events, one line each."""
        return [
            f"soak-lag-event elapsed_s={event.elapsed_seconds:.3f} lag_ms={event.lag_ms:.3f}"
            for event in self.lag_events
        ]


class Metrics:
    """Collects handler-latency and event-loop-lag samples.

    Takes an optional :class:`~nacho_moderator.clock.Clock` so it can time-stamp
    lag samples and report the processing window (``uptime_seconds``) the soak
    gate uses to derive the achieved rate. Without a clock every sample is treated
    as occurring at elapsed ``0``.
    """

    def __init__(self, clock: Clock | None = None) -> None:
        self._clock = clock
        self._latency = Histogram()
        self._lag: list[LagEvent] = []
        self._start: float | None = None

    def _mark_start(self) -> None:
        if self._clock is not None and self._start is None:
            self._start = self._clock.monotonic()

    def _elapsed(self) -> float:
        if self._clock is None or self._start is None:
            return 0.0
        return max(0.0, self._clock.monotonic() - self._start)

    def record_handler_latency(self, seconds: float) -> None:
        """Record one handler's wall duration, in seconds."""
        self._mark_start()
        self._latency.record(max(seconds, 0.0) * 1000.0)

    def record_loop_lag(self, seconds: float) -> None:
        """Record one event-loop scheduling delay, in seconds."""
        self._mark_start()
        if len(self._lag) < MAX_LAG_SAMPLES:
            self._lag.append(LagEvent(self._elapsed(), max(seconds, 0.0) * 1000.0))

    def _lag_windows(self) -> tuple[list[float], list[float]]:
        """Return the warm-up and steady-state lag values (in ms)."""
        warmup: list[float] = []
        steady: list[float] = []
        for event in self._lag:
            (warmup if event.elapsed_seconds < LAG_WARMUP_SECONDS else steady).append(event.lag_ms)
        return warmup, steady

    def snapshot(self) -> MetricsSnapshot:
        """Return the current summary."""
        warmup, steady = self._lag_windows()
        all_lags = [event.lag_ms for event in self._lag]
        top = sorted(self._lag, key=lambda event: event.lag_ms, reverse=True)[:LAG_TOP_EVENTS]
        return MetricsSnapshot(
            latency_count=self._latency.count,
            latency_p50_ms=self._latency.percentile(50),
            latency_p99_ms=self._latency.percentile(99),
            latency_max_ms=self._latency.maximum,
            loop_lag_p99_ms=percentile_ms(all_lags, 99),
            loop_lag_max_ms=max(all_lags, default=0.0),
            loop_lag_warmup_max_ms=max(warmup, default=0.0),
            loop_lag_steady_p99_ms=percentile_ms(steady, 99),
            loop_lag_steady_max_ms=max(steady, default=0.0),
            latency_total_seconds=self._latency.total / 1000.0,
            lag_events=tuple(top),
            uptime_seconds=self._elapsed(),
        )

    def dump(self) -> None:
        """Log the current summary, then the largest lag events."""
        snapshot = self.snapshot()
        _log.info("%s", snapshot.format_line())
        for line in snapshot.format_lag_events():
            _log.info("%s", line)


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
