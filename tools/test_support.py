"""Shared filesystem and executable ownership for shipped-command tests."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from tools.config import ROOT, executable_name


def vela_executable() -> Path:
    return Path(os.environ.get(
        "VELA_TEST_EXECUTABLE", ROOT / "out" / "debug" / executable_name("vela")))


def native_command(test: unittest.TestCase, arguments: list[str | Path], expected: int = 0,
                   env: dict[str, str] | None = None, *, cwd: Path) -> subprocess.CompletedProcess:
    result = subprocess.run(list(map(str, arguments)), cwd=cwd, env=env,
                            capture_output=True, text=True, encoding="utf-8",
                            errors="replace", timeout=120, check=False)
    test.assertEqual(result.returncode, expected, result.stdout + result.stderr)
    return result


class CommandTestCase(unittest.TestCase):
    directory_prefix = "vela-command-"

    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix=self.directory_prefix)
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name).resolve()
        self.executable = vela_executable()

    def write(self, name: str, text: str) -> Path:
        path = self.directory / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8", newline="\n")
        return path

    def invoke(self, *arguments: str, code: int = 0,
               cwd: Path | None = None) -> subprocess.CompletedProcess:
        result = subprocess.run([str(self.executable), *arguments], cwd=cwd or self.directory,
                                capture_output=True, text=True, encoding="utf-8", timeout=30,
                                check=False)
        self.assertEqual(result.returncode, code, result.stdout + result.stderr)
        return result
