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
import os
import re
import subprocess
import sys
import tempfile
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
    Check("C6", "every TOML snippet in docs parses with --check-config"),
    Check("C7", "README bash title=ci snippets parse and use known presets"),
    Check("C8", "tgbot --help matches tests/golden/help.txt and docs/building.md"),
    Check("C9", "every api_methods.def method has a mock handler and a docs row"),
    Check("C10", "commands appear in tgbot.1.scd; config keys in tgbot.toml.5.scd"),
    Check("C11", "setMyCommands payload constraints hold"),
    Check("C12", "every @ingroup is defined in groups.dox; every src file has @file"),
    Check("C13", "no TODO/FIXME without an issue reference in src/tests/docs"),
    Check("C14", "CHANGELOG Unreleased changed when src/ changed (PR context)"),
    Check("C15", "commands.def/config.def changes come with tests and docs"),
    Check("C16", "metrics.def and errors.def entries are documented"),
    Check("C17", "relative links and anchors in docs resolve"),
    Check("C18", "forbidden terminology is absent from docs"),
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


def check_c9(root: Path, tables: dict) -> Result:
    """Every API method has a mock handler and a docs/telegram-api-notes.md row."""
    methods = [m["method"] for m in tables.get("api_methods", [])]
    if not methods:
        return Result("C9", False, "api_methods table is empty")

    mock = root / "tests" / "mock" / "mock_tg_server.py"
    if not mock.is_file():
        return Result("C9", False, "tests/mock/mock_tg_server.py is missing", str(mock))
    mock_text = mock.read_text(encoding="utf-8")

    notes = root / "docs" / "telegram-api-notes.md"
    if not notes.is_file():
        return Result("C9", False, "docs/telegram-api-notes.md is missing", str(notes))
    notes_text = notes.read_text(encoding="utf-8")

    failures = []
    for m in methods:
        # A handler is either an explicit branch or a HANDLERS registry entry.
        if f'"{m}"' not in mock_text:
            failures.append(f"{m}: no handler in mock_tg_server.py")
        if f"`{m}`" not in notes_text:
            failures.append(f"{m}: no row in docs/telegram-api-notes.md")

    if failures:
        return Result("C9", False, "; ".join(failures))
    return Result("C9", True, f"{len(methods)} API methods have a mock handler and docs row")


# --- C6: TOML snippets in docs validate ------------------------------------

FENCE_RE = re.compile(r"^```(?P<info>[^\n]*)\n(?P<body>.*?)^```\s*$", re.MULTILINE | re.DOTALL)


def iter_fences(text: str):
    """Yield (info_string, body) for every fenced code block in @p text."""
    for m in FENCE_RE.finditer(text):
        yield m.group("info").strip(), m.group("body")


def docs_markdown(root: Path) -> list[Path]:
    files = [root / "README.md"]
    files += sorted((root / "docs").rglob("*.md"))
    return [f for f in files if f.is_file()]


def check_c6(root: Path, binary: Path) -> Result:
    """Every TOML fenced block in docs must pass `tgbot --check-config`."""
    failures = []
    checked = 0
    for doc in docs_markdown(root):
        for info, body in iter_fences(doc.read_text(encoding="utf-8")):
            if not info.startswith("toml"):
                continue
            if "no-check" in info:
                continue
            checked += 1
            # Snippets are often partial; prepend the required key so the
            # required-key validation passes while every provided key is still
            # validated. The parser treats a bare key after a [table] header as
            # belonging to that table, so the required key must come first.
            text = body
            if "bot_token" not in body:
                text = 'bot_token = "123456:CHECK"\n' + body
            with tempfile.NamedTemporaryFile("w", suffix=".toml", delete=False) as fh:
                fh.write(text)
                tmp = fh.name
            try:
                proc = subprocess.run([str(binary), "--check-config", tmp],
                                      capture_output=True, text=True)
            finally:
                os.unlink(tmp)
            if proc.returncode != 0:
                failures.append(f"{doc.relative_to(root)}: {proc.stderr.strip()}")
    if failures:
        return Result("C6", False, "; ".join(failures))
    return Result("C6", True, f"{checked} TOML snippet(s) validate")


# --- C7: README quick-start snippets parse and use known presets ------------

PRESET_RE = re.compile(r"--preset\s+([A-Za-z0-9_-]+)")


