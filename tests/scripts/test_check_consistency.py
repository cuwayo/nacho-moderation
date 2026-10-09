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
import os
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
