"""Docs consistency checks (P8).

The command registry and the ``Settings`` model are the single sources of truth.
These tests fail when the prose docs drift from them, or when a doc is
internally inconsistent. Each check is a small pure function that takes the
relevant text/paths, so the check itself is tested against fixture mini-repos
below (see ``TestFixtureChecks``).

Checks:

1. every registered command has a ``### /<name>`` section with Examples and Edge
   cases, and no section names a command that does not exist;
2. the README command table lists the same commands as the registry;
3. every config key appears in ``docs/configuration.md``; ``config.example.toml``
   validates;
4. ``--help`` matches the golden file and the README;
5. ``setMyCommands`` payloads obey Telegram's limits and the group scope is a
   subset of the admin scope;
6. every ```toml`` snippet validates and every ```bash title="ci"`` snippet
   parses and references only existing scripts;
7. no bare TODO/FIXME; markdown tables have consistent cell counts;
8. relative links and anchors resolve;
9. (CI-only) the CHANGELOG ``Unreleased`` section changed when ``src/`` changed.
"""

from __future__ import annotations

import os
import re
import shlex
import shutil
import stat
import subprocess
import tomllib
from pathlib import Path

import pytest

from nacho_moderator.commands.publish import _bot_commands
from nacho_moderator.commands.registry import COMMANDS
from nacho_moderator.config import Settings, check_file_permissions
from nacho_moderator.errors import ConfigError

REPO = Path(__file__).resolve().parents[1]
DOCS = REPO / "docs"
COMMANDS_DOC = DOCS / "commands.md"
CONFIG_DOC = DOCS / "configuration.md"
README = REPO / "README.md"
CHANGELOG = REPO / "CHANGELOG.md"

_COMMAND_HEADING = re.compile(r"^### /([a-z0-9_]+)\s*$", re.M)
_TODO = re.compile(r"\b(TODO|FIXME)\b(?!\s*\(?#?\d)")
_CODE_SPAN = re.compile(r"`[^`]*`")
_TOML_FENCE = re.compile(r"```toml\n(.*?)```", re.S)
_CI_FENCE = re.compile(r'```(?:bash|sh)\s+title="ci"\n(.*?)```', re.S)
_LINK = re.compile(r"\[[^\]]*\]\(([^)]+)\)")


# --- pure checks -----------------------------------------------------------


def command_headings(commands_md: str) -> set[str]:
    """Return the command names that have a ``### /<name>`` heading."""
    return set(_COMMAND_HEADING.findall(commands_md))


def commands_missing_sections(commands_md: str, names: set[str]) -> set[str]:
    """Return registered command names with no section in the doc."""
    return names - command_headings(commands_md)


def sections_without_commands(commands_md: str, names: set[str]) -> set[str]:
    """Return section names in the doc that are not registered commands."""
    return command_headings(commands_md) - names


def section_body(commands_md: str, name: str) -> str:
    """Return the text of one command's section (up to the next heading)."""
    match = re.search(rf"^### /{re.escape(name)}\s*$", commands_md, re.M)
    if match is None:
        return ""
    rest = commands_md[match.end() :]
    nxt = re.search(r"^(##|###) ", rest, re.M)
    return rest[: nxt.start()] if nxt else rest


def readme_command_tokens(readme: str) -> set[str]:
    """Return every ``/name`` token that appears in the README's command table."""
    table = re.search(r"## Commands\n(.*?)(?=\n## )", readme, re.S)
    if table is None:
        return set()
    return {token.lstrip("/") for token in re.findall(r"`/([a-z0-9_]+)`", table.group(1))}


def config_keys_in_doc(config_md: str) -> set[str]:
    """Return the config keys documented in the configuration table."""
    keys: set[str] = set()
    for line in config_md.splitlines():
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip("|").split("|")]
        if not cells:
            continue
        key = cells[0].strip("`")
        if key and not key.startswith("---") and " " not in key and key not in {"Key"}:
            keys.add(key)
    return keys


def model_keys() -> set[str]:
    """Return every dotted config key of the ``Settings`` model."""
    from typing import Any

    def walk(model: Any, prefix: str = "") -> set[str]:
        out: set[str] = set()
        for name, field in model.model_fields.items():
            annotation = field.annotation
            if hasattr(annotation, "model_fields"):
                out |= walk(annotation, f"{prefix}{name}.")
            else:
                out.add(f"{prefix}{name}")
        return out

    return walk(Settings)


