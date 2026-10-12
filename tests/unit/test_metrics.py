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
    assert hist.percentile(99) == float("inf")
    assert hist.maximum == 35.0
    assert hist.mean == 20.0


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


def test_metrics_negative_clamped() -> None:
    metrics = Metrics()
    metrics.record_handler_latency(-1.0)
    assert metrics.snapshot().latency_max_ms == 0.0


def test_snapshot_format_line_parses_back() -> None:
    snap = MetricsSnapshot(
        latency_count=10,
        latency_p50_ms=1.0,
        latency_p99_ms=2.0,
        latency_max_ms=3.0,
        loop_lag_p99_ms=4.0,
        loop_lag_max_ms=5.0,
    )
    line = snap.format_line()
    assert line.startswith("soak-metrics ")
    fields = dict(part.split("=") for part in line.split()[1:])
    assert fields["latency_count"] == "10"
    assert fields["latency_p99_ms"] == "2.000"
    assert fields["loop_lag_max_ms"] == "5.000"


def test_metrics_dump_logs_a_line(caplog: pytest.LogCaptureFixture) -> None:
    """dump() emits the structured summary at INFO."""
    import logging

    metrics = Metrics()
    metrics.record_handler_latency(0.001)
    with caplog.at_level(logging.INFO, logger="nacho_moderator.metrics"):
        metrics.dump()
    assert any("soak-metrics" in record.getMessage() for record in caplog.records)


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
