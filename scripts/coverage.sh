#!/usr/bin/env bash
# coverage.sh - run the test suite with coverage and print a summary.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

uv run pytest --cov --cov-report=term-missing "$@"