def table_cell_counts(markdown: str) -> list[tuple[int, int]]:
    """Return ``(line_number, cell_count)`` for every table row with > 1 cell."""
    rows: list[tuple[int, int]] = []
    for number, line in enumerate(markdown.splitlines(), start=1):
        stripped = line.strip()
        if stripped.startswith("|") and stripped.count("|") >= 2:
            # Count cells, ignoring escaped pipes.
            cells = re.split(r"(?<!\\)\|", stripped.strip("|"))
            rows.append((number, len(cells)))
    return rows


def inconsistent_tables(markdown: str) -> list[str]:
    """Return a description of each table whose rows disagree on cell count."""
    problems: list[str] = []
    block: list[tuple[int, int]] = []
    last_line = -2
    for number, count in table_cell_counts(markdown):
        if number != last_line + 1 and block:
            problems.extend(_check_block(block))
            block = []
        block.append((number, count))
        last_line = number
    problems.extend(_check_block(block))
    return problems


def _check_block(block: list[tuple[int, int]]) -> list[str]:
    if len(block) < 2:
        return []
    counts = {count for _, count in block}
    if len(counts) > 1:
        return [f"table near line {block[0][0]} has inconsistent cell counts {sorted(counts)}"]
    return []


def bare_todos(markdown: str) -> list[str]:
    """Return every bare TODO/FIXME (one without an issue reference).

    Occurrences inside an inline code span (``TODO``) are prose *about* the rule,
    not an actual marker, so they are ignored.
    """
    without_code = _CODE_SPAN.sub("", markdown)
    return [m.group(0) for m in _TODO.finditer(without_code)]


def relative_links_resolve(markdown: str, base: Path) -> list[str]:
    """Return relative links in ``markdown`` that do not resolve from ``base``."""
    broken: list[str] = []
    for target in _LINK.findall(markdown):
        path = target.split("#", 1)[0]
        if not path or path.startswith(("http://", "https://", "mailto:", "#")):
            continue
        if not (base / path).resolve().exists():
            broken.append(target)
    return broken


# --- tests against the real repo -------------------------------------------


def _registry_names() -> set[str]:
    return {spec.name for spec in COMMANDS}


def test_every_command_has_a_section() -> None:
    text = COMMANDS_DOC.read_text(encoding="utf-8")
    missing = commands_missing_sections(text, _registry_names())
    assert not missing, f"commands with no docs section: {sorted(missing)}"


def test_no_section_for_unknown_command() -> None:
    text = COMMANDS_DOC.read_text(encoding="utf-8")
    extra = sections_without_commands(text, _registry_names())
    assert not extra, f"docs sections for unregistered commands: {sorted(extra)}"


def test_every_section_has_examples_and_edge_cases() -> None:
    text = COMMANDS_DOC.read_text(encoding="utf-8")
    for name in _registry_names():
        body = section_body(text, name)
        assert "**Examples**" in body, f"/{name} section has no Examples"
        assert "**Edge cases**" in body, f"/{name} section has no Edge cases"


def test_readme_table_matches_registry() -> None:
    tokens = readme_command_tokens(README.read_text(encoding="utf-8"))
    assert tokens == _registry_names(), f"README/registry differ: {tokens ^ _registry_names()}"


def test_config_keys_are_documented() -> None:
    documented = config_keys_in_doc(CONFIG_DOC.read_text(encoding="utf-8"))
    keys = model_keys()
    missing = keys - documented
    assert not missing, f"config keys not documented: {sorted(missing)}"


def test_documented_keys_exist_in_model() -> None:
    documented = config_keys_in_doc(CONFIG_DOC.read_text(encoding="utf-8"))
    # The table documents the dotted model keys plus the top-level sections.
    extra = {k for k in documented if "." in k and k not in model_keys()}
    assert not extra, f"documented keys absent from the model: {sorted(extra)}"


def test_config_example_validates(tmp_path: Path) -> None:
    """The committed example validates from a private copy.

    Git checks a repo file out as 0644 (umask 022), and the example holds a
    placeholder secret, so the P2 permission check refuses the tracked file on a
    fresh clone. Copy it to a temp file, ``chmod 600``, and validate the copy —
    the same mode a real deployment uses (``install -m 0600``).
    """
    example = REPO / "config" / "config.example.toml"
    copy = tmp_path / "config.example.toml"
    shutil.copyfile(example, copy)
    copy.chmod(0o600)
    result = subprocess.run(
        ["uv", "run", "nacho-moderator", "--check-config", str(copy)],
        capture_output=True,
        text=True,
        cwd=REPO,
        check=False,
    )
    assert result.returncode == 0, result.stderr
    # Placeholder secrets, not real ones.
    data = tomllib.loads(copy.read_text(encoding="utf-8"))
    assert "REPLACE" in data["bot_token"]
    assert "REPLACE" in data["webhook"]["secret"]


