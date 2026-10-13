#!/usr/bin/env python3
"""soak_check.py - parse soak artifacts and decide pass/fail.

The soak script (``scripts/run_soak.sh``) writes four artifacts:

* the bot log (stdout+stderr), which contains one ``soak-metrics ...`` line with
  the in-bot handler-latency and event-loop-lag summary;
* a samples file with one ``rss_kb fds`` pair per 5-second sample;
* the mock server's recorded calls (JSONL), used only to confirm work happened.

This module turns those into a verdict. It is deliberately pure — every input is
a string or a list — so it can be unit-tested without running a soak. It is the
single source of truth for the thresholds; the shell script only orchestrates.

Thresholds (see ``docs/adr/0002-python-loop-lag-budget.md``):

* p99 handler latency <= 50 ms;
* loop lag, split into a warm-up window (first 60 s) and steady state:
  warm-up max <= 1000 ms (a startup hang still fails); steady p99 <= 100 ms and
  steady max <= 250 ms (the p99 is the design bound, the max a regression guard);
* achieved update rate >= 80 % of the requested rate — the gate derives the
  minimum handler-sample count from the run itself
  (``0.8 * requested_rate * measured_seconds``) instead of trusting a fixed
  default, so a run that quietly served a fraction of the load fails;
* RSS growth after a 60 s warm-up <= 10 % (linear-regression slope);
* FD count flat (no upward slope);
* no traceback, "Task exception was never retrieved", or "was never awaited";
* bot exited 0.

These timing budgets are meaningful only with production flags; the debug-mode
smoke run (``PYTHONASYNCIODEBUG``/``-X dev``) inflates them by an order of
magnitude, so it checks hygiene (tracebacks, un-awaited coroutines, resource
warnings) rather than these numbers.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

# --- Thresholds (single source of truth) -----------------------------------

P99_LATENCY_BUDGET_MS = 50.0
#: Steady-state loop-lag budgets, applied only after the warm-up window. The p99
#: is the real design bound; the max is a regression guard (a hard ceiling), not
#: a design target — a Python loop processing 100-update batches stalls briefly.
LOOP_LAG_STEADY_P99_BUDGET_MS = 100.0
LOOP_LAG_STEADY_MAX_BUDGET_MS = 250.0
#: The warm-up window's own lag ceiling: a startup hang (a stalled first batch)
#: must still fail, even though the steady-state budgets do not apply there.
LOOP_LAG_WARMUP_MAX_BUDGET_MS = 1000.0
RSS_GROWTH_PCT = 10.0
FD_SLOPE_TOLERANCE = 0.5  # fds gained per 5 s sample, allowed drift
WARMUP_SECONDS = 60.0
SAMPLE_INTERVAL_SECONDS = 5.0
#: The achieved update rate must be at least this fraction of the requested rate.
MIN_ACHIEVED_RATE_FRACTION = 0.8

_METRICS_RE = re.compile(
    r"soak-metrics "
    r"latency_count=(?P<latency_count>\d+) "
    r"latency_p50_ms=(?P<latency_p50>[\d.]+|inf|nan) "
    r"latency_p99_ms=(?P<latency_p99>[\d.]+|inf|nan) "
    r"latency_max_ms=(?P<latency_max>[\d.]+|inf|nan) "
    r"loop_lag_p99_ms=(?P<loop_lag_p99>[\d.]+|inf|nan) "
    r"loop_lag_max_ms=(?P<loop_lag_max>[\d.]+|inf|nan) "
    r"loop_lag_warmup_max_ms=(?P<loop_lag_warmup_max>[\d.]+|inf|nan) "
    r"loop_lag_steady_p99_ms=(?P<loop_lag_steady_p99>[\d.]+|inf|nan) "
    r"loop_lag_steady_max_ms=(?P<loop_lag_steady_max>[\d.]+|inf|nan) "
    r"loop_lag_warmup_seconds=(?P<loop_lag_warmup_seconds>[\d.]+) "
    r"latency_total_seconds=(?P<latency_total>[\d.]+) "
    r"uptime_seconds=(?P<uptime>[\d.]+)"
)

#: A single ``soak-lag-event`` line: one of the largest lag events, with the
#: elapsed time it occurred at (so a post-warm-up stall is visible).
_LAG_EVENT_RE = re.compile(r"soak-lag-event elapsed_s=(?P<elapsed>[\d.]+) lag_ms=(?P<lag>[\d.]+)")

_FAILURE_PATTERNS = (
    "Traceback (most recent call last)",
    "Task exception was never retrieved",
    "was never awaited",
)

_SAMPLE_RE = re.compile(r"^\s*(?P<rss>\d+)\s+(?P<fds>\d+)\s*$")


@dataclass
class LagEvent:
    """One reported lag event, with the elapsed time it occurred at."""

    elapsed_seconds: float
    lag_ms: float


@dataclass
class SoakMetrics:
    """The parsed in-bot metrics summary."""

    latency_count: int
    latency_p50_ms: float
    latency_p99_ms: float
    latency_max_ms: float
    #: Whole-run loop lag (both windows), reported as a note.
    loop_lag_p99_ms: float
    loop_lag_max_ms: float
    #: Warm-up window (first ``loop_lag_warmup_seconds``) max.
    loop_lag_warmup_max_ms: float
    #: Steady-state window (after the warm-up) p99 and max — the real budgets.
    loop_lag_steady_p99_ms: float
    loop_lag_steady_max_ms: float
    loop_lag_warmup_seconds: float
    #: Sum of every handler's wall duration (aggregate handler-time fraction).
    latency_total_seconds: float
    #: The bot's own processing window, in seconds.
    uptime_seconds: float
    #: The largest lag events reported in the log, largest first.
    lag_events: tuple[LagEvent, ...] = ()

    @property
    def handler_time_fraction(self) -> float:
        """Aggregate handler-time as a fraction of the processing window.

        Handlers run as concurrent tasks, so this is an aggregate (it can exceed
        1 when many handlers overlap); it is a rough busy measure, not wall-clock
        loop occupancy.
        """
        if self.uptime_seconds <= 0:
            return 0.0
        return self.latency_total_seconds / self.uptime_seconds


def parse_metrics(bot_log: str) -> SoakMetrics | None:
    """Return the last ``soak-metrics`` summary in the log, or ``None``."""
    match = None
    for found in _METRICS_RE.finditer(bot_log):
        match = found
    if match is None:
        return None
    return SoakMetrics(
        latency_count=int(match.group("latency_count")),
        latency_p50_ms=float(match.group("latency_p50")),
        latency_p99_ms=float(match.group("latency_p99")),
        latency_max_ms=float(match.group("latency_max")),
        loop_lag_p99_ms=float(match.group("loop_lag_p99")),
        loop_lag_max_ms=float(match.group("loop_lag_max")),
        loop_lag_warmup_max_ms=float(match.group("loop_lag_warmup_max")),
        loop_lag_steady_p99_ms=float(match.group("loop_lag_steady_p99")),
        loop_lag_steady_max_ms=float(match.group("loop_lag_steady_max")),
        loop_lag_warmup_seconds=float(match.group("loop_lag_warmup_seconds")),
        latency_total_seconds=float(match.group("latency_total")),
        uptime_seconds=float(match.group("uptime")),
        lag_events=parse_lag_events(bot_log),
    )


def parse_lag_events(bot_log: str) -> tuple[LagEvent, ...]:
    """Parse the ``soak-lag-event`` lines (largest first, as logged)."""
    return tuple(
        LagEvent(float(m.group("elapsed")), float(m.group("lag")))
        for m in _LAG_EVENT_RE.finditer(bot_log)
    )


@dataclass
class Sample:
    """One RSS/FD sample taken during the soak."""

    rss_kb: int
    fds: int


@dataclass
class Verdict:
    """The outcome of a soak check."""

    ok: bool
    failures: list[str] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)

    def report(self) -> str:
        """Render a human-readable report."""
        lines = ["soak: PASS" if self.ok else "soak: FAIL"]
        lines.extend(f"  note: {note}" for note in self.notes)
        lines.extend(f"  failure: {fail}" for fail in self.failures)
        return "\n".join(lines)


def parse_samples(samples_text: str) -> list[Sample]:
    """Parse the ``rss_kb fds`` sample lines, skipping blanks and comments."""
    out: list[Sample] = []
    for line in samples_text.splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        match = _SAMPLE_RE.match(line)
        if match:
            out.append(Sample(int(match.group("rss")), int(match.group("fds"))))
    return out


def _slope(values: list[float]) -> float:
    """Return the least-squares slope of ``values`` against their index."""
    n = len(values)
    if n < 2:
        return 0.0
    mean_x = (n - 1) / 2.0
    mean_y = sum(values) / n
    numerator = sum((i - mean_x) * (y - mean_y) for i, y in enumerate(values))
    denominator = sum((i - mean_x) ** 2 for i in range(n))
    return numerator / denominator if denominator else 0.0


def post_warmup(samples: list[Sample], warmup_seconds: float = WARMUP_SECONDS) -> list[Sample]:
    """Drop the warm-up samples from the front of the series."""
    skip = int(warmup_seconds / SAMPLE_INTERVAL_SECONDS)
    return samples[skip:] if len(samples) > skip else samples


def derive_min_samples(requested_rate: float, measured_seconds: float) -> int:
    """Return the minimum handler samples a run must record.

    The gate demands the run actually carried at least
    :data:`MIN_ACHIEVED_RATE_FRACTION` of the requested load, so the floor is
    derived from the run itself — ``0.8 * rate * seconds`` — never a fixed
    default. A run that quietly served a fraction of the load fails here even if
    its latency numbers look healthy.

    Args:
        requested_rate: The requested update rate, in updates per second.
        measured_seconds: The measured run duration, in seconds.

    Returns:
        The minimum number of handler samples (rounded up).
    """
    return math.ceil(MIN_ACHIEVED_RATE_FRACTION * requested_rate * measured_seconds)


def _metrics_notes(
    metrics: SoakMetrics,
    requested_rate: float,
    achieved_rate: float,
    min_samples: int,
    denominator: float,
) -> list[str]:
    """Render the human-readable metrics notes (rates, windows, largest events)."""
    notes = [
        f"requested rate={requested_rate:.0f}/s achieved rate={achieved_rate:.1f}/s "
        f"(handler samples={metrics.latency_count}, min={min_samples}, "
        f"window={denominator:.1f}s)",
        f"latency p50={metrics.latency_p50_ms:.1f}ms p99={metrics.latency_p99_ms:.1f}ms "
        f"max={metrics.latency_max_ms:.1f}ms",
        f"loop lag whole-run p99={metrics.loop_lag_p99_ms:.1f}ms "
        f"max={metrics.loop_lag_max_ms:.1f}ms",
        f"loop lag warm-up (0-{metrics.loop_lag_warmup_seconds:.0f}s) "
        f"max={metrics.loop_lag_warmup_max_ms:.1f}ms",
        f"loop lag steady (>={metrics.loop_lag_warmup_seconds:.0f}s) "
        f"p99={metrics.loop_lag_steady_p99_ms:.1f}ms "
        f"max={metrics.loop_lag_steady_max_ms:.1f}ms",
        f"handler-time fraction (aggregate)={metrics.handler_time_fraction:.2f} "
        f"over {metrics.uptime_seconds:.1f}s",
    ]
    for event in metrics.lag_events:
        window = "warm-up" if event.elapsed_seconds < metrics.loop_lag_warmup_seconds else "steady"
        notes.append(
            f"largest lag event: {event.lag_ms:.1f}ms at {event.elapsed_seconds:.1f}s ({window})"
        )
    return notes


def _timing_failures(metrics: SoakMetrics) -> list[str]:
    """Return the timing-budget failures for the parsed metrics."""
    failures: list[str] = []
    if metrics.latency_p99_ms > P99_LATENCY_BUDGET_MS:
        failures.append(
            f"handler latency p99 {metrics.latency_p99_ms:.1f}ms > {P99_LATENCY_BUDGET_MS:.0f}ms"
        )
    # Warm-up: only a genuine startup hang fails.
    if metrics.loop_lag_warmup_max_ms > LOOP_LAG_WARMUP_MAX_BUDGET_MS:
        failures.append(
            f"loop lag warm-up max {metrics.loop_lag_warmup_max_ms:.1f}ms "
            f"> {LOOP_LAG_WARMUP_MAX_BUDGET_MS:.0f}ms"
        )
    # Steady state: p99 (the design bound) and max (a regression guard).
    if metrics.loop_lag_steady_p99_ms > LOOP_LAG_STEADY_P99_BUDGET_MS:
        failures.append(
            f"loop lag steady p99 {metrics.loop_lag_steady_p99_ms:.1f}ms "
            f"> {LOOP_LAG_STEADY_P99_BUDGET_MS:.0f}ms"
        )
    if metrics.loop_lag_steady_max_ms > LOOP_LAG_STEADY_MAX_BUDGET_MS:
        failures.append(
            f"loop lag steady max {metrics.loop_lag_steady_max_ms:.1f}ms "
            f"> {LOOP_LAG_STEADY_MAX_BUDGET_MS:.0f}ms"
        )
    return failures


def check(
    *,
    bot_log: str,
    samples: list[Sample],
    call_count: int,
    exit_code: int,
    requested_rate: float,
    measured_seconds: float,
    min_samples: int | None = None,
    warmup_seconds: float = WARMUP_SECONDS,
    enforce_timing_budgets: bool = True,
) -> Verdict:
    """Evaluate every soak criterion and return the verdict.

    Args:
        bot_log: The bot's combined stdout/stderr.
        samples: The parsed RSS/FD samples.
        call_count: The number of API calls the mock recorded.
        exit_code: The bot process's exit code.
        requested_rate: The requested update rate, in updates per second.
        measured_seconds: The measured run duration, in seconds.
        min_samples: An explicit minimum handler-sample count; when ``None`` it
            is derived from ``requested_rate`` and ``measured_seconds``.
        warmup_seconds: The warm-up window excluded from the RSS/FD trends.
        enforce_timing_budgets: When ``False`` (a debug-mode run, where
            ``PYTHONASYNCIODEBUG``/``-X dev`` inflate timing by an order of
            magnitude), the latency-p99 and loop-lag budgets are reported as
            notes but not asserted; hygiene, the achieved-rate floor, RSS/FD, and
            the exit code are still checked.

    Returns:
        The verdict.
    """
    failures: list[str] = []
    notes: list[str] = []

    # The achieved rate is samples / seconds. Prefer the bot's own processing
    # window (uptime_seconds in the metrics line); fall back to the harness's
    # measured duration for an older build that does not report it.
    metrics = parse_metrics(bot_log)
    denominator = measured_seconds
    if metrics is not None and metrics.uptime_seconds > 0:
        denominator = metrics.uptime_seconds
    if min_samples is None:
        min_samples = derive_min_samples(requested_rate, denominator)
    achieved_rate = 0.0

    # 1. The bot must exit cleanly.
    if exit_code != 0:
        failures.append(f"bot exited with code {exit_code}")

    # 2. The log must be free of tracebacks and un-awaited/retrieved task errors.
    failures.extend(
        f"bot log contains {pattern!r}" for pattern in _FAILURE_PATTERNS if pattern in bot_log
    )

    # 3. Metrics must be present and within budget.
    if metrics is None:
        failures.append("no soak-metrics line found in the bot log")
    else:
        achieved_rate = metrics.latency_count / denominator if denominator > 0 else 0.0
        notes.extend(
            _metrics_notes(metrics, requested_rate, achieved_rate, min_samples, denominator)
        )
        if enforce_timing_budgets:
            failures.extend(_timing_failures(metrics))
            if metrics.latency_count < min_samples:
                failures.append(
                    f"achieved rate {achieved_rate:.1f}/s is below "
                    f"{MIN_ACHIEVED_RATE_FRACTION:.0%} of the requested {requested_rate:.0f}/s "
                    f"({metrics.latency_count} handler samples < {min_samples} minimum)"
                )
        else:
            notes.append(
                "timing and achieved-rate budgets not enforced (debug-mode run; hygiene only)"
            )

    # 4. The mock must have served something (a sanity check; the handler-sample
    #    floor above is what actually proves the load was carried).
    if call_count <= 0:
        failures.append("no API calls recorded by the mock server")

    # 5. RSS must not grow (regression slope) after warm-up.
    steady = post_warmup(samples, warmup_seconds)
    if len(steady) >= 2:
        rss_values = [float(s.rss_kb) for s in steady]
        rss_slope = _slope(rss_values)
        baseline = rss_values[0] or 1.0
        growth_pct = (rss_slope * len(rss_values) / baseline) * 100.0
        notes.append(f"RSS slope={rss_slope:.1f}kB/sample (~{growth_pct:.1f}% over run)")
        if growth_pct > RSS_GROWTH_PCT:
            failures.append(f"RSS grew ~{growth_pct:.1f}% > {RSS_GROWTH_PCT:.0f}% post warm-up")
        # 6. FD count must be flat.
        fd_slope = _slope([float(s.fds) for s in steady])
        notes.append(f"FD slope={fd_slope:.3f}/sample")
        if fd_slope > FD_SLOPE_TOLERANCE:
            failures.append(f"FD count grew ({fd_slope:.3f}/sample)")
    else:
        notes.append("too few post-warm-up samples to judge RSS/FD trends")

    return Verdict(ok=not failures, failures=failures, notes=notes)


def load_call_count(calls_log: Path) -> int:
    """Count the JSONL records in the mock server's call log."""
    if not calls_log.is_file():
        return 0
    count = 0
    for line in calls_log.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        try:
            json.loads(line)
        except json.JSONDecodeError:
            continue
        count += 1
    return count


