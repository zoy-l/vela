"""Check that the pinned formatter, lint fixes and compiler agree on C++ source."""

from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from tools.config import (MACOS_DEPLOYMENT_TARGET, ROOT, executable_name, gn_value, static_library_name, target_cpu,
                          target_os, toolchain_tools)
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


class ThinLtoTest(unittest.TestCase):
    def test_gn_caches_project_and_dependency_bitcode_without_changing_native_objects(self) -> None:
        tools = toolchain_tools()
        with tempfile.TemporaryDirectory(prefix="vela-thinlto build ") as temporary:
            root = Path(temporary)
            for relative in ("build/BUILDCONFIG.gn", "build/config/args.gni",
                             "build/config/BUILD.gn", "build/toolchain/BUILD.gn",
                             "build/scripts/gn_helper.py"):
                destination = root / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(ROOT / relative, destination)
            (root / ".gn").write_text('buildconfig = "//build/BUILDCONFIG.gn"\n',
                                      encoding="utf-8", newline="\n")
            (root / "BUILD.gn").write_text('''declare_args() {
  bitcode_dependency = false
}
static_library("values") {
  sources = [ "calculate.cc" ]
  if (bitcode_dependency) {
    if (target_os == "win") {
      cflags = [ "/clang:-flto=thin" ]
    } else {
      cflags = [ "-flto=thin" ]
    }
  }
}
executable("sample") {
  sources = [ "main.c" ]
  deps = [ ":values" ]
  output_dir = "$root_out_dir/bin with spaces"
}
''', encoding="utf-8", newline="\n")
            (root / "calculate.cc").write_text(
                'extern "C" int Calculate(int input) { return input * 2; }\n',
                encoding="utf-8", newline="\n")
            (root / "main.c").write_text(
                "int Calculate(int input);\nint main(void) { return Calculate(21) != 42; }\n",
                encoding="utf-8", newline="\n")
            for variant, bitcode_dependency in (("debug", False), ("debug", True),
                                                ("release", False)):
                with self.subTest(variant=variant, bitcode_dependency=bitcode_dependency):
                    project_bitcode = variant == "release"
                    output = root / (variant + ("-bitcode-sdk" if bitcode_dependency else ""))
                    binary = output / "bin with spaces" / executable_name("sample")
                    args = {
                        "cc_wrapper": "", "target_os": target_os(), "target_cpu": target_cpu(),
                        "vela_build_config": variant, "vela_thin_lto": project_bitcode,
                        "bitcode_dependency": bitcode_dependency,
                        "vela_cc": tools["cc"].as_posix(), "vela_cxx": tools["cxx"].as_posix(),
                        "vela_archiver": tools["archiver"].as_posix(),
                        "vela_linker": tools["linker"].as_posix(),
                        "vela_strip": tools["strip"].as_posix(),
                        "vela_python": Path(sys.executable).resolve().as_posix(),
                    }
                    if sys.platform == "darwin":
                        args["vela_sysroot"] = tools["sysroot"].as_posix()
                        args["vela_macos_deployment_target"] = MACOS_DEPLOYMENT_TARGET
                    encoded_args = " ".join(f"{key}={gn_value(value)}" for key, value in args.items())
                    commands = [
                        [tools["gn"], "gen", output, f"--root={root}", f"--args={encoded_args}"],
                        [tools["ninja"], "-C", output, "sample"],
                        [binary],
                    ]
                    for command in commands:
                        result = subprocess.run([str(value) for value in command], cwd=root,
                                                capture_output=True, text=True, encoding="utf-8",
                                                check=False)
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    extension = "*.obj" if sys.platform == "win32" else "*.o"
                    objects = list((output / "obj").rglob(extension))
                    self.assertEqual(len(objects), 2)
                    for obj in objects:
                        magic = obj.read_bytes()[:4]
                        expected_bitcode = (project_bitcode or
                                            (bitcode_dependency and "calculate" in obj.name))
                        self.assertEqual(magic in (b"BC\xc0\xde", b"\xde\xc0\x17\x0b"),
                                         expected_bitcode, str(obj))
                    cached = list((output / "thinlto-cache").glob("llvmcache-*"))
                    self.assertEqual(bool(cached), project_bitcode or bitcode_dependency)
                    nm = tools["cxx"].parent / executable_name("llvm-nm")
                    archive = output / "obj" / static_library_name("values")
                    symbols = subprocess.run([str(nm), "--defined-only", "-j", str(archive)],
                                             capture_output=True, text=True, encoding="utf-8",
                                             check=True).stdout
                    self.assertIn("Calculate", symbols)
                    if sys.platform != "win32":
                        symbols = subprocess.run(
                            [str(nm), "--defined-only", "-j", str(binary)], capture_output=True,
                            text=True, encoding="utf-8", check=True).stdout
                        self.assertEqual(any(symbol.lstrip("_") == "main"
                                             for symbol in symbols.splitlines()), variant == "debug")


if __name__ == "__main__":
    unittest.main()
