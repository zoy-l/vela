"""Check that the pinned formatter, lint fixes and compiler agree on C++ source."""

from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from tools.config import ROOT, toolchain_tools
from tools.lint import check_source, header_contexts


class HeaderLintContextTest(unittest.TestCase):
    def test_private_headers_use_their_declaring_target(self) -> None:
        header = ROOT / "runtime/regexp/regexp_storage.h"
        targets = {
            "//runtime:runtime": {
                "sources": ["//runtime/regexp/regexp_storage.h", "//runtime/regexp/regexp.cc"],
                "defines": ["PCRE2_STATIC", "PCRE2_CODE_UNIT_WIDTH=8"],
                "include_dirs": ["//out/debug/gen/", "//include/"],
                "cflags": ["-isystem", "gen/third_party/pcre2"],
                "cflags_cc": ["-std=c++23"],
            },
            "//runtime:runtime_tests": {
                "sources": ["//runtime/regexp/regexp_unittest.cc"],
                "defines": ["TEST_CONTEXT"],
            },
        }
        arguments = header_contexts(targets, [header], Path("clang++"))[header]
        self.assertEqual(arguments[:2], ["-x", "c++"])
        self.assertIn("-DPCRE2_STATIC", arguments)
        self.assertIn("-DPCRE2_CODE_UNIT_WIDTH=8", arguments)
        self.assertIn("gen/third_party/pcre2", arguments)
        self.assertIn("-I" + str(ROOT / "out/debug/gen"), arguments)
        self.assertNotIn("-DTEST_CONTEXT", arguments)

    def test_clang_cl_headers_use_cpp_driver_mode(self) -> None:
        header = ROOT / "runtime/isolate/isolate.h"
        targets = {"//runtime:runtime": {
            "sources": ["//runtime/isolate/isolate.h"],
            "defines": ["NOMINMAX"], "include_dirs": ["//include/"],
            "cflags": ["/MT"], "cflags_cc": ["/clang:-std=c++23"],
        }}
        arguments = header_contexts(targets, [header], Path("clang-cl.exe"))[header]
        self.assertEqual(arguments[:2], ["--driver-mode=cl", "/TP"])
        self.assertIn("/DNOMINMAX", arguments)
        self.assertIn("/I" + str(ROOT / "include"), arguments)
        self.assertIn("/clang:-std=c++23", arguments)

    def test_rejects_missing_or_conflicting_header_owners(self) -> None:
        header = ROOT / "runtime/isolate/isolate.h"
        with self.assertRaisesRegex(RuntimeError, "owning GN target"):
            header_contexts({}, [header], Path("clang++"))
        targets = {
            "//runtime:first": {"sources": ["//runtime/isolate/isolate.h"], "defines": ["FIRST"]},
            "//runtime:second": {"sources": ["//runtime/isolate/isolate.h"], "defines": ["SECOND"]},
        }
        with self.assertRaisesRegex(RuntimeError, "conflicting GN compilation contexts"):
            header_contexts(targets, [header], Path("clang++"))

    def test_relative_header_flags_use_the_build_directory(self) -> None:
        header = ROOT / "runtime/regexp/regexp_storage.h"
        output = ROOT / "out/debug"
        arguments = ["-x", "c++", "-isystem", "gen/third_party/pcre2"]
        with patch("tools.lint.subprocess.run", return_value=subprocess.CompletedProcess([], 0, "")) as run:
            result = check_source(Path("clang-tidy"), header, output, fix=False, arguments=arguments)
        self.assertEqual(result.return_code, 0)
        self.assertEqual(run.call_args.kwargs["cwd"], output)
        self.assertEqual(run.call_args.args[0][-len(arguments):], arguments)


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

    def lint_fix(self) -> subprocess.CompletedProcess:
        command = [str(self.tools["clang_tidy"]), str(self.source),
                   f"--config-file={ROOT / '.clang-tidy'}", "--warnings-as-errors=*", "--quiet",
                   "--fix", "--format-style=file", "--", "-std=c++23"]
        return subprocess.run(command, capture_output=True,
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
        result = self.lint_fix()
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
        result = self.lint_fix()
        self.assertIn("readability-trailing-comma", result.stdout + result.stderr)
        self.assert_stable()

    def test_redundant_member_cleanup_keeps_explicit_result_initialization_valid(self) -> None:
        self.source.write_text("""struct Payload { Payload() noexcept; };
struct Result { int value = 0; Payload payload = {}; };
Result MakeResult() { return {.value = 1, .payload = {}}; }
""", encoding="utf-8", newline="\n")
        result = self.lint_fix()
        self.assertIn("readability-redundant-member-init", result.stdout + result.stderr)
        self.assert_stable()


if __name__ == "__main__":
    unittest.main()