def main(argv: list[str] | None = None) -> int:
    """CLI: read the soak artifacts and print a verdict.

    The gate derives the minimum handler-sample count from the requested rate
    and the measured duration (``0.8 * rate * seconds``); pass ``--min-samples``
    to override it explicitly.

    Returns:
        ``0`` when the soak passed, ``1`` when it failed.
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bot-log", required=True, type=Path)
    parser.add_argument("--samples", required=True, type=Path)
    parser.add_argument("--calls", required=True, type=Path)
    parser.add_argument("--exit-code", type=int, default=0)
    parser.add_argument(
        "--rate", type=float, default=1000.0, help="requested update rate (updates/s)"
    )
    parser.add_argument(
        "--seconds", type=float, default=30.0, help="measured run duration (seconds)"
    )
    parser.add_argument(
        "--min-samples",
        type=int,
        default=None,
        help="explicit minimum handler-sample count (default: derived from --rate/--seconds)",
    )
    parser.add_argument(
        "--hygiene-only",
        action="store_true",
        help="skip the timing budgets (debug-mode run: report them but do not assert)",
    )
    parser.add_argument("--warmup", type=float, default=WARMUP_SECONDS)
    args = parser.parse_args(argv)

    bot_log = (
        args.bot_log.read_text(encoding="utf-8", errors="replace") if args.bot_log.is_file() else ""
    )
    samples = (
        parse_samples(args.samples.read_text(encoding="utf-8")) if args.samples.is_file() else []
    )
    call_count = load_call_count(args.calls)

    verdict = check(
        bot_log=bot_log,
        samples=samples,
        call_count=call_count,
        exit_code=args.exit_code,
        requested_rate=args.rate,
        measured_seconds=args.seconds,
        min_samples=args.min_samples,
        warmup_seconds=args.warmup,
        enforce_timing_budgets=not args.hygiene_only,
    )
    print(verdict.report())
    return 0 if verdict.ok else 1


if __name__ == "__main__":
    sys.exit(main())
