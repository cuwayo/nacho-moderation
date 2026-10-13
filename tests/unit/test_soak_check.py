"""Tests for the soak pass/fail logic in scripts/soak_check.py (P5).

Every input is a fixture string, so these run in milliseconds and cover each
failure mode the soak must catch.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))

import soak_check

# A healthy run: 1000 updates/s for 30 s is 30 000 updates, comfortably above the
# derived 0.8 * 1000 * 30 = 24 000 minimum. Lag is inside every window budget.
_GOOD_METRICS = (
    "2026-01-01 00:00:00 INFO nacho_moderator.metrics "
    "soak-metrics latency_count=30000 latency_p50_ms=1.000 latency_p99_ms=5.000 "
    "latency_max_ms=20.000 loop_lag_p99_ms=10.000 loop_lag_max_ms=40.000 "
    "loop_lag_warmup_max_ms=40.000 loop_lag_steady_p99_ms=5.000 "
    "loop_lag_steady_max_ms=30.000 loop_lag_warmup_seconds=60.000 "
    "latency_total_seconds=30.000 uptime_seconds=30.000"
)

# 40 samples: 12 warm-up (60 s @ 5 s) + 28 steady, flat RSS/FD.
_FLAT_SAMPLES = "\n".join(["100000 20"] * 40)


def _check(
    *,
    bot_log: str = _GOOD_METRICS,
    samples_text: str = _FLAT_SAMPLES,
    call_count: int = 30000,
    exit_code: int = 0,
    requested_rate: float = 1000.0,
    measured_seconds: float = 30.0,
    min_samples: int | None = None,
    enforce_timing_budgets: bool = True,
) -> soak_check.Verdict:
    return soak_check.check(
        bot_log=bot_log,
        samples=soak_check.parse_samples(samples_text),
        call_count=call_count,
        exit_code=exit_code,
        requested_rate=requested_rate,
        measured_seconds=measured_seconds,
        min_samples=min_samples,
        enforce_timing_budgets=enforce_timing_budgets,
    )


def test_good_run_passes() -> None:
    verdict = _check()
    assert verdict.ok, verdict.report()


def test_parse_metrics_roundtrip() -> None:
    metrics = soak_check.parse_metrics(_GOOD_METRICS)
    assert metrics is not None
    assert metrics.latency_count == 30000
    assert metrics.latency_p99_ms == 5.0
    assert metrics.loop_lag_max_ms == 40.0
    assert metrics.loop_lag_steady_max_ms == 30.0
    assert metrics.loop_lag_warmup_max_ms == 40.0
    assert metrics.latency_total_seconds == 30.0
    assert metrics.uptime_seconds == 30.0
    assert metrics.handler_time_fraction == pytest.approx(1.0)


def test_parse_metrics_missing() -> None:
    assert soak_check.parse_metrics("nothing here") is None


def test_parse_metrics_accepts_inf_percentiles() -> None:
    """The parser accepts ``inf`` percentiles (an older build's open bucket).

    Regression: with asyncio debug on, p99 latency could land in the ``inf``
    bucket, and the old ``[\\d.]+`` regex did not match it, so the gate reported
    "no soak-metrics line found" instead of the real (over-budget) number.
    """
    line = (
        "soak-metrics latency_count=20000 latency_p50_ms=0.500 latency_p99_ms=inf "
        "latency_max_ms=2190.158 loop_lag_p99_ms=1000.000 loop_lag_max_ms=650.207 "
        "loop_lag_warmup_max_ms=400.000 loop_lag_steady_p99_ms=120.000 "
        "loop_lag_steady_max_ms=650.207 loop_lag_warmup_seconds=60.000 "
        "latency_total_seconds=100.000 uptime_seconds=22.746"
    )
    metrics = soak_check.parse_metrics(line)
    assert metrics is not None
    assert metrics.latency_p99_ms == float("inf")
    assert metrics.loop_lag_p99_ms == 1000.0
    assert metrics.loop_lag_steady_max_ms == 650.207
    assert metrics.uptime_seconds == pytest.approx(22.746)


def test_parse_lag_events() -> None:
    log = _GOOD_METRICS + (
        "\nsoak-lag-event elapsed_s=75.500 lag_ms=210.000"
        "\nsoak-lag-event elapsed_s=12.000 lag_ms=900.000"
    )
    events = soak_check.parse_lag_events(log)
    assert len(events) == 2
    assert events[0].elapsed_seconds == pytest.approx(75.5)
    assert events[0].lag_ms == pytest.approx(210.0)


def test_uptime_is_used_as_the_rate_denominator() -> None:
    """The bot's own window is preferred over the harness wall clock."""
    # 30 000 samples over the bot's 30 s window = 1000/s, even though the
    # harness measured a longer (startup+shutdown-inclusive) 35 s.
    verdict = _check(bot_log=_GOOD_METRICS, measured_seconds=35.0)
    assert verdict.ok, verdict.report()
    assert any("achieved rate=1000.0/s" in n for n in verdict.notes)


def test_parse_metrics_takes_last() -> None:
    log = _GOOD_METRICS + "\n" + _GOOD_METRICS.replace("latency_count=30000", "latency_count=7")
    metrics = soak_check.parse_metrics(log)
    assert metrics is not None
    assert metrics.latency_count == 7


def test_high_p99_latency_fails() -> None:
    log = _GOOD_METRICS.replace("latency_p99_ms=5.000", "latency_p99_ms=80.000")
    verdict = _check(bot_log=log)
    assert not verdict.ok
    assert any("latency p99" in f for f in verdict.failures)


def test_steady_state_lag_p99_fails() -> None:
    log = _GOOD_METRICS.replace("loop_lag_steady_p99_ms=5.000", "loop_lag_steady_p99_ms=150.000")
    verdict = _check(bot_log=log)
    assert not verdict.ok
    assert any("loop lag steady p99" in f for f in verdict.failures)


def test_steady_state_lag_max_fails() -> None:
    log = _GOOD_METRICS.replace("loop_lag_steady_max_ms=30.000", "loop_lag_steady_max_ms=400.000")
    verdict = _check(bot_log=log)
    assert not verdict.ok
    assert any("loop lag steady max" in f for f in verdict.failures)


def test_warmup_lag_spike_only_passes() -> None:
    """A spike inside the warm-up window does not fail the steady-state budgets."""
    log = _GOOD_METRICS.replace("loop_lag_warmup_max_ms=40.000", "loop_lag_warmup_max_ms=600.000")
    # Whole-run max rises with it, but the steady window is untouched.
    log = log.replace("loop_lag_max_ms=40.000", "loop_lag_max_ms=600.000")
    verdict = _check(bot_log=log)
    assert verdict.ok, verdict.report()


def test_warmup_lag_hang_fails() -> None:
    """A genuine startup hang (warm-up max over 1000 ms) still fails."""
    log = _GOOD_METRICS.replace("loop_lag_warmup_max_ms=40.000", "loop_lag_warmup_max_ms=2000.000")
    verdict = _check(bot_log=log)
    assert not verdict.ok
    assert any("loop lag warm-up max" in f for f in verdict.failures)


def test_hygiene_only_skips_timing_budgets() -> None:
    """In debug-mode (hygiene-only) runs the timing budgets are notes, not gates."""
    log = _GOOD_METRICS.replace("latency_p99_ms=5.000", "latency_p99_ms=800.000")
    log = log.replace("loop_lag_steady_max_ms=30.000", "loop_lag_steady_max_ms=900.000")
    verdict = _check(bot_log=log, enforce_timing_budgets=False)
    assert verdict.ok, verdict.report()
    assert any("not enforced" in n for n in verdict.notes)


def test_hygiene_only_skips_the_rate_floor() -> None:
    """A debug run reports the achieved rate but does not gate on it."""
    log = _GOOD_METRICS.replace("latency_count=30000", "latency_count=780")
    verdict = _check(bot_log=log, call_count=780, enforce_timing_budgets=False)
    assert verdict.ok, verdict.report()


def test_hygiene_only_still_checks_hygiene() -> None:
    log = _GOOD_METRICS + "\nTraceback (most recent call last):\n  ..."
    verdict = _check(bot_log=log, enforce_timing_budgets=False)
    assert not verdict.ok
    assert any("Traceback" in f for f in verdict.failures)


def test_traceback_fails() -> None:
    log = _GOOD_METRICS + "\nTraceback (most recent call last):\n  ..."
    verdict = _check(bot_log=log)
    assert not verdict.ok
    assert any("Traceback" in f for f in verdict.failures)


def test_task_exception_never_retrieved_fails() -> None:
    log = _GOOD_METRICS + "\nTask exception was never retrieved"
    verdict = _check(bot_log=log)
    assert not verdict.ok


def test_never_awaited_fails() -> None:
    log = _GOOD_METRICS + "\ncoroutine ... was never awaited"
    verdict = _check(bot_log=log)
    assert not verdict.ok


def test_nonzero_exit_fails() -> None:
    verdict = _check(exit_code=1)
    assert not verdict.ok
    assert any("exited with code 1" in f for f in verdict.failures)


def test_missing_metrics_fails() -> None:
    verdict = _check(bot_log="no metrics here")
    assert not verdict.ok
    assert any("no soak-metrics line" in f for f in verdict.failures)


def test_no_calls_fails() -> None:
    """No API calls at all means the mock served nothing — a broken run."""
    verdict = _check(call_count=0)
    assert not verdict.ok
    assert any("no API calls recorded" in f for f in verdict.failures)


def test_too_few_latency_samples_fails() -> None:
    log = _GOOD_METRICS.replace("latency_count=30000", "latency_count=0")
    verdict = _check(bot_log=log)
    assert not verdict.ok
    assert any("achieved rate" in f for f in verdict.failures)


# --- achieved-rate gate -----------------------------------------------------


def test_derive_min_samples() -> None:
    assert soak_check.derive_min_samples(1000, 30) == 24000
    assert soak_check.derive_min_samples(1000, 600) == 480000
    # Rounds up, never down.
    assert soak_check.derive_min_samples(3, 1) == 3  # ceil(2.4)


def test_low_achieved_rate_fails() -> None:
    """A run that served ~3 % of the load (the old SOAK_BATCH=1 symptom) fails."""
    # 780 updates in 30 s is the reported ~26/s, far below 1000/s.
    log = _GOOD_METRICS.replace("latency_count=30000", "latency_count=780")
    verdict = _check(bot_log=log, call_count=780)
    assert not verdict.ok
    assert any("achieved rate" in f for f in verdict.failures)


def test_achieved_rate_at_the_floor_passes() -> None:
    """Exactly 80 % of the requested rate is accepted."""
    log = _GOOD_METRICS.replace("latency_count=30000", "latency_count=24000")
    verdict = _check(bot_log=log, call_count=24000)
    assert verdict.ok, verdict.report()


def test_achieved_rate_just_below_the_floor_fails() -> None:
    log = _GOOD_METRICS.replace("latency_count=30000", "latency_count=23999")
    verdict = _check(bot_log=log, call_count=23999)
    assert not verdict.ok
    assert any("achieved rate" in f for f in verdict.failures)


def test_derived_minimum_used_when_none_given() -> None:
    """With no explicit minimum, the floor comes from rate * seconds."""
    log = _GOOD_METRICS.replace("latency_count=30000", "latency_count=23999")
    verdict = _check(bot_log=log, call_count=23999, min_samples=None)
    assert not verdict.ok
    assert any("24000 minimum" in f for f in verdict.failures)


def test_explicit_minimum_overrides_derivation() -> None:
    """An explicit minimum is used verbatim instead of the derived one."""
    log = _GOOD_METRICS.replace("latency_count=30000", "latency_count=500")
    verdict = _check(bot_log=log, call_count=500, min_samples=100)
    assert verdict.ok, verdict.report()


def test_notes_report_requested_achieved_and_samples() -> None:
    verdict = _check()
    joined = "\n".join(verdict.notes)
    assert "requested rate=1000/s" in joined
    assert "achieved rate=1000.0/s" in joined
    assert "handler samples=30000" in joined


def test_zero_seconds_does_not_divide_by_zero() -> None:
    verdict = _check(measured_seconds=0.0, min_samples=0, call_count=0)
    assert any("achieved rate" in n for n in verdict.notes)


# --- RSS / FD ---------------------------------------------------------------


def test_rss_growth_fails() -> None:
    # Warm-up flat, then a clear upward trend: 100000 -> 120000 over 28 samples.
    warm = ["100000 20"] * 12
    steady = [f"{100000 + i * 700} 20" for i in range(28)]
    verdict = _check(samples_text="\n".join(warm + steady))
    assert not verdict.ok
    assert any("RSS grew" in f for f in verdict.failures)


def test_fd_growth_fails() -> None:
    warm = ["100000 20"] * 12
    steady = [f"100000 {20 + i}" for i in range(28)]
    verdict = _check(samples_text="\n".join(warm + steady))
    assert not verdict.ok
    assert any("FD count grew" in f for f in verdict.failures)


def test_fd_stable_passes() -> None:
    warm = ["100000 20"] * 12
    steady = ["100000 20"] * 28
    verdict = _check(samples_text="\n".join(warm + steady))
    assert verdict.ok, verdict.report()


def test_warmup_ignores_early_rise() -> None:
    """A rise during warm-up (JIT/interning) is not counted as a leak."""
    warm = [f"{50000 + i * 5000} 20" for i in range(12)]  # big rise
    steady = ["110000 20"] * 28  # then flat
    verdict = _check(samples_text="\n".join(warm + steady))
    assert verdict.ok, verdict.report()


def test_parse_samples_skips_comments_and_blanks() -> None:
    text = "# a comment\n\n100 5\n200 6\n"
    samples = soak_check.parse_samples(text)
    assert [(s.rss_kb, s.fds) for s in samples] == [(100, 5), (200, 6)]


def test_post_warmup_drops_prefix() -> None:
    samples = [soak_check.Sample(1, 1)] * 20
    assert len(soak_check.post_warmup(samples, warmup_seconds=60)) == 20 - 12


def test_slope_flat_and_rising() -> None:
    assert soak_check._slope([5.0, 5.0, 5.0]) == 0.0
    assert soak_check._slope([1.0, 2.0, 3.0]) == pytest.approx(1.0)


def test_load_call_count(tmp_path: Path) -> None:
    calls = tmp_path / "calls.jsonl"
    calls.write_text('{"method":"a"}\n\n{"method":"b"}\n', encoding="utf-8")
    assert soak_check.load_call_count(calls) == 2
    assert soak_check.load_call_count(tmp_path / "absent.jsonl") == 0


# --- CLI --------------------------------------------------------------------


def _write_artifacts(
    tmp_path: Path, *, bot_log: str, samples_text: str = _FLAT_SAMPLES, calls: int = 30000
) -> list[str]:
    log = tmp_path / "bot.log"
    log.write_text(bot_log, encoding="utf-8")
    samples = tmp_path / "samples.txt"
    samples.write_text(samples_text, encoding="utf-8")
    calls_log = tmp_path / "calls.jsonl"
    calls_log.write_text('{"method":"x"}\n' * calls, encoding="utf-8")
    return ["--bot-log", str(log), "--samples", str(samples), "--calls", str(calls_log)]


def test_main_returns_nonzero_on_failure(tmp_path: Path) -> None:
    argv = _write_artifacts(tmp_path, bot_log="no metrics\n", calls=1)
    assert soak_check.main(argv) == 1


def test_main_returns_zero_on_success(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    argv = _write_artifacts(tmp_path, bot_log=_GOOD_METRICS)
    rc = soak_check.main([*argv, "--rate", "1000", "--seconds", "30"])
    assert rc == 0
    assert "PASS" in capsys.readouterr().out


def test_main_low_achieved_rate_fails(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    argv = _write_artifacts(
        tmp_path,
        bot_log=_GOOD_METRICS.replace("latency_count=30000", "latency_count=780"),
        calls=780,
    )
    rc = soak_check.main([*argv, "--rate", "1000", "--seconds", "30"])
    assert rc == 1
    assert "achieved rate" in capsys.readouterr().out


def test_main_derives_minimum_from_rate_and_seconds(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """The CLI uses the derived floor: a short run at a high rate still fails."""
    # 3000 samples over 30 s is 100/s at a requested 1000/s: below 80 %.
    argv = _write_artifacts(
        tmp_path,
        bot_log=_GOOD_METRICS.replace("latency_count=30000", "latency_count=3000"),
        calls=3000,
    )
    rc = soak_check.main([*argv, "--rate", "1000", "--seconds", "30"])
    assert rc == 1
    out = capsys.readouterr().out
    assert "24000 minimum" in out


def test_main_hygiene_only_flag_skips_timing(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """--hygiene-only reports the timing numbers but does not assert them."""
    log = _GOOD_METRICS.replace("latency_p99_ms=5.000", "latency_p99_ms=800.000")
    log = log.replace("loop_lag_steady_max_ms=30.000", "loop_lag_steady_max_ms=900.000")
    argv = _write_artifacts(tmp_path, bot_log=log)
    rc = soak_check.main([*argv, "--rate", "1000", "--seconds", "30", "--hygiene-only"])
    assert rc == 0
    out = capsys.readouterr().out
    assert "not enforced" in out


def test_main_enforces_timing_by_default(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    log = _GOOD_METRICS.replace("loop_lag_steady_max_ms=30.000", "loop_lag_steady_max_ms=900.000")
    argv = _write_artifacts(tmp_path, bot_log=log)
    rc = soak_check.main([*argv, "--rate", "1000", "--seconds", "30"])
    assert rc == 1
    assert "loop lag steady max" in capsys.readouterr().out
