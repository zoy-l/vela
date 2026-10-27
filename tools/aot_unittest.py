"""Link emitted native objects with the standalone runtime and execute GoogleTest."""

from __future__ import annotations

import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

from tools.config import ROOT, executable_name, toolchain_tools


HOST_TESTS = r'''
#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <span>
#include <gtest/gtest.h>
#include "base/execution_status.h"
#include "runtime/isolate.h"
#include "runtime/native_abi.h"

using vela::ExecutionStatus;
using namespace vela::runtime;
using Entry = std::uint8_t (*)(NativeExecution*, const NativeValue*, std::uint64_t, NativeValue*);
extern "C" std::uint8_t INTEGER_ENTRY(NativeExecution*, const NativeValue*, std::uint64_t, NativeValue*);
extern "C" std::uint8_t FLOAT_ENTRY(NativeExecution*, const NativeValue*, std::uint64_t, NativeValue*);
extern "C" std::uint8_t BOOL_ENTRY(NativeExecution*, const NativeValue*, std::uint64_t, NativeValue*);
extern "C" std::uint8_t NULLABLE_ENTRY(NativeExecution*, const NativeValue*, std::uint64_t, NativeValue*);
extern "C" std::uint8_t NULL_TEST_ENTRY(NativeExecution*, const NativeValue*, std::uint64_t, NativeValue*);

NativeValue Integer(std::int64_t value) {
  return {.payload = std::bit_cast<std::uint64_t>(value), .kind = NativeValueKind::Integer};
}
NativeValue Double(double value) {
  return {.payload = std::bit_cast<std::uint64_t>(value), .kind = NativeValueKind::Double};
}
ExecutionStatus Execute(Entry entry, NativeExecution& execution,
                        std::span<const NativeValue> arguments, NativeValue& result) {
  return static_cast<ExecutionStatus>(entry(&execution, arguments.data(), arguments.size(), &result));
}

TEST(NativeHostTest, OptimizedIntegerCallsLoopsAndShiftSemantics) {
  Isolate isolate;
  NativeExecution execution(isolate);
  const std::array<std::array<std::int64_t, 4>, 7> cases{{
      {0, -3, -4, 12}, {0, 3, -4, -12}, {3, -1, 1, -2},
      {3, 1, -1, std::numeric_limits<std::int64_t>::min()},
      {4, -8, 65, -4}, {5, -1, 64, -1}, {6, 0, 0, 90},
  }};
  for (const auto& values : cases) {
    const std::array arguments{Integer(values[0]), Integer(values[1]), Integer(values[2])};
    NativeValue result;
    ASSERT_EQ(Execute(INTEGER_ENTRY, execution, arguments, result), ExecutionStatus::Returned);
    EXPECT_EQ(result.kind, NativeValueKind::Integer);
    EXPECT_EQ(std::bit_cast<std::int64_t>(result.payload), values[3]);
    EXPECT_EQ(execution.frame_count(), 0U);
  }
}

TEST(NativeHostTest, ErrorsAndDepthLimitsCleanUpAllNativeFrames) {
  Isolate isolate;
  NativeExecution execution(isolate);
  const auto minimum = std::numeric_limits<std::int64_t>::min();
  const auto maximum = std::numeric_limits<std::int64_t>::max();
  struct Case { std::int64_t operation; std::int64_t left; std::int64_t right; ExecutionStatus status; };
  const std::array cases{
      Case{0, maximum, 2, ExecutionStatus::IntegerOverflow},
      Case{1, minimum, -1, ExecutionStatus::IntegerOverflow},
      Case{2, minimum, -1, ExecutionStatus::IntegerOverflow},
      Case{1, 1, 0, ExecutionStatus::DivisionByZero},
      Case{7, 100, 0, ExecutionStatus::StackOverflow},
  };
  for (const auto& item : cases) {
    const std::array arguments{Integer(item.operation), Integer(item.left), Integer(item.right)};
    NativeValue result;
    EXPECT_EQ(Execute(INTEGER_ENTRY, execution, arguments, result), item.status);
    EXPECT_EQ(execution.frame_count(), 0U);
  }
}

TEST(NativeHostTest, FloatingValuesPreserveNanInfinityAndSignedZero) {
  Isolate isolate;
  NativeExecution execution(isolate);
  const std::array arguments{Double(-0.0), Double(1.0)};
  NativeValue result;
  ASSERT_EQ(Execute(FLOAT_ENTRY, execution, arguments, result), ExecutionStatus::Returned);
  EXPECT_EQ(result.kind, NativeValueKind::Double);
  EXPECT_EQ(result.payload, std::bit_cast<std::uint64_t>(-0.0));
  const std::array infinity{Double(std::numeric_limits<double>::infinity()), Double(1.0)};
  ASSERT_EQ(Execute(FLOAT_ENTRY, execution, infinity, result), ExecutionStatus::Returned);
  EXPECT_EQ(std::bit_cast<double>(result.payload), std::numeric_limits<double>::infinity());
  const std::array zero{Double(1.0), Double(-0.0)};
  EXPECT_EQ(Execute(FLOAT_ENTRY, execution, zero, result), ExecutionStatus::DivisionByZero);
  const std::array nan{Double(std::numeric_limits<double>::quiet_NaN()), Double(1.0)};
  ASSERT_EQ(Execute(FLOAT_ENTRY, execution, nan, result), ExecutionStatus::Returned);
  const auto value = std::bit_cast<double>(result.payload);
  EXPECT_NE(value, value);
}

TEST(NativeHostTest, BooleanAndNullableResultsUseExplicitShapes) {
  Isolate isolate;
  NativeExecution execution(isolate);
  const std::array boolean{NativeValue{.payload = 0, .kind = NativeValueKind::Boolean}};
  NativeValue result;
  ASSERT_EQ(Execute(BOOL_ENTRY, execution, boolean, result), ExecutionStatus::Returned);
  EXPECT_EQ(result.kind, NativeValueKind::Boolean);
  EXPECT_EQ(result.payload, 1U);
  const std::array null{NativeValue{}};
  ASSERT_EQ(Execute(NULLABLE_ENTRY, execution, null, result), ExecutionStatus::Returned);
  EXPECT_EQ(result.kind, NativeValueKind::Null);
  const std::array integer{Integer(std::numeric_limits<std::int64_t>::min())};
  ASSERT_EQ(Execute(NULLABLE_ENTRY, execution, integer, result), ExecutionStatus::Returned);
  EXPECT_EQ(result.payload, integer.front().payload);
  ASSERT_EQ(Execute(NULL_TEST_ENTRY, execution, null, result), ExecutionStatus::Returned);
  EXPECT_EQ(result.payload, 1U);
  ASSERT_EQ(Execute(NULL_TEST_ENTRY, execution, integer, result), ExecutionStatus::Returned);
  EXPECT_EQ(result.payload, 0U);
}

TEST(NativeHostTest, ExportBoundariesRejectWrongCountsAndKinds) {
  Isolate isolate;
  NativeExecution execution(isolate);
  NativeValue result;
  EXPECT_EQ(Execute(INTEGER_ENTRY, execution, {}, result), ExecutionStatus::InvalidEntry);
  const std::array wrong{Double(1.0)};
  EXPECT_EQ(Execute(BOOL_ENTRY, execution, wrong, result), ExecutionStatus::TypeError);
  const std::array malformed{NativeValue{.payload = 2, .kind = NativeValueKind::Boolean}};
  EXPECT_EQ(Execute(BOOL_ENTRY, execution, malformed, result), ExecutionStatus::TypeError);
  EXPECT_EQ(execution.frame_count(), 0U);
}
'''


