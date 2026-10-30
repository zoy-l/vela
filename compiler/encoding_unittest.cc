#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "gtest/gtest.h"

#include "base/execution_limits.h"

#include "compiler/compilation_state.h"
#include "compiler/compiler.h"
#include "compiler/frontend/source_loader.h"
#include "compiler/frontend/type_checker.h"
#include "runtime/isolate.h"
#include "runtime/string.h"
#include "vm/program.h"
#include "vm/vm.h"

namespace vela::compiler {
namespace {

constexpr std::size_t kBeyondByteIndex = kMaxFrameRegisters + 1;
constexpr std::size_t kLongBranchStatements = 17000;
constexpr std::size_t kLongFunctionCalls = 1000;

std::string FunctionSource(std::size_t count) {
  std::string source;
  for (std::size_t index = 0; index < count; ++index) {
    source +=
        "func f" + std::to_string(index) + "(): int { return " + std::to_string(index) + "; }\n";
  }
  return source;
}

TEST(EncodingTest, CallsFunctionIndex256WithoutTruncation) {
  runtime::Isolate isolate;
  const auto source = FunctionSource(kBeyondByteIndex) + "func main(): int { return f256(); }";
  const auto compiled = CompileSource(source, isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsInteger(), 256);
}

TEST(EncodingTest, AllowsFunctionIndex255AndAnEntryBeyondTheByteIndexRange) {
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

TEST(EncodingTest, AllocatesClassIndex256) {
  std::string source;
  for (std::size_t index = 0; index < kBeyondByteIndex; ++index) {
    source += "class C" + std::to_string(index) + " {}\n";
  }
  source += "func main(): C256 { return C256(); }";
  runtime::Isolate isolate;
  const auto compiled = CompileSource(source, isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  ASSERT_TRUE(result.Succeeded());
  EXPECT_TRUE(result.value.IsObject());
}

TEST(EncodingTest, LoadsStringConstantIndex256WithoutTruncation) {
  std::string source;
  for (std::size_t index = 0; index < kBeyondByteIndex; ++index) {
    source += "func f" + std::to_string(index) + "(): string { return \"s" + std::to_string(index) +
              "\"; }\n";
  }
  runtime::Isolate isolate;
  const auto compiled = CompileSource(source, isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("f256"));
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsString()->value(), "s256");
}

TEST(EncodingTest, AcceptsDoubleConstantIndex256) {
  std::string source = "func main(): int {\n";
  for (std::size_t index = 0; index < kBeyondByteIndex; ++index) {
    source += "print(" + std::to_string(index) + ".5);\n";
  }
  source += "return 0; }";
  runtime::Isolate isolate;
  const auto compiled = CompileSource(source, isolate);
  ASSERT_TRUE(compiled.Succeeded());
  EXPECT_EQ(compiled.program.functions().front().doubles.size(), kBeyondByteIndex);
}

TEST(EncodingTest, AllocatesArrayLayoutIndex256) {
  std::string source;
  for (std::size_t index = 0; index < kBeyondByteIndex; ++index) {
    const auto type = "Element" + std::to_string(index);
    source += "class " + type + " {}\n";
    source += "func f" + std::to_string(index) + "(): " + type + "[] { return []; }\n";
  }
  runtime::Isolate isolate;
  const auto compiled = CompileSource(source, isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("f256"));
  ASSERT_TRUE(result.Succeeded());
  EXPECT_TRUE(result.value.IsObject());
}

CompileResult CompileBranchOverflow(CompilationState& state, runtime::Isolate& isolate) {
  std::string source =
      "class Element {} func main(): void { const value = Element(); while (true) {";
  for (std::size_t index = 0; index < kLongBranchStatements; ++index) {
    source += "print(1);";
  }
  source += "} }";
  const frontend::SourceInput input{.path = "main.vela", .text = source};
  auto parsed = frontend::LoadSources(std::span(&input, 1), input.path);
  if (!parsed.diagnostics.empty()) {
    return {.program = {}, .diagnostics = std::move(parsed.diagnostics)};
  }
  auto checked = frontend::TypeChecker().Check(parsed.program);
  if (!checked.Succeeded()) {
    return {.program = {}, .diagnostics = std::move(checked.diagnostics)};
  }
  return CompileProgram(parsed.program, checked.model, isolate, &state);
}

TEST(EncodingTest, FailedEncodingDoesNotPublishSessionLayouts) {
  runtime::Isolate isolate;
  CompilationState state;
  const auto before = isolate.layout_counts();
  const auto compiled = CompileBranchOverflow(state, isolate);
  ASSERT_FALSE(compiled.Succeeded());
  EXPECT_TRUE(compiled.diagnostics.front().message.contains("bytecode range"));
  EXPECT_EQ(isolate.layout_counts(), before);
  EXPECT_TRUE(state.layouts.classes.empty());
  EXPECT_TRUE(state.layouts.arrays.empty());
  EXPECT_TRUE(state.layouts.signatures.empty());
}

std::string FieldsSource(std::size_t count) {
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

TEST(EncodingTest, ReusesRegistersAndAddressesFieldsBeyondByteIndexRange) {
  runtime::Isolate isolate;
  const auto valid = CompileSource(FieldsSource(kMaxFrameRegisters), isolate);
  ASSERT_TRUE(valid.Succeeded());
  const auto main = valid.program.FindFunction("main");
  ASSERT_TRUE(main.IsValid());
  EXPECT_LE(main.register_count(), 3U);
  const auto result = vm::VirtualMachine(isolate).Execute(main);
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsInteger(), 255);
  const auto wide = CompileSource(FieldsSource(kBeyondByteIndex), isolate);
  ASSERT_TRUE(wide.Succeeded());
  const auto wide_result = vm::VirtualMachine(isolate).Execute(wide.program.FindFunction("main"));
  ASSERT_TRUE(wide_result.Succeeded());
  EXPECT_EQ(wide_result.value.AsInteger(), 256);
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
  EXPECT_TRUE(CompileSource("func main(): int { return 32768; }", isolate).Succeeded());
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
      "class Counter { value: int; next(): int { this.value=this.value+1; return this.value; "
      "} "
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

std::string ParametersSource(std::size_t count) {
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
