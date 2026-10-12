"""Tests for the ``python -m nacho_moderator`` entry point."""

from __future__ import annotations

import runpy
import sys

import pytest


def test_main_module_runs_the_cli(monkeypatch: pytest.MonkeyPatch) -> None:
    """Running ``python -m nacho_moderator --version`` exits 0."""
    monkeypatch.setattr(sys, "argv", ["nacho-moderator", "--version"])
    with pytest.raises(SystemExit) as excinfo:
        runpy.run_module("nacho_moderator", run_name="__main__", alter_sys=True)
    assert excinfo.value.code == 0
