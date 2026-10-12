"""Tests for the per-module coverage-floor checker (P7)."""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))

import check_coverage


def _report(**files: float) -> dict[str, Any]:
    """Build a minimal coverage.json-shaped report from ``name=percent`` pairs."""
    return {
        "files": {
            name: {"summary": {"percent_covered": percent}} for name, percent in files.items()
        }
    }


def test_check_passes_when_all_meet_floor() -> None:
    report = _report(
        **{
            "nacho_moderator/transport/buckets.py": 99.0,
            "nacho_moderator/config.py": 95.0,
        }
    )
    ok, failures = check_coverage.check(report, {"transport/buckets.py": 95.0, "config.py": 95.0})
    assert ok
    assert failures == []


def test_check_fails_below_floor() -> None:
    report = _report(**{"nacho_moderator/config.py": 80.0})
    ok, failures = check_coverage.check(report, {"config.py": 95.0})
    assert not ok
    assert any("config.py" in f for f in failures)


def test_check_fails_when_module_missing() -> None:
    ok, failures = check_coverage.check(_report(), {"config.py": 95.0})
    assert not ok
    assert any("not found" in f for f in failures)


def test_check_exact_floor_passes() -> None:
    report = _report(**{"nacho_moderator/main.py": 85.0})
    ok, _ = check_coverage.check(report, {"main.py": 85.0})
    assert ok


def test_main_missing_file_returns_1(tmp_path: Path) -> None:
    assert check_coverage.main([str(tmp_path / "absent.json")]) == 1


def test_main_reports_failure(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    report = tmp_path / "coverage.json"
    report.write_text(json.dumps(_report(**{"nacho_moderator/config.py": 10.0})), encoding="utf-8")
    rc = check_coverage.main([str(report)])
    assert rc == 1
    assert "NOT met" in capsys.readouterr().err


def test_main_reports_success(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    report = tmp_path / "coverage.json"
    report.write_text(
        json.dumps(_report(**{f"nacho_moderator/{s}": 100.0 for s in check_coverage.FLOORS})),
        encoding="utf-8",
    )
    rc = check_coverage.main([str(report)])
    assert rc == 0
    assert "met" in capsys.readouterr().out


def test_floors_cover_the_required_modules() -> None:
    """The P7 floors include every module the task names."""
    for required in (
        "transport/buckets.py",
        "transport/ratelimit.py",
        "services/perms.py",
        "services/automod.py",
        "commands/args.py",
        "config.py",
        "logging_setup.py",
        "handlers.py",
        "main.py",
    ):
        assert required in check_coverage.FLOORS, required
