#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "gtest/gtest.h"

#include "compiler/ir/preparation_test_support.h"

#include "compiler/frontend/modules/source_loader.h"
#include "compiler/frontend/typing/type_checker.h"
#include "compiler/test_support/program_test_support.h"
#include "runtime/isolate/isolate.h"
#include "runtime/strings/string.h"

namespace vela::compiler {
namespace {

constexpr std::size_t kWideCount = 257;
constexpr std::size_t kLongBranchStatements = 17000;
constexpr std::size_t kLongFunctionCalls = 1000;
constexpr std::string_view kWideFunctionName = "f256";

std::string FunctionSource(std::size_t count) {
  std::string source;
  for (std::size_t index = 0; index < count; ++index) {
    source +=
        "func f" + std::to_string(index) + "(): int { return " + std::to_string(index) + "; }\n";
  }
  return source;
}

TEST(NativeLimitsTest, CallsFunctionIndex256WithoutTruncation) {
  runtime::Isolate isolate;
  const auto source = FunctionSource(kWideCount) + "func main(): int { return f256(); }";
  const auto result = test::RunInteger(source, isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 256);
}

TEST(NativeLimitsTest, AllowsFunctionIndex255AndAnEntryBeyondTheByteIndexRange) {
  runtime::Isolate isolate;
  const auto compiled = test::CompileSource(
      FunctionSource((kWideCount - 1)) + "func main(): int { return f255(); }", isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto main = compiled.instance->FindFunction("main");
  ASSERT_TRUE(main);
  EXPECT_EQ(*main, (kWideCount - 1));
  const auto result = test::Execute(*compiled.instance);
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsInteger(), 255);
}

TEST(NativeLimitsTest, AllocatesClassIndex256) {
  std::string source;
  for (std::size_t index = 0; index < kWideCount; ++index) {
    source += "class C" + std::to_string(index) + " {}\n";
  }
  source += "func main(): C256 { return C256(); }";
  runtime::Isolate isolate;
  const auto entry = test::CompileCleanSource(source, isolate);
  ASSERT_TRUE(entry) << entry.error();
  const auto result = test::Execute(**entry);
  ASSERT_TRUE(result.Succeeded());
  EXPECT_TRUE(result.value.IsObject());
}

TEST(NativeLimitsTest, LoadsStringConstantIndex256WithoutTruncation) {
  std::string source;
  for (std::size_t index = 0; index < kWideCount; ++index) {
    source += "func f" + std::to_string(index) + "(): string { return \"s" + std::to_string(index) +
              "\"; }\n";
  }
  runtime::Isolate isolate;
  const auto program = test::CompileCleanSource(source, isolate);
  ASSERT_TRUE(program) << program.error();
  const auto result = test::Execute(**program, kWideFunctionName);
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsString()->value(), "s256");
}

TEST(NativeLimitsTest, AcceptsDoubleConstantIndex256) {
  std::string source = "func main(): int {\n";
  for (std::size_t index = 0; index < kWideCount; ++index) {
    source += "print(" + std::to_string(index) + ".5);\n";
  }
  source += "return 0; }";
  runtime::Isolate isolate;
  const auto compiled = test::CompileSource(source, isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto prepared = ir::test::Prepare(source);
  ASSERT_TRUE(prepared.Succeeded());
  EXPECT_EQ(prepared.program.functions.front().doubles.size(), kWideCount);
}

TEST(NativeLimitsTest, AllocatesArrayLayoutIndex256) {
  std::string source;
  for (std::size_t index = 0; index < kWideCount; ++index) {
    const auto type = "Element" + std::to_string(index);
    source += "class " + type + " {}\n";
    source += "func f" + std::to_string(index) + "(): " + type + "[] { return []; }\n";
  }
  runtime::Isolate isolate;
  const auto program = test::CompileCleanSource(source, isolate);
  ASSERT_TRUE(program) << program.error();
  const auto result = test::Execute(**program, kWideFunctionName);
  ASSERT_TRUE(result.Succeeded());
  EXPECT_TRUE(result.value.IsObject());
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

TEST(NativeLimitsTest, AddressesFieldsBeyondByteIndexRange) {
  runtime::Isolate isolate;
  const auto valid = test::CompileSource(FieldsSource((kWideCount - 1)), isolate);
  ASSERT_TRUE(valid.Succeeded());
  const auto main = valid.instance->FindFunction("main");
  ASSERT_TRUE(main);
  const auto result = test::Execute(*valid.instance);
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsInteger(), 255);
  const auto wide = test::CompileSource(FieldsSource(kWideCount), isolate);
  ASSERT_TRUE(wide.Succeeded());
  const auto wide_result = test::Execute(*wide.instance);
  ASSERT_TRUE(wide_result.Succeeded());
  EXPECT_EQ(wide_result.value.AsInteger(), 256);
}

TEST(NativeLimitsTest, PreservesArithmeticResultsOutsideTheImmediateRange) {
  runtime::Isolate isolate;
  const auto result = test::RunInteger("func main(): int { return 20000 + 20000; }", isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, 40000);
  EXPECT_TRUE(test::CompileSource("func main(): int { return 32768; }", isolate).Succeeded());
}

TEST(NativeLimitsTest, CompilesBranchesBeyondSigned16BitInstructionOffsets) {
  std::string source = "func work(flag: bool): void { if (flag) {\n";
  for (std::size_t index = 0; index < kLongBranchStatements; ++index) {
    source += "print(1);\n";
  }
  source += "} }";
  runtime::Isolate isolate;
  const auto compiled = test::CompileSource(source, isolate);
  ASSERT_TRUE(compiled.Succeeded());
}

TEST(NativeLimitsTest, ReusesDeadTemporariesInLongFunctions) {
  std::string source =
      "class Counter { value: int; next(): int { this.value=this.value+1; return this.value; "
      "} "
      "}\nfunc main(): int { const counter=Counter(value: 0); let total=0;\n";
  for (std::size_t index = 0; index < kLongFunctionCalls; ++index) {
    source += "total=total+counter.next();\n";
  }
  source += "return total; }";
  runtime::Isolate isolate(1);
  const auto compiled = test::CompileSource(source, isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto main = compiled.instance->FindFunction("main");
  ASSERT_TRUE(main);
  const auto result = test::Execute(*compiled.instance);
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsInteger(), 500500);
}

TEST(NativeLimitsTest, SupportsMoreThan256SimultaneouslyLiveValues) {
  std::string source = "class Box { value: int; } func main(): int {\n";
  for (std::size_t index = 0; index < kWideCount; ++index) {
    source += "const b" + std::to_string(index) + "=Box(value: " + std::to_string(index) + ");\n";
  }
  source += "return ";
  for (std::size_t index = 0; index < kWideCount; ++index) {
    if (index != 0) {
      source += "+";
    }
    source += "b" + std::to_string(index) + ".value";
  }
  source += "; }";
  runtime::Isolate isolate;
  const auto compiled = test::CompileSource(source, isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = test::ExecuteInteger(*compiled.instance);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (kWideCount * (kWideCount - 1)) / 2);
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

TEST(NativeLimitsTest, SupportsMoreThan256Parameters) {
  runtime::Isolate isolate;
  for (const auto count : {kWideCount - 1, kWideCount}) {
    const auto result = test::RunInteger(ParametersSource(count), isolate);
    ASSERT_TRUE(result) << result.error();
    EXPECT_EQ(*result, count - 1);
  }
}

}  // namespace
}  // namespace vela::compiler