class NativeObjectTest(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="vela-aot-原生 ")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.executable = Path(os.environ.get(
            "VELA_TEST_EXECUTABLE", ROOT / "out" / "debug" / executable_name("vela")))
        self.output = self.executable.parent
        self.tools = toolchain_tools()

    def command(self, arguments: list[str | Path], expected: int = 0) -> subprocess.CompletedProcess:
        result = subprocess.run(list(map(str, arguments)), capture_output=True,
                                text=True, encoding="utf-8", errors="replace",
                                cwd=self.directory, timeout=120, check=False)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        return result

    def emit(self, name: str, source: str) -> tuple[Path, str]:
        entry = self.directory / (name + ".vela")
        entry.write_text(source, encoding="utf-8", newline="\n")
        object_file = self.directory / (name + (".obj" if sys.platform == "win32" else ".o"))
        self.command([self.executable, "build", entry, "--emit=obj", "--export=calculate", "-o", object_file])
        nm = self.tools["cxx"].parent / executable_name("llvm-nm")
        symbols = self.command([nm, "--defined-only", "--extern-only", object_file]).stdout
        entries = re.findall(r"\b(vela_aot_[a-f0-9]+_entry_[0-9]+)\b", symbols)
        self.assertEqual(len(entries), 1, symbols)
        entry.unlink()
        return object_file, entries[0]

    def test_statically_linked_optimized_native_contracts_without_vm(self) -> None:
        fixtures = {
            "INTEGER_ENTRY": """
func twice(value: int): int { return value * 2; }
func touch(): void {}
func depth(value: int): int { if (value == 0) { return 0; } return depth(value - 1) + 1; }
export func calculate(operation: int, left: int, right: int): int {
  touch();
  if (operation == 0) { return left * right; }
  if (operation == 1) { return left / right; }
  if (operation == 2) { let remainder = left; remainder %= right; return remainder; }
  if (operation == 3) { return left << right; }
  if (operation == 4) { return left >> right; }
  if (operation == 5) { return left >>> right; }
  if (operation == 7) { return depth(left); }
  let sum = 0;
  for (let index = 0; index < 10; index++) { sum += twice(index); }
  return sum;
}
""",
            "FLOAT_ENTRY": "export func calculate(left: double, right: double): double { return left / right; }",
            "BOOL_ENTRY": "export func calculate(value: bool): bool { return !value; }",
            "NULLABLE_ENTRY": "export func calculate(value: int?): int? { return value; }",
            "NULL_TEST_ENTRY": "export func calculate(value: int?): bool { return value == null; }",
        }
        host = HOST_TESTS
        objects = []
        for name, source in fixtures.items():
            object_file, symbol = self.emit(name.lower(), source)
            objects.append(object_file)
            host = host.replace(name, symbol)
        source_file = self.directory / "host.cc"
        source_file.write_text(host, encoding="utf-8", newline="\n")
        executable = self.directory / executable_name("host")
        archive_suffix = ".lib" if sys.platform == "win32" else ".a"
        runtime = self.output / "obj/runtime" / ("vela_runtime" + archive_suffix)
        gtest = [self.output / "obj/third_party" / (name + archive_suffix)
                 for name in ("gtest_main", "gtest")]
        includes = [ROOT, ROOT / "third_party/googletest/googletest/include", ROOT / "third_party/abseil-cpp"]
        if sys.platform == "win32":
            flags = ["/nologo", "/MT", "/O2", "/clang:-std=c++23", "/D_ITERATOR_DEBUG_LEVEL=0",
                     "/D_HAS_EXCEPTIONS=0", "/c",
                     f"/Fo{self.directory / 'host.obj'}", *[f"/I{path}" for path in includes]]
            self.command([self.tools["cxx"], *flags, source_file])
            self.command([self.tools["cxx"], "/nologo", "/MT", "-fuse-ld=lld", f"/Fe{executable}",
                          self.directory / "host.obj", *objects, runtime, *gtest])
        else:
            flags = ["-std=c++23", "-O2", "-stdlib=libc++", "-fno-exceptions", "-pthread",
                     "-o", str(executable), *[f"-I{path}" for path in includes]]
            if sys.platform == "darwin":
                flags.extend(["-isysroot", str(self.tools["sysroot"])])
            self.command([self.tools["cxx"], *flags, source_file, *objects, runtime, *gtest])
        executed = self.command([executable])
        self.assertIn("[  PASSED  ] 5 tests.", executed.stdout)
        nm = self.tools["cxx"].parent / executable_name("llvm-nm")
        symbols = self.command([nm, "--defined-only", "--demangle", runtime]).stdout
        self.assertNotIn("vela::vm::", symbols)
        self.assertNotIn("vela::compiler::", symbols)
        self.assertNotIn("llvm::", symbols)

    def test_failed_compilation_preserves_existing_output(self) -> None:
        entry = self.directory / "invalid.vela"
        entry.write_text("export func calculate(): int { return true; }", encoding="utf-8")
        output = self.directory / "existing.obj"
        output.write_bytes(b"existing valid artifact")
        self.command([self.executable, "build", entry, "--emit=obj", "--export=calculate", "-o", output], 65)
        self.assertEqual(output.read_bytes(), b"existing valid artifact")


if __name__ == "__main__":
    unittest.main()
