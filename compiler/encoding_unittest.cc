#include <cstddef>
#include <string>
#include <string_view>

#include "gtest/gtest.h"

#include "base/execution_limits.h"

#include "compiler/compiler.h"
#include "runtime/isolate.h"
#include "vm/program.h"
#include "vm/vm.h"

namespace vela::compiler {
namespace {

constexpr std::size_t kBeyondByteIndex = kMaxFrameRegisters + 1;
constexpr std::size_t kLongBranchStatements = 17000;
constexpr std::size_t kLongFunctionCalls = 1000;

auto FunctionSource(std::size_t count) -> std::string {
  std::string source;
  for (std::size_t index = 0; index < count; ++index) {
    source +=
        "func f" + std::to_string(index) + "(): int { return " + std::to_string(index) + "; }\n";
  }
  return source;
}

TEST(EncodingTest, RejectsFunctionIndex256WithoutTruncation) {
  runtime::Isolate isolate;
  const auto source = FunctionSource(kBeyondByteIndex) + "func main(): int { return f256(); }";
  const auto compiled = CompileSource(source, isolate);
  ASSERT_FALSE(compiled.Succeeded());
  EXPECT_TRUE(compiled.diagnostics.front().message.contains("encoding"));
}

TEST(EncodingTest, AllowsFunctionIndex255AndAnEntryBeyondTheCallOperandLimit) {
  runtime::Isolate isolate;
  const auto compiled = CompileSource(
      FunctionSource(kMaxFrameRegisters) + "func main(): int { return f255(); }", isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto main = compiled.program.FindFunction("main");
  ASSERT_TRUE(main.IsValid());
  EXPECT_EQ(main.index(), kMaxFrameRegisters);
  const auto result = vm::VirtualMachine(isolate).Execute(main);
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsInteger(), 255);
}

TEST(EncodingTest, RejectsClassIndex256) {
  std::string source;
  for (std::size_t index = 0; index < kBeyondByteIndex; ++index) {
    source += "class C" + std::to_string(index) + " {}\n";
  }
  source += "func main(): C256 { return C256(); }";
  runtime::Isolate isolate;
  const auto compiled = CompileSource(source, isolate);
  ASSERT_FALSE(compiled.Succeeded());
  EXPECT_TRUE(compiled.diagnostics.front().message.contains("encoding"));
}

TEST(EncodingTest, RejectsStringConstantIndex256) {
  std::string source;
  for (std::size_t index = 0; index < kBeyondByteIndex; ++index) {
    source += "func f" + std::to_string(index) + "(): string { return \"s" + std::to_string(index) +
              "\"; }\n";
  }
  runtime::Isolate isolate;
  const auto compiled = CompileSource(source, isolate);
  ASSERT_FALSE(compiled.Succeeded());
  EXPECT_TRUE(compiled.diagnostics.front().message.contains("encoding"));
}

TEST(EncodingTest, RejectsDoubleConstantIndex256) {
  std::string source = "func main(): int {\n";
  for (std::size_t index = 0; index < kBeyondByteIndex; ++index) {
    source += "print(" + std::to_string(index) + ".5);\n";
  }
  source += "return 0; }";
  runtime::Isolate isolate;
  const auto compiled = CompileSource(source, isolate);
  ASSERT_FALSE(compiled.Succeeded());
  EXPECT_TRUE(compiled.diagnostics.front().message.contains("encoding"));
}

TEST(EncodingTest, RejectsArrayLayoutIndex256) {
  std::string source;
  std::string type = "int";
  for (std::size_t index = 0; index < kBeyondByteIndex; ++index) {
    type += "[]";
    source += "func f" + std::to_string(index) + "(): " + type + " { return []; }\n";
  }
  runtime::Isolate isolate;
  const auto compiled = CompileSource(source, isolate);
  ASSERT_FALSE(compiled.Succeeded());
  EXPECT_TRUE(compiled.diagnostics.front().message.contains("encoding"));
}

auto FieldsSource(std::size_t count) -> std::string {
  std::string source = "class Record {\n";
  for (std::size_t index = 0; index < count; ++index) {
    source += "f" + std::to_string(index) + ": int;\n";
  }
  source += "}\nfunc main(): int { const record = Record(";
  for (std::size_t index = 0; index < count; ++index) {
    if (index != 0) {
      source += ",";
    }
    source += "f" + std::to_string(index) + ": " + std::to_string(index);
  }
  return source + "); return record.f" + std::to_string(count - 1) + "; }";
}

TEST(EncodingTest, ReusesRegistersFor256FieldsAndRejectsFieldIndex256) {
  runtime::Isolate isolate;
  const auto valid = CompileSource(FieldsSource(kMaxFrameRegisters), isolate);
  ASSERT_TRUE(valid.Succeeded());
  const auto main = valid.program.FindFunction("main");
  ASSERT_TRUE(main.IsValid());
  EXPECT_LE(main.register_count(), 3U);
  const auto result = vm::VirtualMachine(isolate).Execute(main);
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsInteger(), 255);
  const auto invalid = CompileSource(FieldsSource(kBeyondByteIndex), isolate);
  ASSERT_FALSE(invalid.Succeeded());
  EXPECT_TRUE(invalid.diagnostics.front().message.contains("encoding"));
}

TEST(EncodingTest, PreservesArithmeticResultsOutsideTheImmediateRange) {
  runtime::Isolate isolate;
  const auto compiled = CompileSource("func main(): int { return 20000 + 20000; }", isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto main = compiled.program.FindFunction("main");
  ASSERT_TRUE(main.IsValid());
  const auto result = vm::VirtualMachine(isolate).Execute(main);
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsInteger(), 40000);
  EXPECT_FALSE(CompileSource("func main(): int { return 32768; }", isolate).Succeeded());
}

TEST(EncodingTest, DiagnosesJumpOffsetsOutsideInt16) {
  std::string source = "func work(flag: bool): void { if (flag) {\n";
  for (std::size_t index = 0; index < kLongBranchStatements; ++index) {
    source += "print(1);\n";
  }
  source += "} }";
  runtime::Isolate isolate;
  const auto compiled = CompileSource(source, isolate);
  ASSERT_FALSE(compiled.Succeeded());
  EXPECT_TRUE(compiled.diagnostics.front().message.contains("jump target"));
}

TEST(EncodingTest, ReusesDeadTemporariesInLongFunctions) {
  std::string source =
      "class Counter { value: int; next(): int { this.value=this.value+1; return this.value; } "
      "}\nfunc main(): int { const counter=Counter(value: 0); let total=0;\n";
  for (std::size_t index = 0; index < kLongFunctionCalls; ++index) {
    source += "total=total+counter.next();\n";
  }
  source += "return total; }";
  runtime::Isolate isolate(1);
  const auto compiled = CompileSource(source, isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto main = compiled.program.FindFunction("main");
  ASSERT_TRUE(main.IsValid());
  EXPECT_LE(main.register_count(), 8U);
  const auto result = vm::VirtualMachine(isolate).Execute(main);
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsInteger(), 500500);
}

TEST(EncodingTest, ReportsLiveRegisterPressureInsteadOfTotalExpressionCount) {
  std::string source = "class Box { value: int; } func main(): int {\n";
  for (std::size_t index = 0; index < kBeyondByteIndex; ++index) {
    source += "const b" + std::to_string(index) + "=Box(value: " + std::to_string(index) + ");\n";
  }
  source += "return ";
  for (std::size_t index = 0; index < kBeyondByteIndex; ++index) {
    if (index != 0) {
      source += "+";
    }
    source += "b" + std::to_string(index) + ".value";
  }
  source += "; }";
  runtime::Isolate isolate;
  const auto compiled = CompileSource(source, isolate);
  ASSERT_FALSE(compiled.Succeeded());
  EXPECT_TRUE(compiled.diagnostics.front().message.contains("register slots"));
}

auto ParametersSource(std::size_t count) -> std::string {
  std::string source = "func pick(";
  for (std::size_t index = 0; index < count; ++index) {
    if (index != 0) {
      source += ",";
    }
    source += "p" + std::to_string(index) + ": int";
  }
  source +=
      "): int { return p" + std::to_string(count - 1) + "; }\nfunc main(): int { return pick(";
  for (std::size_t index = 0; index < count; ++index) {
    if (index != 0) {
      source += ",";
    }
    source += std::to_string(index);
  }
  return source + "); }";
}

TEST(EncodingTest, Supports256ParameterSlotsAndRejects257) {
  runtime::Isolate isolate;
  const auto valid = CompileSource(ParametersSource(kMaxFrameRegisters), isolate);
  for (const auto& diagnostic : valid.diagnostics) {
    ADD_FAILURE() << diagnostic.message;
  }
  ASSERT_TRUE(valid.Succeeded());
  const auto main = valid.program.FindFunction("main");
  ASSERT_TRUE(main.IsValid());
  const auto result = vm::VirtualMachine(isolate).Execute(main);
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsInteger(), 255);
  const auto invalid = CompileSource(ParametersSource(kBeyondByteIndex), isolate);
  ASSERT_FALSE(invalid.Succeeded());
  EXPECT_TRUE(invalid.diagnostics.front().message.contains("parameter count"));
}

}  // namespace
}  // namespace vela::compiler
