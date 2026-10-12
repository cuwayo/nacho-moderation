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
* loop lag (p99 and max) <= 100 ms;
* RSS growth after a 60 s warm-up <= 10 % (linear-regression slope);
* FD count flat (no upward slope);
* no traceback, "Task exception was never retrieved", or "was never awaited";
* bot exited 0.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

# --- Thresholds (single source of truth) -----------------------------------

P99_LATENCY_BUDGET_MS = 50.0
LOOP_LAG_BUDGET_MS = 100.0
RSS_GROWTH_PCT = 10.0
FD_SLOPE_TOLERANCE = 0.5  # fds gained per 5 s sample, allowed drift
WARMUP_SECONDS = 60.0
SAMPLE_INTERVAL_SECONDS = 5.0

_METRICS_RE = re.compile(
    r"soak-metrics "
    r"latency_count=(?P<latency_count>\d+) "
    r"latency_p50_ms=(?P<latency_p50>[\d.]+) "
    r"latency_p99_ms=(?P<latency_p99>[\d.]+) "
    r"latency_max_ms=(?P<latency_max>[\d.]+) "
    r"loop_lag_p99_ms=(?P<loop_lag_p99>[\d.]+) "
    r"loop_lag_max_ms=(?P<loop_lag_max>[\d.]+)"
)

_FAILURE_PATTERNS = (
    "Traceback (most recent call last)",
    "Task exception was never retrieved",
    "was never awaited",
)

_SAMPLE_RE = re.compile(r"^\s*(?P<rss>\d+)\s+(?P<fds>\d+)\s*$")


@dataclass
class SoakMetrics:
    """The parsed in-bot metrics summary."""

    latency_count: int
    latency_p50_ms: float
    latency_p99_ms: float
    latency_max_ms: float
    loop_lag_p99_ms: float
    loop_lag_max_ms: float


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
    )


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


def check(
    *,
    bot_log: str,
    samples: list[Sample],
    call_count: int,
    exit_code: int,
    min_calls: int = 1,
    warmup_seconds: float = WARMUP_SECONDS,
) -> Verdict:
    """Evaluate every soak criterion and return the verdict."""
    failures: list[str] = []
    notes: list[str] = []

    # 1. The bot must exit cleanly.
    if exit_code != 0:
        failures.append(f"bot exited with code {exit_code}")

    # 2. The log must be free of tracebacks and un-awaited/retrieved task errors.
    failures.extend(
        f"bot log contains {pattern!r}" for pattern in _FAILURE_PATTERNS if pattern in bot_log
    )

    # 3. Metrics must be present and within budget.
    metrics = parse_metrics(bot_log)
    if metrics is None:
        failures.append("no soak-metrics line found in the bot log")
    else:
        notes.append(
            f"latency p50={metrics.latency_p50_ms:.1f}ms p99={metrics.latency_p99_ms:.1f}ms"
        )
        notes.append(
            f"loop lag p99={metrics.loop_lag_p99_ms:.1f}ms max={metrics.loop_lag_max_ms:.1f}ms"
        )
        if metrics.latency_p99_ms > P99_LATENCY_BUDGET_MS:
            failures.append(
                f"handler latency p99 {metrics.latency_p99_ms:.1f}ms "
                f"> {P99_LATENCY_BUDGET_MS:.0f}ms"
            )
        if metrics.loop_lag_max_ms > LOOP_LAG_BUDGET_MS:
            failures.append(
                f"loop lag max {metrics.loop_lag_max_ms:.1f}ms > {LOOP_LAG_BUDGET_MS:.0f}ms"
            )
        if metrics.latency_count < min_calls:
            failures.append(f"only {metrics.latency_count} handler samples (< {min_calls})")

    # 4. The run must have done real work.
    if call_count < min_calls:
        failures.append(f"only {call_count} API calls recorded (< {min_calls})")

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

    Returns:
        ``0`` when the soak passed, ``1`` when it failed.
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bot-log", required=True, type=Path)
    parser.add_argument("--samples", required=True, type=Path)
    parser.add_argument("--calls", required=True, type=Path)
    parser.add_argument("--exit-code", type=int, default=0)
    parser.add_argument("--min-calls", type=int, default=1)
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
        min_calls=args.min_calls,
        warmup_seconds=args.warmup,
    )
    print(verdict.report())
    return 0 if verdict.ok else 1


if __name__ == "__main__":
    sys.exit(main())
