"""Shared filesystem and executable ownership for shipped-command tests."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

from tools.config import ROOT, executable_name, static_library_name, toolchain_tools
from tools.package import create_package


def vela_executable() -> Path:
    return Path(os.environ.get(
        "VELA_TEST_EXECUTABLE", ROOT / "out" / "debug" / executable_name("vela")))


def native_test_libraries(output: Path, runtime: Path | None = None) -> list[Path]:
    return [runtime if runtime is not None else
            output / "obj/runtime" / static_library_name("vela_runtime"),
            *[output / "obj/third_party" / static_library_name(name)
              for name in ("gtest_main", "gtest")]]


def create_native_package(executable: Path, destination: Path) -> dict[str, str]:
    specification = os.environ.get("VELA_TEST_TARGET_SUPPORT_SPEC")
    return create_package(executable, executable.parent / "obj/runtime" /
                          static_library_name("vela_runtime"), destination,
                          Path(specification) if specification else None)


def compile_native_host(test: unittest.TestCase, sources: list[Path], executable: Path,
                        libraries: list[Path], includes: list[Path], *, cwd: Path) -> None:
    tools = toolchain_tools()
    if sys.platform == "win32":
        objects = []
        for source in sources:
            obj = cwd / (source.stem + ".obj")
            native_command(test, [tools["cxx"], "/nologo", "/MT", "/O2", "/clang:-std=c++23",
                                  "/D_ITERATOR_DEBUG_LEVEL=0", "/D_HAS_EXCEPTIONS=0", "/c", source,
                                  "/Fo" + str(obj), *["/I" + str(path) for path in includes]], cwd=cwd)
            objects.append(obj)
        command = [tools["cxx"], "/nologo", "/MT", "-fuse-ld=lld", "/Fe" + str(executable),
                   *objects, *libraries]
    else:
        flags = ["-std=c++23", "-O2", "-stdlib=libc++", "-fno-exceptions", "-pthread",
                 "-fuse-ld=" + str(tools["linker"])]
        if sys.platform == "darwin":
            flags += ["-isysroot", str(tools["sysroot"]), "-nostdinc++", "-isystem",
                      str(tools["sysroot"] / "usr/include/c++/v1"), "-framework", "CoreFoundation"]
        command = [tools["cxx"], *flags, *["-I" + str(path) for path in includes],
                   *sources, *libraries, "-o", executable]
    native_command(test, command, cwd=cwd)


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
