#!/usr/bin/env python3
"""docgen.py - rewrite generated doc blocks from the binary's SSOT tables.

SPEC-ADDENDUM-SSOT S6. Hand-written docs contain marker pairs:

    <!-- BEGIN GENERATED: <name> -->
    ...content rewritten by this tool...
    <!-- END GENERATED: <name> -->

Only the content between the markers is touched. All table data comes from the
built binary's `--dump-tables-json` (S1: scripts never regex-parse C).

Usage:
    scripts/docgen.py --update [--bin build/debug/tgbot] [--root .]
    scripts/docgen.py --check  [--bin build/debug/tgbot] [--root .]

--check exits 0 when every block is up to date, 1 when any differs, and 2 on a
tool error (missing binary, invalid JSON) so CI can tell "stale" from "broken".
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

BEGIN_RE = re.compile(r"^<!-- BEGIN GENERATED: ([a-z0-9-]+) -->$")
END_RE = re.compile(r"^<!-- END GENERATED: ([a-z0-9-]+) -->$")


class ToolError(Exception):
    """Raised for conditions that mean the tool itself cannot run."""


def load_tables(binary: Path) -> dict:
    if not binary.is_file():
        raise ToolError(f"binary not found: {binary} (build it first)")
    try:
        out = subprocess.run(
            [str(binary), "--dump-tables-json"],
            check=True,
            capture_output=True,
            text=True,
        ).stdout
    except (OSError, subprocess.CalledProcessError) as exc:
        raise ToolError(f"failed to run {binary} --dump-tables-json: {exc}") from exc
    try:
        return json.loads(out)
    except json.JSONDecodeError as exc:
        raise ToolError(f"invalid JSON from {binary}: {exc}") from exc


# --- Block renderers -------------------------------------------------------


def render_commands_table(tables: dict) -> str:
    lines = ["| Command | Role | Usage | Description |", "|---|---|---|---|"]
    for c in tables["commands"]:
        if c["hidden"]:
            continue
        usage = c["usage"] or ""
        lines.append(f"| `/{c['id']}` | {c['min_role']} | `{usage}` | {c['brief']} |")
    return "\n".join(lines)


def render_commands_reference(tables: dict, root: Path) -> str:
    parts = []
    for c in tables["commands"]:
        flags = ", ".join(c["flags"]) if c["flags"] else "-"
        parts.append(f"### /{c['id']}")
        parts.append("")
        parts.append(f"{c['brief']}.")
        parts.append("")
        parts.append(f"- **Usage:** `/{c['id']} {c['usage']}`".rstrip())
        parts.append(f"- **Minimum role:** {c['min_role']}")
        parts.append(f"- **Caller right:** {c['user_right']}")
        parts.append(f"- **Bot right:** {c['bot_right']}")
        parts.append(f"- **Flags:** {flags}")
        parts.append("")
        snippet = root / "docs" / "commands" / f"{c['id']}.md"
        if snippet.is_file():
            parts.append(snippet.read_text(encoding="utf-8").rstrip())
        else:
            parts.append(f"<!-- missing snippet: docs/commands/{c['id']}.md (see check C3) -->")
        parts.append("")
    return "\n".join(parts).rstrip()


def render_config_table(tables: dict) -> str:
    lines = ["| Key | Type | Default | Restart | Description |", "|---|---|---|---|---|"]
    for k in tables["config"]:
        default = k["default"] if k["default"] != "" else "*(required)*"
        if k["secret"]:
            default = "*(secret)*"
        restart = "yes" if k["restart"] else "no"
        lines.append(
            f"| `{k['key']}` | {k['type']} | {default} | {restart} | {k['brief']} |"
        )
    return "\n".join(lines)


def render_config_reference(tables: dict) -> str:
    parts = []
    for k in tables["config"]:
        parts.append(f"### `{k['key']}`")
        parts.append("")
        parts.append(k["brief"] + ".")
        parts.append("")
        parts.append(f"- **Type:** {k['type']}")
        parts.append(f"- **Environment variable:** `{k['env']}`")
        default = k["default"] if k["default"] != "" else "*(required, no default)*"
        if k["secret"]:
            default = "*(secret)*"
        parts.append(f"- **Default:** {default}")
        parts.append(f"- **Restart required:** {'yes' if k['restart'] else 'no'}")
        if k["enum_values"]:
            parts.append(f"- **Allowed values:** {', '.join(k['enum_values'])}")
        parts.append("")
    return "\n".join(parts).rstrip()


def render_lock_types(tables: dict) -> str:
    lines = ["| Lock | Description |", "|---|---|"]
    for lock in tables["locks"]:
        lines.append(f"| `{lock['name']}` | {lock['brief']} |")
    return "\n".join(lines)


def render_error_classes(tables: dict) -> str:
    lines = ["| Class | Code | Log level | Retry | Description |", "|---|---|---|---|---|"]
    for e in tables["errors"]:
        retry = "yes" if e["retry"] else "no"
        lines.append(f"| {e['id']} | {e['code']} | {e['log_level']} | {retry} | {e['brief']} |")
    return "\n".join(lines)


def render_metrics_table(tables: dict) -> str:
    lines = ["| Metric | Description |", "|---|---|"]
    for m in tables["metrics"]:
        lines.append(f"| `{m['name']}` | {m['brief']} |")
    return "\n".join(lines)


BLOCKS = {
    "commands-table": lambda t, r: render_commands_table(t),
    "commands-reference": lambda t, r: render_commands_reference(t, r),
    "config-table": lambda t, r: render_config_table(t),
    "config-reference": lambda t, r: render_config_reference(t),
    "lock-types": lambda t, r: render_lock_types(t),
    "error-classes": lambda t, r: render_error_classes(t),
    "metrics-table": lambda t, r: render_metrics_table(t),
}

# Docs that may contain generated blocks (SPEC-ADDENDUM-SSOT S6). This is an
# explicit allow-list, never a broad glob: the binding spec files contain
# illustrative `BEGIN GENERATED` examples that must not be rewritten.
TARGETS = [
    "README.md",
    "docs/commands.md",
    "docs/configuration.md",
    "docs/architecture.md",
    "docs/man/tgbot.1.scd",
    "docs/man/tgbot.toml.5.scd",
]


def rewrite_file(path: Path, tables: dict, root: Path) -> tuple[str, bool]:
    """Return (new_text, changed) for one file."""
    text = path.read_text(encoding="utf-8")
    lines = text.split("\n")
    out: list[str] = []
    i = 0
    changed = False
    while i < len(lines):
        m = BEGIN_RE.match(lines[i])
        if not m:
            out.append(lines[i])
            i += 1
            continue
        name = m.group(1)
        out.append(lines[i])
        i += 1
        # Skip until the matching END marker.
        while i < len(lines) and not END_RE.match(lines[i]):
            i += 1
        if i >= len(lines):
            raise ToolError(f"{path}: BEGIN GENERATED '{name}' without END")
        if name not in BLOCKS:
            raise ToolError(f"{path}: unknown generated block '{name}'")
        rendered = BLOCKS[name](tables, root)
        out.extend(rendered.split("\n"))
        out.append(lines[i])  # END marker
        i += 1
        changed = True
    new_text = "\n".join(out)
    return new_text, changed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--update", action="store_true", help="rewrite blocks in place")
    mode.add_argument("--check", action="store_true", help="exit non-zero if stale")
    parser.add_argument("--bin", default="build/debug/tgbot", help="path to the tgbot binary")
    parser.add_argument("--root", default=".", help="repository root")
    parser.add_argument(
        "--file",
        action="append",
        default=[],
        help="process only this file (relative to --root); repeatable; for tests",
    )
    args = parser.parse_args()

    root = Path(args.root).resolve()
    try:
        tables = load_tables(Path(args.bin).resolve())
    except ToolError as exc:
        print(f"docgen: {exc}", file=sys.stderr)
        return 2

    names = args.file if args.file else TARGETS
    targets = [root / name for name in names]

    stale: list[str] = []
    for path in targets:
        if not path.is_file():
            continue
        try:
            new_text, changed = rewrite_file(path, tables, root)
        except ToolError as exc:
            print(f"docgen: {exc}", file=sys.stderr)
            return 2
        if not changed:
            continue
        current = path.read_text(encoding="utf-8")
        if new_text == current:
            continue
        if args.update:
            path.write_text(new_text, encoding="utf-8")
            print(f"docgen: updated {path.relative_to(root)}")
        else:
            stale.append(str(path.relative_to(root)))

    if args.check and stale:
        for rel in stale:
            print(f"docgen: stale block in {rel}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
