"""Tests for the runtime metrics module (P5)."""

from __future__ import annotations

import asyncio

import pytest

from nacho_moderator.clock import FakeClock
from nacho_moderator.metrics import (
    Histogram,
    LoopLagMonitor,
    Metrics,
    MetricsSnapshot,
)


def test_histogram_empty() -> None:
    hist = Histogram()
    assert hist.count == 0
    assert hist.percentile(50) == 0.0
    assert hist.mean == 0.0
    assert hist.maximum == 0.0


def test_histogram_percentile_is_upper_edge() -> None:
    hist = Histogram(edges=(10.0, 20.0, 30.0, float("inf")))
    for value in (5.0, 15.0, 25.0, 35.0):
        hist.record(value)
    assert hist.count == 4
    assert hist.percentile(50) == 20.0  # 2nd sample lands in the (20] bucket
    # The open-ended top bucket reports the observed maximum, not infinity.
    assert hist.percentile(99) == 35.0
    assert hist.maximum == 35.0
    assert hist.mean == 20.0


def test_histogram_overflow_bucket_is_finite() -> None:
    """A p99 in the open-ended bucket is the real max, never ``inf``."""
    hist = Histogram()
    for value in (1.0, 2.0, 5.0):
        hist.record(value)
    hist.record(3000.0)  # lands in the inf bucket
    p99 = hist.percentile(99)
    assert p99 == 3000.0
    assert p99 != float("inf")


def test_percentile_ms_nearest_rank() -> None:
    from nacho_moderator.metrics import percentile_ms

    assert percentile_ms([], 99) == 0.0
    assert percentile_ms([5.0], 99) == 5.0
    assert percentile_ms([1.0, 2.0, 3.0, 4.0, 100.0], 99) == 100.0
    assert percentile_ms([1.0, 2.0, 3.0, 4.0, 100.0], 50) == 3.0


def test_histogram_maximum_tracks_largest() -> None:
    hist = Histogram()
    hist.record(3.0)
    hist.record(500.0)
    hist.record(1.0)
    assert hist.maximum == 500.0


def test_metrics_records_latency_and_lag() -> None:
    metrics = Metrics()
    metrics.record_handler_latency(0.010)
    metrics.record_handler_latency(0.040)
    metrics.record_loop_lag(0.005)
    snap = metrics.snapshot()
    assert snap.latency_count == 2
    assert snap.latency_max_ms == 40.0
    assert snap.loop_lag_max_ms == 5.0
    assert snap.latency_total_seconds == pytest.approx(0.050)


def test_metrics_negative_clamped() -> None:
    metrics = Metrics()
    metrics.record_handler_latency(-1.0)
    assert metrics.snapshot().latency_max_ms == 0.0


def test_lag_split_into_warmup_and_steady() -> None:
    """Lag samples are windowed by elapsed time: first 60 s is warm-up."""
    clock = FakeClock()
    metrics = Metrics(clock)
    metrics.record_loop_lag(0.010)  # t=0, warm-up
    clock.advance(30.0)
    metrics.record_loop_lag(0.500)  # t=30, warm-up (a startup spike)
    clock.advance(60.0)
    metrics.record_loop_lag(0.020)  # t=90, steady
    clock.advance(5.0)
    metrics.record_loop_lag(0.150)  # t=95, steady
    snap = metrics.snapshot()
    assert snap.loop_lag_warmup_max_ms == pytest.approx(500.0)
    assert snap.loop_lag_steady_max_ms == pytest.approx(150.0)
    assert snap.loop_lag_steady_p99_ms == pytest.approx(150.0)
    # Whole-run max still sees the warm-up spike.
    assert snap.loop_lag_max_ms == pytest.approx(500.0)


def test_lag_top_events_are_largest_first() -> None:
    clock = FakeClock()
    metrics = Metrics(clock)
    for lag_ms in (5.0, 400.0, 20.0, 250.0, 10.0):
        metrics.record_loop_lag(lag_ms / 1000.0)
        clock.advance(1.0)
    snap = metrics.snapshot()
    lags = [event.lag_ms for event in snap.lag_events]
    assert lags == sorted(lags, reverse=True)
    assert lags[:3] == [400.0, 250.0, 20.0]


