#!/usr/bin/env bash
# tidy.sh - run clang-tidy over first-party sources (SPEC-ADDENDUM-TOOLCHAIN A1).
#
# Two modes:
#   scripts/tidy.sh <build-dir>   # tidy every first-party source, using that
#                                 # build dir's compile_commands.json
#   scripts/tidy.sh <file> [...]  # tidy only the listed files (used by pre-commit)
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

build_dir=""
if [ "$#" -ge 1 ] && [ -d "${1}" ] && [ -f "${1}/compile_commands.json" ]; then
    build_dir="${1}"
    shift
fi

if [ "$#" -eq 0 ]; then
    mapfile -t files < <(find "${root}/src" "${root}/include" "${root}/tests" \
        -type f \( -name '*.c' -o -name '*.h' \) | sort)
else
    files=("$@")
fi

if [ "${#files[@]}" -eq 0 ]; then
    echo "tidy: no files to check"
    exit 0
fi

if [ -z "${build_dir}" ]; then
    build_dir="${root}/build/debug"
    if [ ! -f "${build_dir}/compile_commands.json" ]; then
        echo "tidy: no compile_commands.json (configure a preset first, e.g. cmake --preset debug)" >&2
        exit 2
    fi
fi

clang-tidy -p "${build_dir}" --quiet "${files[@]}"
echo "tidy: OK (${#files[@]} files)"
