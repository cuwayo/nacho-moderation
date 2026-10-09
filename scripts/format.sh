#!/usr/bin/env bash
# format.sh - clang-format first-party C sources (SPEC-ADDENDUM-TOOLCHAIN A1).
#
# Usage:
#   scripts/format.sh --check   # exit non-zero if any file would change (CI)
#   scripts/format.sh --fix     # rewrite files in place
set -euo pipefail

mode="${1:---check}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

mapfile -t files < <(find "${root}/src" "${root}/include" "${root}/tests" \
    -type f \( -name '*.c' -o -name '*.h' \) | sort)

if [ "${#files[@]}" -eq 0 ]; then
    echo "format: no C sources found"
    exit 0
fi

case "${mode}" in
    --check)
        if ! clang-format --dry-run --Werror "${files[@]}"; then
            echo "format: files need formatting (run 'scripts/format.sh --fix')" >&2
            exit 1
        fi
        echo "format: OK (${#files[@]} files)"
        ;;
    --fix)
        clang-format -i "${files[@]}"
        echo "format: rewrote ${#files[@]} files"
        ;;
    *)
        echo "usage: $0 [--check|--fix]" >&2
        exit 2
        ;;
esac
