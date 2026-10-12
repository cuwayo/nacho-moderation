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

_GOOD_METRICS = (
    "2026-01-01 00:00:00 INFO nacho_moderator.metrics "
    "soak-metrics latency_count=1000 latency_p50_ms=1.000 latency_p99_ms=5.000 "
    "latency_max_ms=20.000 loop_lag_p99_ms=2.000 loop_lag_max_ms=10.000"
)

# 40 samples: 12 warm-up (60 s @ 5 s) + 28 steady, flat RSS/FD.
_FLAT_SAMPLES = "\n".join(["100000 20"] * 40)


def _check(
    *,
    bot_log: str = _GOOD_METRICS,
    samples_text: str = _FLAT_SAMPLES,
    call_count: int = 1000,
    exit_code: int = 0,
) -> soak_check.Verdict:
    return soak_check.check(
        bot_log=bot_log,
        samples=soak_check.parse_samples(samples_text),
        call_count=call_count,
        exit_code=exit_code,
        min_calls=1,
    )


def test_good_run_passes() -> None:
    verdict = _check()
    assert verdict.ok, verdict.report()


def test_parse_metrics_roundtrip() -> None:
    metrics = soak_check.parse_metrics(_GOOD_METRICS)
    assert metrics is not None
    assert metrics.latency_count == 1000
    assert metrics.latency_p99_ms == 5.0
    assert metrics.loop_lag_max_ms == 10.0


def test_parse_metrics_missing() -> None:
    assert soak_check.parse_metrics("nothing here") is None


def test_parse_metrics_takes_last() -> None:
    log = _GOOD_METRICS + "\n" + _GOOD_METRICS.replace("latency_count=1000", "latency_count=7")
    metrics = soak_check.parse_metrics(log)
    assert metrics is not None
    assert metrics.latency_count == 7


def test_high_p99_latency_fails() -> None:
    log = _GOOD_METRICS.replace("latency_p99_ms=5.000", "latency_p99_ms=80.000")
    verdict = _check(bot_log=log)
    assert not verdict.ok
    assert any("latency p99" in f for f in verdict.failures)


def test_high_loop_lag_fails() -> None:
    log = _GOOD_METRICS.replace("loop_lag_max_ms=10.000", "loop_lag_max_ms=250.000")
    verdict = _check(bot_log=log)
    assert not verdict.ok
    assert any("loop lag max" in f for f in verdict.failures)


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


def test_too_few_calls_fails() -> None:
    verdict = _check(call_count=0)
    assert not verdict.ok
    assert any("API calls recorded" in f for f in verdict.failures)


def test_too_few_latency_samples_fails() -> None:
    log = _GOOD_METRICS.replace("latency_count=1000", "latency_count=0")
    verdict = _check(bot_log=log)
    assert not verdict.ok
    assert any("handler samples" in f for f in verdict.failures)


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


def test_main_returns_nonzero_on_failure(tmp_path: Path) -> None:
    bot_log = tmp_path / "bot.log"
    bot_log.write_text("no metrics\n", encoding="utf-8")
    samples = tmp_path / "samples.txt"
    samples.write_text("100 5\n", encoding="utf-8")
    calls = tmp_path / "calls.jsonl"
    calls.write_text('{"method":"x"}\n', encoding="utf-8")
    rc = soak_check.main(
        [
            "--bot-log",
            str(bot_log),
            "--samples",
            str(samples),
            "--calls",
            str(calls),
        ]
    )
    assert rc == 1


def test_main_returns_zero_on_success(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    bot_log = tmp_path / "bot.log"
    bot_log.write_text(_GOOD_METRICS, encoding="utf-8")
    samples = tmp_path / "samples.txt"
    samples.write_text(_FLAT_SAMPLES, encoding="utf-8")
    calls = tmp_path / "calls.jsonl"
    calls.write_text('{"method":"x"}\n', encoding="utf-8")
    rc = soak_check.main(
        [
            "--bot-log",
            str(bot_log),
            "--samples",
            str(samples),
            "--calls",
            str(calls),
        ]
    )
    assert rc == 0
    assert "PASS" in capsys.readouterr().out
