"""Check that the pinned formatter, lint fixes and compiler agree on C++ source."""

from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from tools.config import ROOT, toolchain_tools


class FormatLintTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tools = toolchain_tools()

    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="vela-format-lint-")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.source = self.directory / "sample.cc"
        shutil.copyfile(ROOT / ".clang-format", self.directory / ".clang-format")

    def format(self) -> None:
        subprocess.run([str(self.tools["clang_format"]), "-i", str(self.source)], check=True)

    def lint(self, *, fix: bool = False) -> subprocess.CompletedProcess:
        command = [str(self.tools["clang_tidy"]), str(self.source),
                   f"--config-file={ROOT / '.clang-tidy'}", "--warnings-as-errors=*", "--quiet"]
        if fix:
            command.extend(["--fix", "--format-style=file"])
        return subprocess.run([*command, "--", "-std=c++23"], capture_output=True,
                              text=True, encoding="utf-8", check=False)

    def compile(self) -> None:
        flags = (["/nologo", "/Zs", "/W4", "/WX", "/clang:-std=c++23"]
                 if sys.platform == "win32" else
                 ["-fsyntax-only", "-std=c++23", "-Wall", "-Wextra", "-Werror"])
        result = subprocess.run([str(self.tools["cxx"]), *flags, str(self.source)],
                                capture_output=True, text=True, encoding="utf-8", check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def assert_stable(self) -> None:
        self.format()
        formatted = self.source.read_bytes()
        result = self.lint()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = self.lint(fix=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.format()
        self.assertEqual(self.source.read_bytes(), formatted)
        self.compile()

    def test_nested_designated_initializers_keep_separators_after_fix_and_format(self) -> None:
        self.source.write_text("""#include <cstdint>
enum class Operation : std::uint8_t { kScheduleAsynchronousContinuation };
struct Metadata { int count = 0; };
struct Descriptor {
  Operation operation = Operation::kScheduleAsynchronousContinuation;
  Metadata signature;
  bool receiver = false;
};
Descriptor MakeDescriptor() {
  return {.operation = Operation::kScheduleAsynchronousContinuation,
          .signature = {},
          .receiver = true};
}
""", encoding="utf-8", newline="\n")
        result = self.lint(fix=True)
        self.assertIn("readability-trailing-comma", result.stdout + result.stderr)
        self.assert_stable()

    def test_redundant_member_cleanup_keeps_explicit_result_initialization_valid(self) -> None:
        self.source.write_text("""struct Payload { Payload() noexcept; };
struct Result { int value = 0; Payload payload = {}; };
Result MakeResult() { return {.value = 1, .payload = {}}; }
""", encoding="utf-8", newline="\n")
        result = self.lint(fix=True)
        self.assertIn("readability-redundant-member-init", result.stdout + result.stderr)
        self.assert_stable()


if __name__ == "__main__":
    unittest.main()
