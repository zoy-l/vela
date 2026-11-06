"""Shared filesystem and executable ownership for shipped-command tests."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import tempfile
import time
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


def closed_output_command(test: unittest.TestCase, arguments: list[str | Path], *, cwd: Path,
                          env: dict[str, str] | None = None) -> str:
    with subprocess.Popen(list(map(str, arguments)), cwd=cwd, env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          text=True, encoding="utf-8") as process:
        process.stdout.close()
        process.stdout = None
        try:
            _, errors = process.communicate(timeout=30)
        finally:
            if process.poll() is None:
                process.kill()
        test.assertEqual(process.returncode, 1, errors)
        return errors


def pending_command(test: unittest.TestCase, arguments: list[str | Path], readiness: bytes, *,
                    cwd: Path, env: dict[str, str] | None = None) -> None:
    with tempfile.TemporaryFile() as output, tempfile.TemporaryFile() as errors:
        with subprocess.Popen(list(map(str, arguments)), cwd=cwd, env=env,
                              stdout=output, stderr=errors) as process:
            try:
                deadline = time.monotonic() + 30
                while os.fstat(output.fileno()).st_size < len(readiness):
                    remaining = deadline - time.monotonic()
                    test.assertGreater(remaining, 0, "main did not publish its readiness marker")
                    try:
                        process.wait(timeout=min(0.05, remaining))
                    except subprocess.TimeoutExpired:
                        continue
                    test.fail("program exited before remaining pending")
                with test.assertRaises(subprocess.TimeoutExpired):
                    process.wait(timeout=0.5)
            finally:
                process.kill()
        output.seek(0)
        errors.seek(0)
        test.assertTrue(output.read().startswith(readiness), "main readiness marker was not output")
        test.assertEqual(errors.read(), b"")


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
