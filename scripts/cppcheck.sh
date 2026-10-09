#!/usr/bin/env bash
# cppcheck.sh - static analysis (SPEC-ADDENDUM-TOOLCHAIN A1).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cppcheck \
    --enable=warning,style,performance,portability \
    --error-exitcode=1 \
    --inline-suppr \
    --std=c11 \
    --language=c \
    --suppress=missingIncludeSystem \
    --quiet \
    -I "${root}/include" \
    "${root}/src" "${root}/tests/unit" "${root}/tests/fuzz"

echo "cppcheck: OK"
