#!/usr/bin/env bash
# gen_man.sh - build the man pages from the scdoc sources (D6).
#
# Usage: gen_man.sh <scdoc-binary> <output-dir>
set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <scdoc-binary> <output-dir>" >&2
    exit 2
fi

scdoc="$1"
outdir="$2"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

mkdir -p "${outdir}"
"${scdoc}" < "${root}/docs/man/tgbot.1.scd" > "${outdir}/tgbot.1"
"${scdoc}" < "${root}/docs/man/tgbot.toml.5.scd" > "${outdir}/tgbot.toml.5"
echo "gen_man: wrote ${outdir}/tgbot.1 and ${outdir}/tgbot.toml.5"