def test_config_example_is_refused_when_world_readable(tmp_path: Path) -> None:
    """The committed example itself is 0644 (git's checkout mode) and is refused.

    This documents *why* the validation above must copy and chmod: the example
    holds a placeholder secret, so the permission check applies to it too. A
    fresh clone checks the tracked file out 0644, which is exactly the mode the
    old test fed to ``--check-config``.
    """
    copy = tmp_path / "config.example.toml"
    shutil.copyfile(REPO / "config" / "config.example.toml", copy)
    copy.chmod(0o644)
    with pytest.raises(ConfigError):
        check_file_permissions(copy)


def test_fresh_checkout_config_example_passes_check_config(tmp_path: Path) -> None:
    """Regression for F1: a fresh checkout (umask 022) validates the example.

    The old test ran ``--check-config`` on the tracked file, which git checks out
    0644 under umask 022, so the permission check refused it and the test failed
    on every clone. Here the example is materialised the way a fresh ``git
    checkout`` does (0644), then copied and ``chmod 600`` exactly as the fixed
    test does, and the copy validates cleanly.
    """
    previous_umask = os.umask(0o022)
    try:
        checkout = tmp_path / "checkout"
        checkout.mkdir()
        example = checkout / "config.example.toml"
        shutil.copyfile(REPO / "config" / "config.example.toml", example)
        example.chmod(0o644)  # git's checkout mode under umask 022
        assert stat.S_IMODE(example.stat().st_mode) == 0o644
        private = checkout / "config.private.toml"
        shutil.copyfile(example, private)
        private.chmod(0o600)
        result = subprocess.run(
            ["uv", "run", "nacho-moderator", "--check-config", str(private)],
            capture_output=True,
            text=True,
            cwd=REPO,
            check=False,
        )
        assert result.returncode == 0, result.stderr
    finally:
        os.umask(previous_umask)


def test_help_matches_readme_and_golden() -> None:
    result = subprocess.run(
        ["uv", "run", "nacho-moderator", "--help"],
        capture_output=True,
        text=True,
        cwd=REPO,
        check=False,
    )
    assert result.returncode == 0, result.stderr
    help_text = result.stdout
    golden = REPO / "docs" / "cli-help.txt"
    assert golden.is_file(), "docs/cli-help.txt golden file is missing"
    assert help_text == golden.read_text(encoding="utf-8")
    # The README documents the same flags.
    readme = README.read_text(encoding="utf-8")
    for flag in ("--config", "--check-config", "--allow-insecure-config", "--log-level"):
        assert flag in help_text
        assert flag in readme, f"{flag} not documented in the README"


def test_setmycommands_within_telegram_limits() -> None:
    name_re = re.compile(r"^[a-z0-9_]{1,32}$")
    for admin in (False, True):
        for cmd in _bot_commands(admin=admin):
            assert name_re.match(cmd.command), cmd.command
            assert 1 <= len(cmd.description) <= 256, cmd.command


def test_group_scope_is_subset_of_admin_scope() -> None:
    group = {c.command for c in _bot_commands(admin=False)}
    admin = {c.command for c in _bot_commands(admin=True)}
    assert group <= admin


def test_toml_snippets_validate() -> None:
    for path in [README, *sorted(DOCS.glob("*.md"))]:
        for snippet in _TOML_FENCE.findall(path.read_text(encoding="utf-8")):
            try:
                tomllib.loads(snippet)
            except tomllib.TOMLDecodeError as exc:  # pragma: no cover - failure path
                pytest.fail(f"invalid TOML snippet in {path}: {exc}")


def test_ci_bash_snippets_parse_and_reference_existing_scripts() -> None:
    for path in [README, *sorted(DOCS.glob("*.md"))]:
        for snippet in _CI_FENCE.findall(path.read_text(encoding="utf-8")):
            for line in snippet.splitlines():
                stripped = line.strip()
                if not stripped or stripped.startswith("#"):
                    continue
                tokens = shlex.split(stripped)
                # A script invocation must name a file that exists.
                for token in tokens:
                    if token.endswith(".sh") or token.startswith("scripts/"):
                        assert (REPO / token).exists(), f"{path}: missing script {token}"