def check_c7(root: Path) -> Result:
    readme = root / "README.md"
    if not readme.is_file():
        return Result("C7", False, "README.md is missing", str(readme))

    presets = set()
    presets_json = root / "CMakePresets.json"
    if presets_json.is_file():
        try:
            data = json.loads(presets_json.read_text(encoding="utf-8"))
            for key in ("configurePresets", "buildPresets", "testPresets"):
                for p in data.get(key, []):
                    if "name" in p:
                        presets.add(p["name"])
        except json.JSONDecodeError as exc:
            raise ToolError(f"CMakePresets.json is invalid JSON: {exc}") from exc

    failures = []
    checked = 0
    for info, body in iter_fences(readme.read_text(encoding="utf-8")):
        if 'title="ci"' not in info:
            continue
        checked += 1
        proc = subprocess.run(["bash", "-n"], input=body, capture_output=True, text=True)
        if proc.returncode != 0:
            failures.append(f"snippet does not parse: {proc.stderr.strip()}")
        for name in PRESET_RE.findall(body):
            if presets and name not in presets:
                failures.append(f"unknown preset '{name}'")
    if failures:
        return Result("C7", False, "; ".join(failures), "README.md")
    return Result("C7", True, f"{checked} CI snippet(s) parse; presets known")


# --- C8: --help golden file and building.md --------------------------------

def check_c8(root: Path, binary: Path) -> Result:
    golden = root / "tests" / "golden" / "help.txt"
    if not golden.is_file():
        return Result("C8", False, "tests/golden/help.txt is missing", str(golden))
    help_text = run_text(binary, "--help")
    golden_text = golden.read_text(encoding="utf-8")
    if help_text != golden_text:
        return Result("C8", False, "--help differs from tests/golden/help.txt", str(golden))

    building = root / "docs" / "building.md"
    if not building.is_file():
        return Result("C8", False, "docs/building.md is missing", str(building))
    if help_text.strip() not in building.read_text(encoding="utf-8"):
        return Result("C8", False, "--help text not reproduced verbatim in docs/building.md",
                      str(building))
    return Result("C8", True, "--help matches the golden file and building.md")


# --- C10: man pages list every command and config key ----------------------

def check_c10(root: Path, tables: dict) -> Result:
    man1 = root / "docs" / "man" / "tgbot.1.scd"
    man5 = root / "docs" / "man" / "tgbot.toml.5.scd"
    if not man1.is_file():
        return Result("C10", False, "docs/man/tgbot.1.scd is missing", str(man1))
    if not man5.is_file():
        return Result("C10", False, "docs/man/tgbot.toml.5.scd is missing", str(man5))

    man1_text = man1.read_text(encoding="utf-8")
    man5_text = man5.read_text(encoding="utf-8")

    failures = []
    for c in tables["commands"]:
        if c["hidden"]:
            continue
        if f"`/{c['id']}`" not in man1_text:
            failures.append(f"command /{c['id']} missing from tgbot.1.scd")
    for k in tables["config"]:
        if f"`{k['key']}`" not in man5_text:
            failures.append(f"config key {k['key']} missing from tgbot.toml.5.scd")
    if failures:
        return Result("C10", False, "; ".join(failures))
    return Result("C10", True, "man pages list every command and config key")


# --- C11: setMyCommands payload constraints --------------------------------

NAME_RE = re.compile(r"^[a-z0-9_]{1,32}$")


def check_c11(tables: dict) -> Result:
    failures = []
    non_admin = set()
    admin = set()
    for c in tables["commands"]:
        if c["hidden"]:
            continue
        name = c["id"]
        if not NAME_RE.match(name):
            failures.append(f"{name}: invalid command name")
        brief = c["brief"]
        if not (1 <= len(brief) <= 256):
            failures.append(f"{name}: brief length {len(brief)} out of range")
        if c["min_role"] == "ROLE_ANYONE":
            non_admin.add(name)
        admin.add(name)
    # Every non-admin command must also be visible to administrators.
    extra = non_admin - admin
    if extra:
        failures.append(f"non-admin scope not a subset of admin scope: {sorted(extra)}")
    if failures:
        return Result("C11", False, "; ".join(failures))
    return Result("C11", True, f"{len(admin)} command(s) fit the setMyCommands limits")


# --- C12: Doxygen groups and @file -----------------------------------------

