#!/usr/bin/env python3
"""check_consistency.py - verify docs and generated artifacts against the tables.

SPEC-ADDENDUM-SSOT S7. Runs against a built `tgbot` binary and the repository
tree; implements checks C1-C5 now (C6-C18 are added as their features land, S10).

Usage:
    scripts/check_consistency.py --bin build/debug/tgbot
    scripts/check_consistency.py --bin build/debug/tgbot --only C3,C4
    scripts/check_consistency.py --list
    scripts/check_consistency.py --bin ... --format github

Exit codes: 0 all selected checks pass; 1 at least one check failed;
2 a tool error (missing binary, invalid JSON) so CI can tell the two apart.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

# --- Check registry --------------------------------------------------------


class Check:
    def __init__(self, cid: str, description: str):
        self.cid = cid
        self.description = description


CHECKS = [
    Check("C1", "docgen.py --check: generated blocks up to date"),
    Check("C2", "commands.md headings match the command table"),
    Check("C3", "each non-hidden command has a docs/commands/<name>.md snippet"),
    Check("C4", "configuration.md documents every config key"),
    Check("C5", "config/config.example.toml matches --dump-config-example"),
]


class ToolError(Exception):
    """A condition that prevents a check from running (exit code 2)."""


class Result:
    def __init__(self, cid: str, passed: bool, message: str, where: str = ""):
        self.cid = cid
        self.passed = passed
        self.message = message
        self.where = where


def run_json(binary: Path, flag: str) -> object:
    try:
        out = subprocess.run(
            [str(binary), flag], check=True, capture_output=True, text=True
        ).stdout
    except (OSError, subprocess.CalledProcessError) as exc:
        raise ToolError(f"failed to run {binary} {flag}: {exc}") from exc
    try:
        return json.loads(out)
    except json.JSONDecodeError as exc:
        raise ToolError(f"invalid JSON from {binary} {flag}: {exc}") from exc


def run_text(binary: Path, *flags: str) -> str:
    try:
        return subprocess.run(
            [str(binary), *flags], check=True, capture_output=True, text=True
        ).stdout
    except (OSError, subprocess.CalledProcessError) as exc:
        raise ToolError(f"failed to run {binary} {' '.join(flags)}: {exc}") from exc


# --- Individual checks -----------------------------------------------------


def check_c1(root: Path, binary: Path) -> Result:
    proc = subprocess.run(
        [sys.executable, str(root / "scripts" / "docgen.py"), "--check", "--bin", str(binary),
         "--root", str(root)],
        capture_output=True,
        text=True,
    )
    if proc.returncode == 0:
        return Result("C1", True, "generated blocks are up to date")
    if proc.returncode == 1:
        detail = proc.stderr.strip().splitlines()
        where = detail[0] if detail else "docs"
        return Result("C1", False, "generated blocks are stale (run docgen.py --update)", where)
    raise ToolError(f"docgen.py failed: {proc.stderr.strip()}")


HEADING_RE = re.compile(r"^### /([a-z0-9_]+)\s*$", re.MULTILINE)


def check_c2(root: Path, tables: dict) -> Result:
    doc = root / "docs" / "commands.md"
    if not doc.is_file():
        return Result("C2", False, "docs/commands.md is missing", str(doc))
    headings = set(HEADING_RE.findall(doc.read_text(encoding="utf-8")))
    known = {c["id"] for c in tables["commands"]}

    missing = sorted(known - headings)
    extra = sorted(headings - known)
    if missing or extra:
        parts = []
        if missing:
            parts.append("missing headings for: " + ", ".join(missing))
        if extra:
            parts.append("headings for unknown commands: " + ", ".join(extra))
        return Result("C2", False, "; ".join(parts), str(doc))
    return Result("C2", True, f"{len(known)} command headings present")


SECTION_RE = re.compile(r"^##\s+(.+?)\s*$", re.MULTILINE)


def check_c3(root: Path, tables: dict) -> Result:
    failures = []
    for c in tables["commands"]:
        if c["hidden"]:
            continue
        snippet = root / "docs" / "commands" / f"{c['id']}.md"
        if not snippet.is_file():
            failures.append(f"{c['id']}: missing snippet file")
            continue
        text = snippet.read_text(encoding="utf-8")
        sections = {s.strip().lower() for s in SECTION_RE.findall(text)}
        for required in ("examples", "edge cases"):
            if required not in sections:
                failures.append(f"{c['id']}: no '{required.title()}' section")
    if failures:
        return Result("C3", False, "; ".join(failures))
    return Result("C3", True, "all non-hidden commands have snippets")


ROW_RE = re.compile(r"^\|\s*`([^`]+)`\s*\|(.*)\|\s*$", re.MULTILINE)


def check_c4(root: Path, tables: dict) -> Result:
    doc = root / "docs" / "configuration.md"
    if not doc.is_file():
        return Result("C4", False, "docs/configuration.md is missing", str(doc))
    text = doc.read_text(encoding="utf-8")

    # Table rows of the form: | `key` | TYPE | default | restart | desc |
    documented: dict[str, list[str]] = {}
    for match in ROW_RE.finditer(text):
        cells = [c.strip() for c in match.group(2).split("|")]
        documented[match.group(1)] = cells

    failures = []
    for k in tables["config"]:
        key = k["key"]
        if key not in documented:
            failures.append(f"{key}: no row in configuration.md")
            continue
        cells = documented[key]
        if k["type"] not in cells:
            failures.append(f"{key}: type {k['type']} not shown")
        restart = "yes" if k["restart"] else "no"
        if restart not in cells:
            failures.append(f"{key}: restart flag '{restart}' not shown")
        if k["env"] not in text:
            failures.append(f"{key}: env var {k['env']} not documented")

    known = {k["key"] for k in tables["config"]}
    for key in documented:
        if key not in known:
            failures.append(f"{key}: documented but not in config table")

    if failures:
        return Result("C4", False, "; ".join(failures), str(doc))
    return Result("C4", True, f"{len(known)} config keys documented")


def check_c5(root: Path, binary: Path, tables: dict) -> Result:
    sample = root / "config" / "config.example.toml"
    if not sample.is_file():
        return Result("C5", False, "config/config.example.toml is missing", str(sample))

    generated = run_text(binary, "--dump-config-example")
    on_disk = sample.read_text(encoding="utf-8")
    if on_disk != generated:
        return Result("C5", False, "sample differs from --dump-config-example (regenerate)",
                      str(sample))

    # It must parse with --check-config.
    proc = subprocess.run(
        [str(binary), "--check-config", str(sample)], capture_output=True, text=True
    )
    if proc.returncode != 0:
        return Result("C5", False, f"sample does not validate: {proc.stderr.strip()}", str(sample))

    # Secret values must be obvious placeholders.
    for k in tables["config"]:
        if not k["secret"]:
            continue
        leaf = k["key"].split(".")[-1]
        if not re.search(rf'^\s*{re.escape(leaf)}\s*=\s*"[^"]*REPLACE_ME"', on_disk, re.MULTILINE):
            return Result("C5", False, f"secret {k['key']} lacks a placeholder value", str(sample))

    return Result("C5", True, "sample matches the generator and validates")


# --- Runner ----------------------------------------------------------------


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", default="build/debug/tgbot", help="path to the tgbot binary")
    parser.add_argument("--root", default=".", help="repository root")
    parser.add_argument("--only", default="", help="comma-separated check ids, e.g. C3,C4")
    parser.add_argument("--list", action="store_true", help="list checks and exit")
    parser.add_argument(
        "--format", choices=("human", "github", "json"), default="human", help="output format"
    )
    args = parser.parse_args()

    if args.list:
        for c in CHECKS:
            print(f"{c.cid}: {c.description}")
        return 0

    root = Path(args.root).resolve()
    binary = Path(args.bin).resolve()

    selected = {c.cid for c in CHECKS}
    if args.only:
        selected = {x.strip() for x in args.only.split(",") if x.strip()}
        unknown = selected - {c.cid for c in CHECKS}
        if unknown:
            print(f"check_consistency: unknown check(s): {', '.join(sorted(unknown))}",
                  file=sys.stderr)
            return 2

    try:
        tables = run_json(binary, "--dump-tables-json")
        if not isinstance(tables, dict) or "commands" not in tables or "config" not in tables:
            raise ToolError("--dump-tables-json did not return the expected object")

        results: list[Result] = []
        for cid in [c.cid for c in CHECKS]:
            if cid not in selected:
                continue
            if cid == "C1":
                results.append(check_c1(root, binary))
            elif cid == "C2":
                results.append(check_c2(root, tables))
            elif cid == "C3":
                results.append(check_c3(root, tables))
            elif cid == "C4":
                results.append(check_c4(root, tables))
            elif cid == "C5":
                results.append(check_c5(root, binary, tables))
    except ToolError as exc:
        print(f"check_consistency: {exc}", file=sys.stderr)
        return 2

    failed = [r for r in results if not r.passed]
    if args.format == "json":
        print(json.dumps([r.__dict__ for r in results], indent=2))
    else:
        for r in results:
            status = "PASS" if r.passed else "FAIL"
            line = f"{status} {r.cid}: {r.message}"
            if args.format == "github" and not r.passed:
                where = f" file={r.where}" if r.where else ""
                print(f"::error{where}::{r.cid}: {r.message}")
            else:
                print(line)

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
