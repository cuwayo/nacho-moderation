#!/usr/bin/env python3
"""check_coverage.py - enforce per-module coverage floors from coverage.json.

``pytest --cov --cov-report=json`` writes ``coverage.json``. This script reads it
and fails (exit 1) when a module is below its floor. The floors are the single
source of truth for P7:

* ``transport/*``, ``services/perms.py``, ``services/automod.py``,
  ``commands/args.py``, ``config.py``, ``logging_setup.py`` >= 95 %;
* ``handlers.py`` and ``main.py`` >= 85 %.

The global floor (90 %) is enforced by coverage itself
(``[tool.coverage.report] fail_under``); this script adds the per-module gates.

Usage:
    uv run pytest --cov --cov-report=json
    uv run python scripts/check_coverage.py [coverage.json]
"""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any

# Module suffix (as coverage.py reports it) -> minimum percent.
FLOORS: dict[str, float] = {
    "transport/buckets.py": 95.0,
    "transport/ratelimit.py": 95.0,
    "transport/retry.py": 95.0,
    "services/perms.py": 95.0,
    "services/automod.py": 95.0,
    "commands/args.py": 95.0,
    "config.py": 95.0,
    "logging_setup.py": 95.0,
    "handlers.py": 85.0,
    "main.py": 85.0,
}


def _percent(entry: dict[str, Any]) -> float:
    """Return the coverage percent for one file entry."""
    summary = entry.get("summary", {})
    return float(summary.get("percent_covered", 0.0))


def check(report: dict[str, Any], floors: dict[str, float]) -> tuple[bool, list[str]]:
    """Return ``(ok, failures)`` for the given coverage report and floors."""
    files = report.get("files", {})
    failures: list[str] = []
    checked = 0
    for suffix, floor in floors.items():
        match = next((path for path in files if path.endswith(suffix)), None)
        if match is None:
            failures.append(f"{suffix}: not found in coverage report (floor {floor:.0f}%)")
            continue
        checked += 1
        percent = _percent(files[match])
        if percent + 1e-9 < floor:
            failures.append(f"{suffix}: {percent:.2f}% < {floor:.0f}%")
    return (not failures, failures) if checked or failures else (False, ["no modules checked"])


def main(argv: list[str] | None = None) -> int:
    """Read coverage.json and report any module below its floor."""
    path = Path(argv[0]) if argv else Path("coverage.json")
    if not path.is_file():
        print(
            f"check_coverage: {path} not found; run pytest --cov --cov-report=json",
            file=sys.stderr,
        )
        return 1
    report = json.loads(path.read_text(encoding="utf-8"))
    ok, failures = check(report, FLOORS)
    if ok:
        print("check_coverage: all per-module floors met")
        return 0
    print("check_coverage: per-module coverage floors NOT met:", file=sys.stderr)
    for failure in failures:
        print(f"  {failure}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