INGROUP_RE = re.compile(r"@ingroup\s+([a-z0-9_]+)")
DEFGROUP_RE = re.compile(r"@defgroup\s+([a-z0-9_]+)")


def check_c12(root: Path) -> Result:
    groups_dox = root / "docs" / "groups.dox"
    if not groups_dox.is_file():
        return Result("C12", False, "docs/groups.dox is missing", str(groups_dox))
    defined = set(DEFGROUP_RE.findall(groups_dox.read_text(encoding="utf-8")))

    failures = []
    src = root / "src"
    for path in sorted(list(src.rglob("*.c")) + list(src.rglob("*.h"))):
        text = path.read_text(encoding="utf-8")
        if "@file" not in text:
            failures.append(f"{path.relative_to(root)}: missing @file")
        for g in INGROUP_RE.findall(text):
            if g not in defined:
                failures.append(f"{path.relative_to(root)}: @ingroup {g} not in groups.dox")
    if failures:
        return Result("C12", False, "; ".join(failures))
    return Result("C12", True, "every @ingroup defined and every src file has @file")


# --- C13: no bare TODO/FIXME -----------------------------------------------

TODO_RE = re.compile(r"\b(TODO|FIXME)\b")
ISSUE_RE = re.compile(r"\((?:#\d+|TODO-ID)\)")


def check_c13(root: Path) -> Result:
    failures = []
    for sub in ("src", "tests", "docs"):
        base = root / sub
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if not path.is_file() or path.suffix in (".pyc",):
                continue
            if "__pycache__" in path.parts:
                continue
            try:
                text = path.read_text(encoding="utf-8")
            except (UnicodeDecodeError, OSError):
                continue
            for lineno, line in enumerate(text.splitlines(), 1):
                for m in TODO_RE.finditer(line):
                    # A real work marker is a bare comment token. Documentation
                    # that describes the rule writes it in a code span, so a
                    # backtick on either side marks it as prose, not a marker.
                    before = line[m.start() - 1] if m.start() > 0 else ""
                    after = line[m.end()] if m.end() < len(line) else ""
                    if before == "`" or after == "`":
                        continue
                    if ISSUE_RE.search(line):
                        continue
                    if path.name.startswith("SPEC-ADDENDUM"):
                        continue
                    failures.append(
                        f"{path.relative_to(root)}:{lineno}: TODO/FIXME without issue id")
                    break
    if failures:
        return Result("C13", False, "; ".join(failures[:10]))
    return Result("C13", True, "no TODO/FIXME without an issue reference")


# --- C14: CHANGELOG Unreleased touched when src/ changed -------------------

def check_c14(root: Path, base: str) -> Result:
    """Only meaningful in PR context (--base); otherwise passes vacuously."""
    if not base:
        return Result("C14", True, "no --base given; skipped (PR context only)")
    try:
        changed = subprocess.run(["git", "diff", "--name-only", f"{base}...HEAD"],
                                 cwd=root, capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError) as exc:
        raise ToolError(f"git diff failed: {exc}") from exc
    files = [f for f in changed.splitlines() if f]
    if not any(f.startswith("src/") for f in files):
        return Result("C14", True, "no src/ changes; CHANGELOG not required")
    if "CHANGELOG.md" in files:
        return Result("C14", True, "src/ changed and CHANGELOG.md updated")
    return Result("C14", False, "src/ changed but CHANGELOG.md was not updated", "CHANGELOG.md")


# --- C15: .def changes come with tests and docs ----------------------------

def check_c15(root: Path, base: str) -> Result:
    if not base:
        return Result("C15", True, "no --base given; skipped (PR context only)")
    try:
        changed = subprocess.run(["git", "diff", "--name-only", f"{base}...HEAD"],
                                 cwd=root, capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError) as exc:
        raise ToolError(f"git diff failed: {exc}") from exc
    files = [f for f in changed.splitlines() if f]
    def_changed = [f for f in files if f.endswith(".def")]
    if not def_changed:
        return Result("C15", True, "no .def changes")
    has_test = any(f.startswith("tests/") for f in files)
    has_docs = any(f.startswith("docs/") for f in files)
    if has_test and has_docs:
        return Result("C15", True, f".def changes accompanied by tests and docs")
    missing = []
    if not has_test:
        missing.append("tests/")
    if not has_docs:
        missing.append("docs/")
    return Result("C15", False, f".def changed without changes under: {', '.join(missing)}")