def test_snapshot_format_line_parses_back() -> None:
    snap = MetricsSnapshot(
        latency_count=10,
        latency_p50_ms=1.0,
        latency_p99_ms=2.0,
        latency_max_ms=3.0,
        loop_lag_p99_ms=4.0,
        loop_lag_max_ms=5.0,
        loop_lag_warmup_max_ms=6.0,
        loop_lag_steady_p99_ms=7.0,
        loop_lag_steady_max_ms=8.0,
        latency_total_seconds=9.0,
        uptime_seconds=30.0,
    )
    line = snap.format_line()
    assert line.startswith("soak-metrics ")
    fields = dict(part.split("=") for part in line.split()[1:])
    assert fields["latency_count"] == "10"
    assert fields["latency_p99_ms"] == "2.000"
    assert fields["loop_lag_max_ms"] == "5.000"
    assert fields["loop_lag_warmup_max_ms"] == "6.000"
    assert fields["loop_lag_steady_p99_ms"] == "7.000"
    assert fields["loop_lag_steady_max_ms"] == "8.000"
    assert fields["latency_total_seconds"] == "9.000"
    assert fields["uptime_seconds"] == "30.000"


def test_latency_total_and_uptime() -> None:
    clock = FakeClock()
    metrics = Metrics(clock)
    metrics.record_handler_latency(1.0)
    clock.advance(4.0)
    snap = metrics.snapshot()
    assert snap.latency_total_seconds == pytest.approx(1.0)
    assert snap.uptime_seconds == pytest.approx(4.0)


def test_lag_events_render_lines() -> None:
    clock = FakeClock()
    metrics = Metrics(clock)
    metrics.record_loop_lag(0.2)
    clock.advance(70.0)
    metrics.record_loop_lag(0.3)
    lines = metrics.snapshot().format_lag_events()
    assert any("elapsed_s=" in line and "lag_ms=" in line for line in lines)


def test_metrics_uptime_tracks_the_clock() -> None:
    """The snapshot reports the window from the first sample, for the soak rate."""
    clock = FakeClock()
    metrics = Metrics(clock)
    assert metrics.snapshot().uptime_seconds == 0.0  # no samples yet
    clock.advance(5.0)
    metrics.record_handler_latency(0.001)  # starts the window at t=5
    clock.advance(10.0)
    assert metrics.snapshot().uptime_seconds == pytest.approx(10.0)


def test_metrics_uptime_zero_without_a_clock() -> None:
    metrics = Metrics()
    metrics.record_handler_latency(0.001)
    assert metrics.snapshot().uptime_seconds == 0.0


def test_metrics_dump_logs_a_line(caplog: pytest.LogCaptureFixture) -> None:
    """dump() emits the structured summary at INFO."""
    import logging

    metrics = Metrics()
    metrics.record_handler_latency(0.001)
    with caplog.at_level(logging.INFO, logger="nacho_moderator.metrics"):
        metrics.dump()
    assert any("soak-metrics" in record.getMessage() for record in caplog.records)


def test_metrics_dump_logs_lag_events(caplog: pytest.LogCaptureFixture) -> None:
    """dump() also emits one line per largest lag event."""
    import logging

    metrics = Metrics(FakeClock())
    metrics.record_loop_lag(0.250)
    with caplog.at_level(logging.INFO, logger="nacho_moderator.metrics"):
        metrics.dump()
    assert any("soak-lag-event" in record.getMessage() for record in caplog.records)


def test_metrics_snapshot_empty() -> None:
    snap = Metrics().snapshot()
    assert snap.latency_count == 0
    assert snap.latency_p99_ms == 0.0
    assert snap.loop_lag_max_ms == 0.0


async def test_loop_lag_monitor_records_zero_when_on_time() -> None:
    """A sleep that advances the clock exactly is recorded as ~zero lag."""
    metrics = Metrics()
    clock = FakeClock()

    async def fake_sleep(seconds: float) -> None:
        clock.advance(seconds)
        await asyncio.sleep(0)  # yield so the monitor can be cancelled

    monitor = LoopLagMonitor(metrics, clock, interval=0.1, sleep=fake_sleep)
    monitor.start()
    for _ in range(3):
        await asyncio.sleep(0)
    await monitor.stop()
    assert metrics.snapshot().loop_lag_max_ms == 0.0


async def test_loop_lag_monitor_records_delay() -> None:
    """If the loop oversleeps once, the excess is recorded as lag."""
    metrics = Metrics()
    clock = FakeClock()

    async def fake_sleep(seconds: float) -> None:
        # Sleep 100 ms but advance the clock 150 ms the first time: 50 ms of lag.
        clock.advance(seconds + (0.05 if metrics.snapshot().loop_lag_max_ms == 0.0 else 0.0))
        await asyncio.sleep(0)

    monitor = LoopLagMonitor(metrics, clock, interval=0.1, sleep=fake_sleep)
    monitor.start()
    for _ in range(3):
        await asyncio.sleep(0)
    await monitor.stop()
    assert metrics.snapshot().loop_lag_max_ms == pytest.approx(50.0)


async def test_loop_lag_monitor_stop_is_idempotent() -> None:
    metrics = Metrics()
    monitor = LoopLagMonitor(metrics, FakeClock())
    monitor.start()
    await monitor.stop()
    await monitor.stop()  # no error
    assert monitor._task is None
