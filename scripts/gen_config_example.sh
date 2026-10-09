#!/usr/bin/env bash
# gen_config_example.sh - regenerate the committed sample config (S3, C5).
#
# Usage: gen_config_example.sh <tgbot-binary> <output-path>
# The sample is produced by `tgbot --dump-config-example` so it can never drift
# from src/config.def; check_consistency.py C5 verifies it is up to date.
set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <tgbot-binary> <output-path>" >&2
    exit 2
fi

binary="$1"
output="$2"

if [ ! -x "${binary}" ]; then
    echo "gen_config_example: '${binary}' is not executable" >&2
    exit 2
fi

tmp="$(mktemp)"
trap 'rm -f "${tmp}"' EXIT
"${binary}" --dump-config-example > "${tmp}"
mv "${tmp}" "${output}"
echo "gen_config_example: wrote ${output}"
