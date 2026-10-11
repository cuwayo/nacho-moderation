"""Tests for scripts/check_consistency.py (SPEC-ADDENDUM-SSOT S7).

Each implemented check (C1-C5) is exercised with a passing fixture mini-repo and
a deliberately broken variant that must fail. The mini-repo is generated from the
real `tgbot` binary so the tests stay in sync with the SSOT tables.

The binary is located via $NM_TGBOT_BIN, else build/debug/tgbot under the repo
root; the whole module is skipped when no binary is available (e.g. a docs-only
checkout) so `ctest` never fails for a missing build.
"""

from __future__ import annotations

import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = REPO_ROOT / "scripts"


def _find_binary() -> Path | None:
    env = os.environ.get("NM_TGBOT_BIN")
    candidates = [Path(env)] if env else []
    candidates += [REPO_ROOT / "build" / p / "tgbot" for p in ("debug", "asan", "release")]
    for c in candidates:
        if c.is_file():
            return c
    return None


BINARY = _find_binary()

# Import check_consistency as a module so we can call individual checks.
_spec = importlib.util.spec_from_file_location(
    "check_consistency", SCRIPTS / "check_consistency.py"
)
check_consistency = importlib.util.module_from_spec(_spec)
assert _spec and _spec.loader
_spec.loader.exec_module(check_consistency)