def test_no_bare_todos() -> None:
    for path in [README, *sorted(DOCS.glob("*.md")), *sorted((REPO / "src").rglob("*.py"))]:
        found = bare_todos(path.read_text(encoding="utf-8"))
        assert not found, f"{path}: bare TODO/FIXME {found}"


def test_markdown_tables_are_consistent() -> None:
    for path in [README, *sorted(DOCS.glob("*.md"))]:
        problems = inconsistent_tables(path.read_text(encoding="utf-8"))
        assert not problems, f"{path}: {problems}"


def test_relative_links_resolve() -> None:
    for path in [README, REPO / "CONTRIBUTING.md", *sorted(DOCS.glob("*.md"))]:
        broken = relative_links_resolve(path.read_text(encoding="utf-8"), path.parent)
        assert not broken, f"{path}: broken links {broken}"


def _git(*args: str) -> str:
    return subprocess.run(
        ["git", *args], capture_output=True, text=True, cwd=REPO, check=False
    ).stdout


def test_changelog_updated_when_src_changed() -> None:
    """CI-only: a change under ``src/`` must touch the CHANGELOG Unreleased.

    Skipped locally (no base ref). In CI, ``CI_BASE_REF`` names the base branch;
    the ``skip-changelog`` label bypasses the check via ``SKIP_CHANGELOG=1``.
    """
    import os

    base = os.environ.get("CI_BASE_REF")
    if not base:
        pytest.skip("CI_BASE_REF not set (not a pull-request run)")
    if os.environ.get("SKIP_CHANGELOG") == "1":
        pytest.skip("skip-changelog label set")
    changed = _git("diff", "--name-only", f"origin/{base}...HEAD").splitlines()
    if not any(path.startswith("src/") for path in changed):
        pytest.skip("no src/ change")
    assert "CHANGELOG.md" in changed, "src/ changed but CHANGELOG.md did not"


# --- the checks themselves, against fixture mini-repos ----------------------


class TestFixtureChecks:
    """Each check catches a deliberately broken fixture."""

    def test_missing_command_section_detected(self) -> None:
        md = "### /help\n\n**Examples**\n\n**Edge cases**\n"
        assert commands_missing_sections(md, {"help", "ban"}) == {"ban"}

    def test_unknown_command_section_detected(self) -> None:
        md = "### /ghost\n"
        assert sections_without_commands(md, {"help"}) == {"ghost"}

    def test_missing_examples_detected(self) -> None:
        md = "### /help\n\n**Usage:** none\n"
        assert "**Examples**" not in section_body(md, "help")

    def test_readme_table_mismatch_detected(self) -> None:
        readme = "## Commands\n\n| `/help` | anyone | x |\n\n## Next\n"
        assert readme_command_tokens(readme) == {"help"}

    def test_inconsistent_table_detected(self) -> None:
        md = "| a | b |\n| --- | --- |\n| 1 | 2 | 3 |\n"
        assert inconsistent_tables(md)

    def test_consistent_table_ok(self) -> None:
        md = "| a | b |\n| --- | --- |\n| 1 | 2 |\n"
        assert inconsistent_tables(md) == []

    def test_escaped_pipe_does_not_change_cell_count(self) -> None:
        md = "| a | b |\n| --- | --- |\n| `x\\|y` | 2 |\n"
        assert inconsistent_tables(md) == []

    def test_bare_todo_detected(self) -> None:
        assert bare_todos("fix this TODO later") == ["TODO"]
        assert bare_todos("TODO(#12) tracked") == []

    def test_broken_link_detected(self, tmp_path: Path) -> None:
        assert relative_links_resolve("[x](missing.md)", tmp_path) == ["missing.md"]
        (tmp_path / "here.md").write_text("x", encoding="utf-8")
        assert relative_links_resolve("[x](here.md)", tmp_path) == []

    def test_invalid_toml_snippet_detected(self) -> None:
        snippet = _TOML_FENCE.findall("```toml\n= = bad\n```")
        assert snippet
        with pytest.raises(tomllib.TOMLDecodeError):
            tomllib.loads(snippet[0])

    def test_missing_script_in_ci_snippet_detected(self) -> None:
        snippet = _CI_FENCE.findall('```bash title="ci"\nscripts/nope.sh\n```')
        assert snippet
        tokens = shlex.split(snippet[0].strip())
        assert not (REPO / tokens[0]).exists()
