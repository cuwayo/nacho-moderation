#!/usr/bin/env bash
# coverage.sh - generate an HTML + Cobertura coverage report (SPEC-ADDENDUM A5.6).
#
# Usage: scripts/coverage.sh <build-dir>
# Requires a build configured with the `coverage` preset (NM_SANITIZE=coverage).
# Prefers gcovr; falls back to lcov/genhtml if gcovr is unavailable.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-${root}/build/coverage}"
out_dir="${build_dir}/coverage"
mkdir -p "${out_dir}"

if [ ! -d "${build_dir}" ]; then
    echo "coverage: build dir '${build_dir}' not found (run: cmake --preset coverage)" >&2
    exit 2
fi

# Only instrument our own code, never third_party/ or system headers. The
# executable entry point (src/main.c) is thin glue and is excluded.
if command -v gcovr >/dev/null 2>&1; then
    echo "coverage: using gcovr"
    gcovr --root "${root}" \
        --filter "${root}/src/" \
        --filter "${root}/include/" \
        --exclude "${root}/third_party/" \
        --exclude "${root}/src/main.c" \
        --print-summary \
        --html-details "${out_dir}/index.html" \
        --cobertura "${out_dir}/coverage.xml" \
        "${build_dir}"
elif command -v lcov >/dev/null 2>&1; then
    echo "coverage: using lcov + genhtml"
    info="${out_dir}/lcov.info"
    lcov --capture --directory "${build_dir}" --output-file "${info}" \
        --rc branch_coverage=1 >/dev/null
    lcov --remove "${info}" "${root}/third_party/*" "${root}/src/main.c" "/usr/*" \
        --output-file "${info}" >/dev/null
    genhtml "${info}" --output-directory "${out_dir}" \
        --rc branch_coverage=1 >/dev/null
    echo "coverage: HTML report at ${out_dir}/index.html"
else
    echo "coverage: neither gcovr nor lcov found; install one of them" >&2
    exit 2
fi

echo "coverage: report written under ${out_dir}"