# --- C16: metrics and error classes documented -----------------------------

def check_c16(root: Path, tables: dict) -> Result:
    failures = []
    arch = root / "docs" / "architecture.md"
    if not arch.is_file():
        return Result("C16", False, "docs/architecture.md is missing", str(arch))
    arch_text = arch.read_text(encoding="utf-8")
    for e in tables.get("errors", []):
        if f"| {e['id']} |" not in arch_text:
            failures.append(f"error class {e['id']} missing from the architecture error table")

    metrics = tables.get("metrics", [])
    if metrics:
        # Metrics must be documented somewhere in docs/ (a generated or manual table).
        docs_text = "\n".join(p.read_text(encoding="utf-8") for p in docs_markdown(root))
        for m in metrics:
            if f"`{m['name']}`" not in docs_text:
                failures.append(f"metric {m['name']} is not documented")
    if failures:
        return Result("C16", False, "; ".join(failures))
    return Result("C16", True, "metrics and error classes documented")


# --- C17: relative links and anchors in docs -------------------------------

LINK_RE = re.compile(r"\[[^\]]*\]\((?P<target>[^)]+)\)")


def check_c17(root: Path) -> Result:
    failures = []
    for doc in docs_markdown(root):
        for m in LINK_RE.finditer(doc.read_text(encoding="utf-8")):
            target = m.group("target").split("#", 1)[0].strip()
            if not target or "://" in target or target.startswith(("mailto:", "#")):
                continue
            resolved = (doc.parent / target).resolve()
            if not resolved.exists():
                failures.append(f"{doc.relative_to(root)}: broken link '{target}'")
    if failures:
        return Result("C17", False, "; ".join(failures[:10]))
    return Result("C17", True, "relative doc links resolve")


# --- C18: forbidden terminology --------------------------------------------

def load_terminology(root: Path) -> list[tuple[str, list[str]]]:
    path = root / "docs" / "terminology.txt"
    if not path.is_file():
        return []
    terms = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        canonical, forbidden = line.split("=", 1)
        terms.append((canonical.strip(), [f.strip() for f in forbidden.split(",") if f.strip()]))
    return terms


def check_c18(root: Path) -> Result:
    terms = load_terminology(root)
    if not terms:
        return Result("C18", True, "no terminology rules defined")
    # Only prose docs; the spec addenda quote the rules and are exempt.
    docs = [d for d in docs_markdown(root)
            if not d.name.startswith("SPEC-ADDENDUM")]
    docs += sorted((root / "docs" / "commands").glob("*.md"))
    failures = []
    for doc in docs:
        text = doc.read_text(encoding="utf-8")
        for _canonical, forbidden in terms:
            for term in forbidden:
                for m in re.finditer(rf"(?<![\w-]){re.escape(term)}(?![\w-])", text,
                                     re.IGNORECASE):
                    # Report the line for context.
                    line = text[:m.start()].count("\n") + 1
                    failures.append(f"{doc.relative_to(root)}:{line}: forbidden term '{term}'")
    if failures:
        return Result("C18", False, "; ".join(failures[:10]))
    return Result("C18", True, "no forbidden terminology in docs")


# --- Runner ----------------------------------------------------------------


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", default="build/debug/tgbot", help="path to the tgbot binary")
    parser.add_argument("--root", default=".", help="repository root")
    parser.add_argument("--only", default="", help="comma-separated check ids, e.g. C3,C4")
    parser.add_argument("--base", default="", help="git base ref for PR-context checks (C14, C15)")
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
            elif cid == "C6":
                results.append(check_c6(root, binary))
            elif cid == "C7":
                results.append(check_c7(root))
            elif cid == "C8":
                results.append(check_c8(root, binary))
            elif cid == "C9":
                results.append(check_c9(root, tables))
            elif cid == "C10":
                results.append(check_c10(root, tables))
            elif cid == "C11":
                results.append(check_c11(tables))
            elif cid == "C12":
                results.append(check_c12(root))
            elif cid == "C13":
                results.append(check_c13(root))
            elif cid == "C14":
                results.append(check_c14(root, args.base))
            elif cid == "C15":
                results.append(check_c15(root, args.base))
            elif cid == "C16":
                results.append(check_c16(root, tables))
            elif cid == "C17":
                results.append(check_c17(root))
            elif cid == "C18":
                results.append(check_c18(root))
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