@unittest.skipIf(BINARY is None, "no tgbot binary built")
class ConsistencyFixtureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tables = check_consistency.run_json(BINARY, "--dump-tables-json")

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="nm_consistency_"))
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)
        (self.tmp / "docs" / "commands").mkdir(parents=True)
        (self.tmp / "config").mkdir(parents=True)
        (self.tmp / "scripts").mkdir(parents=True)
        shutil.copy(SCRIPTS / "docgen.py", self.tmp / "scripts" / "docgen.py")
        shutil.copy(
            SCRIPTS / "check_consistency.py", self.tmp / "scripts" / "check_consistency.py"
        )

        # Seed docs with generated blocks, then fill them from the real binary.
        (self.tmp / "docs" / "commands.md").write_text(
            "# Commands\n\n<!-- BEGIN GENERATED: commands-reference -->\n"
            "<!-- END GENERATED: commands-reference -->\n",
            encoding="utf-8",
        )
        (self.tmp / "docs" / "configuration.md").write_text(
            "# Configuration\n\n<!-- BEGIN GENERATED: config-table -->\n"
            "<!-- END GENERATED: config-table -->\n\n"
            "<!-- BEGIN GENERATED: config-reference -->\n"
            "<!-- END GENERATED: config-reference -->\n",
            encoding="utf-8",
        )
        for c in self.tables["commands"]:
            (self.tmp / "docs" / "commands" / f"{c['id']}.md").write_text(
                f"# /{c['id']}\n\n## Examples\n\n```text\n/{c['id']}\n```\n\n"
                "## Edge cases\n\n- none yet\n",
                encoding="utf-8",
            )
        subprocess.run(
            [sys.executable, str(self.tmp / "scripts" / "docgen.py"), "--update",
             "--bin", str(BINARY), "--root", str(self.tmp)],
            check=True,
        )
        sample = subprocess.run(
            [str(BINARY), "--dump-config-example"], check=True, capture_output=True, text=True
        ).stdout
        (self.tmp / "config" / "config.example.toml").write_text(sample, encoding="utf-8")

        # C9 fixture: a mock server registry and a docs row per method.
        (self.tmp / "tests" / "mock").mkdir(parents=True)
        methods = [m["method"] for m in self.tables.get("api_methods", [])]
        (self.tmp / "tests" / "mock" / "mock_tg_server.py").write_text(
            "HANDLERS = [" + ", ".join(f'"{m}"' for m in methods) + "]\n", encoding="utf-8"
        )
        (self.tmp / "docs" / "telegram-api-notes.md").write_text(
            "\n".join(f"| `{m}` | global | yes |" for m in methods) + "\n", encoding="utf-8"
        )

        # C8/C10/C12/C16/C17/C18 fixtures.
        (self.tmp / "tests" / "golden").mkdir(parents=True)
        help_text = subprocess.run(
            [str(BINARY), "--help"], check=True, capture_output=True, text=True
        ).stdout
        (self.tmp / "tests" / "golden" / "help.txt").write_text(help_text, encoding="utf-8")
        (self.tmp / "docs" / "building.md").write_text(
            "# Building\n\n```text\n" + help_text + "```\n", encoding="utf-8"
        )
        (self.tmp / "docs" / "man").mkdir(parents=True)
        (self.tmp / "docs" / "man" / "tgbot.1.scd").write_text(
            "\n".join(f"`/{c['id']}`" for c in self.tables["commands"]) + "\n", encoding="utf-8"
        )
        (self.tmp / "docs" / "man" / "tgbot.toml.5.scd").write_text(
            "\n".join(f"`{k['key']}`" for k in self.tables["config"]) + "\n", encoding="utf-8"
        )
        (self.tmp / "docs" / "groups.dox").write_text(
            "\n".join(f"@defgroup {g}" for g in self._src_groups()) + "\n", encoding="utf-8"
        )
        (self.tmp / "src").mkdir(parents=True)
        (self.tmp / "src" / "sample.c").write_text(
            "/**\n * @file sample.c\n * @ingroup util\n */\n", encoding="utf-8"
        )
        (self.tmp / "docs" / "architecture.md").write_text(
            "\n".join(f"| {e['id']} |" for e in self.tables.get("errors", [])) + "\n"
            + "\n".join(f"`{m['name']}`" for m in self.tables.get("metrics", [])) + "\n",
            encoding="utf-8",
        )
        (self.tmp / "docs" / "terminology.txt").write_text(
            "chat = group chat, room\n", encoding="utf-8"
        )
        (self.tmp / "CMakePresets.json").write_text(
            json.dumps({"configurePresets": [{"name": "debug"}]}), encoding="utf-8"
        )

    @staticmethod
    def _src_groups() -> list[str]:
        text = (REPO_ROOT / "docs" / "groups.dox").read_text(encoding="utf-8")
        return re.findall(r"@defgroup\s+([a-z0-9_]+)", text)

    # --- passing fixtures ---------------------------------------------------

    def test_c1_passes_on_fresh_repo(self):
        self.assertTrue(check_consistency.check_c1(self.tmp, BINARY).passed)

    def test_c2_passes_on_fresh_repo(self):
        self.assertTrue(check_consistency.check_c2(self.tmp, self.tables).passed)

    def test_c3_passes_on_fresh_repo(self):
        self.assertTrue(check_consistency.check_c3(self.tmp, self.tables).passed)

    def test_c4_passes_on_fresh_repo(self):
        self.assertTrue(check_consistency.check_c4(self.tmp, self.tables).passed)

    def test_c5_passes_on_fresh_repo(self):
        self.assertTrue(check_consistency.check_c5(self.tmp, BINARY, self.tables).passed)

    def test_c9_passes_on_fresh_repo(self):
        self.assertTrue(check_consistency.check_c9(self.tmp, self.tables).passed)

    def test_c6_passes_on_fresh_repo(self):
        self.assertTrue(check_consistency.check_c6(self.tmp, BINARY).passed)

    def test_c7_passes_on_fresh_repo(self):
        (self.tmp / "README.md").write_text(
            '# R\n\n```sh title="ci"\ncmake --preset debug\n```\n', encoding="utf-8"
        )
        self.assertTrue(check_consistency.check_c7(self.tmp).passed)

    def test_c8_passes_on_fresh_repo(self):
        self.assertTrue(check_consistency.check_c8(self.tmp, BINARY).passed)

    def test_c10_passes_on_fresh_repo(self):
        self.assertTrue(check_consistency.check_c10(self.tmp, self.tables).passed)

    def test_c11_passes_on_fresh_repo(self):
        self.assertTrue(check_consistency.check_c11(self.tables).passed)

    def test_c12_passes_on_fresh_repo(self):
        self.assertTrue(check_consistency.check_c12(self.tmp).passed)

    def test_c13_passes_on_fresh_repo(self):
        self.assertTrue(check_consistency.check_c13(self.tmp).passed)

    def test_c16_passes_on_fresh_repo(self):
        self.assertTrue(check_consistency.check_c16(self.tmp, self.tables).passed)

    def test_c17_passes_on_fresh_repo(self):
        self.assertTrue(check_consistency.check_c17(self.tmp).passed)

    def test_c18_passes_on_fresh_repo(self):
        self.assertTrue(check_consistency.check_c18(self.tmp).passed)

    # --- failing fixtures ---------------------------------------------------

    def test_c1_fails_when_block_is_stale(self):
        doc = self.tmp / "docs" / "configuration.md"
        doc.write_text(doc.read_text(encoding="utf-8").replace("| `mode` |", "| `MODE` |"),
                       encoding="utf-8")
        self.assertFalse(check_consistency.check_c1(self.tmp, BINARY).passed)

    def test_c2_fails_when_heading_missing(self):
        doc = self.tmp / "docs" / "commands.md"
        text = doc.read_text(encoding="utf-8")
        doc.write_text(text.replace("### /help", "### /gone"), encoding="utf-8")
        self.assertFalse(check_consistency.check_c2(self.tmp, self.tables).passed)

    def test_c3_fails_when_snippet_lacks_section(self):
        snippet = self.tmp / "docs" / "commands" / "help.md"
        snippet.write_text("# /help\n\n## Examples\n\n- x\n", encoding="utf-8")
        self.assertFalse(check_consistency.check_c3(self.tmp, self.tables).passed)

    def test_c4_fails_when_key_undocumented(self):
        doc = self.tmp / "docs" / "configuration.md"
        text = doc.read_text(encoding="utf-8")
        # Drop the mode row from the config table only.
        doc.write_text("\n".join(l for l in text.splitlines() if "| `mode` |" not in l),
                       encoding="utf-8")
        self.assertFalse(check_consistency.check_c4(self.tmp, self.tables).passed)

    def test_c5_fails_when_sample_differs(self):
        sample = self.tmp / "config" / "config.example.toml"
        sample.write_text(sample.read_text(encoding="utf-8") + "\n# tampered\n", encoding="utf-8")
        self.assertFalse(check_consistency.check_c5(self.tmp, BINARY, self.tables).passed)

    def test_c6_fails_on_invalid_toml(self):
        (self.tmp / "docs" / "bad.md").write_text(
            "# Bad\n\n```toml\nbogus_key = 1\n```\n", encoding="utf-8"
        )
        self.assertFalse(check_consistency.check_c6(self.tmp, BINARY).passed)

    def test_c7_fails_on_unknown_preset(self):
        (self.tmp / "README.md").write_text(
            '# R\n\n```sh title="ci"\ncmake --preset nonexistent\n```\n', encoding="utf-8"
        )
        self.assertFalse(check_consistency.check_c7(self.tmp).passed)

    def test_c8_fails_when_golden_differs(self):
        (self.tmp / "tests" / "golden" / "help.txt").write_text("stale\n", encoding="utf-8")
        self.assertFalse(check_consistency.check_c8(self.tmp, BINARY).passed)

    def test_c9_fails_when_mock_handler_missing(self):
        mock = self.tmp / "tests" / "mock" / "mock_tg_server.py"
        mock.write_text('HANDLERS = ["getMe"]\n', encoding="utf-8")
        self.assertFalse(check_consistency.check_c9(self.tmp, self.tables).passed)

    def test_c9_fails_when_docs_row_missing(self):
        notes = self.tmp / "docs" / "telegram-api-notes.md"
        notes.write_text("| `getMe` | global | yes |\n", encoding="utf-8")
        self.assertFalse(check_consistency.check_c9(self.tmp, self.tables).passed)

    def test_c10_fails_when_man_missing_command(self):
        man1 = self.tmp / "docs" / "man" / "tgbot.1.scd"
        man1.write_text("no commands here\n", encoding="utf-8")
        self.assertFalse(check_consistency.check_c10(self.tmp, self.tables).passed)

    def test_c11_fails_on_bad_name(self):
        bad = dict(self.tables)
        bad["commands"] = [{"id": "Bad Name", "brief": "x", "min_role": "ROLE_ANYONE",
                            "hidden": False}]
        self.assertFalse(check_consistency.check_c11(bad).passed)

    def test_c12_fails_on_unknown_group(self):
        (self.tmp / "src" / "sample.c").write_text(
            "/**\n * @file sample.c\n * @ingroup nosuchgroup\n */\n", encoding="utf-8"
        )
        self.assertFalse(check_consistency.check_c12(self.tmp).passed)

    def test_c13_fails_on_bare_todo(self):
        marker = "TO" + "DO"  # build the token so this test file passes C13 itself
        (self.tmp / "src" / "sample.c").write_text(
            "/**\n * @file sample.c\n * @ingroup util\n */\n// " + marker + " fix this\n",
            encoding="utf-8",
        )
        self.assertFalse(check_consistency.check_c13(self.tmp).passed)

    def test_c16_fails_when_metric_undocumented(self):
        (self.tmp / "docs" / "architecture.md").write_text("nothing\n", encoding="utf-8")
        self.assertFalse(check_consistency.check_c16(self.tmp, self.tables).passed)

    def test_c17_fails_on_broken_link(self):
        (self.tmp / "docs" / "index.md").write_text(
            "# Index\n\n[gone](does-not-exist.md)\n", encoding="utf-8"
        )
        self.assertFalse(check_consistency.check_c17(self.tmp).passed)

    def test_c18_fails_on_forbidden_term(self):
        (self.tmp / "docs" / "prose.md").write_text(
            "# Prose\n\nUse the room for that.\n", encoding="utf-8"
        )
        self.assertFalse(check_consistency.check_c18(self.tmp).passed)


@unittest.skipIf(BINARY is None, "no tgbot binary built")
class CliSurfaceTests(unittest.TestCase):
    def test_list_runs(self):
        proc = subprocess.run(
            [sys.executable, str(SCRIPTS / "check_consistency.py"), "--list"],
            capture_output=True, text=True,
        )
        self.assertEqual(proc.returncode, 0)
        self.assertIn("C1:", proc.stdout)

    def test_tool_error_exit_code_2(self):
        proc = subprocess.run(
            [sys.executable, str(SCRIPTS / "check_consistency.py"),
             "--bin", str(self._tmp_missing())],
            capture_output=True, text=True,
        )
        self.assertEqual(proc.returncode, 2)

    @staticmethod
    def _tmp_missing() -> Path:
        return Path(tempfile.gettempdir()) / "definitely_no_tgbot_binary_here"


if __name__ == "__main__":
    unittest.main()
