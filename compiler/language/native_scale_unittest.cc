#include <cstddef>
#include <string>

#include "gtest/gtest.h"

#include "compiler/test_support/preparation_test_support.h"

#include "compiler/test_support/program_test_support.h"
#include "runtime/isolate/isolate.h"
#include "runtime/strings/string.h"

namespace vela::compiler {
namespace {

constexpr std::size_t kDeclarationCount = 300;
constexpr std::size_t kLongFunctionCalls = 1000;
const std::string kLastFunctionName = "f" + std::to_string(kDeclarationCount - 1);

std::string FunctionSource(std::size_t count) {
  std::string source;
  for (std::size_t index = 0; index < count; ++index) {
    source +=
        "func f" + std::to_string(index) + "(): int { return " + std::to_string(index) + "; }\n";
  }
  return source;
}

TEST(NativeScaleTest, CallsLastFunctionInLargeModule) {
  runtime::Isolate isolate;
  const auto source = FunctionSource(kDeclarationCount) + "func main(): int { return " +
                      kLastFunctionName + "(); }";
  const auto result = test::RunInteger(source, isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, kDeclarationCount - 1);
}

TEST(NativeScaleTest, AllocatesClassesInLargeModule) {
  std::string source;
  for (std::size_t index = 0; index < kDeclarationCount; ++index) {
    source += "class C" + std::to_string(index) + " {}\n";
  }
  const auto last = "C" + std::to_string(kDeclarationCount - 1);
  source += "func main(): " + last + " { return " + last + "(); }";
  runtime::Isolate isolate;
  const auto entry = test::CompileCleanSource(source, isolate);
  ASSERT_TRUE(entry) << entry.error();
  const auto result = test::Execute(**entry);
  ASSERT_TRUE(result.Succeeded());
  EXPECT_TRUE(result.value.IsObject());
}

TEST(NativeScaleTest, LoadsStringsFromLargeConstantPool) {
  std::string source;
  for (std::size_t index = 0; index < kDeclarationCount; ++index) {
    source += "func f" + std::to_string(index) + "(): string { return \"s" + std::to_string(index) +
              "\"; }\n";
  }
  runtime::Isolate isolate;
  const auto program = test::CompileCleanSource(source, isolate);
  ASSERT_TRUE(program) << program.error();
  const auto result = test::Execute(**program, kLastFunctionName);
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsString()->value(), "s" + std::to_string(kDeclarationCount - 1));
}

TEST(NativeScaleTest, CompilesLargeFloatingPointConstantPool) {
  std::string source = "func main(): int {\n";
  for (std::size_t index = 0; index < kDeclarationCount; ++index) {
    source += "print(" + std::to_string(index) + ".5);\n";
  }
  source += "return 0; }";
  runtime::Isolate isolate;
  const auto compiled = test::CompileSource(source, isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto prepared = ir::test::Prepare(source);
  ASSERT_TRUE(prepared.Succeeded());
  EXPECT_EQ(prepared.program.functions.front().doubles.size(), kDeclarationCount);
}

TEST(NativeScaleTest, AllocatesManyDistinctArrayLayouts) {
  std::string source;
  for (std::size_t index = 0; index < kDeclarationCount; ++index) {
    const auto type = "Element" + std::to_string(index);
    source += "class " + type + " {}\n";
    source += "func f" + std::to_string(index) + "(): " + type + "[] { return []; }\n";
  }
  runtime::Isolate isolate;
  const auto program = test::CompileCleanSource(source, isolate);
  ASSERT_TRUE(program) << program.error();
  const auto result = test::Execute(**program, kLastFunctionName);
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
      source += ',';
    }
    source += "f" + std::to_string(index) + ": " + std::to_string(index);
  }
  return source + "); return record.f" + std::to_string(count - 1) + "; }";
}

TEST(NativeScaleTest, AddressesFieldsInLargeObjects) {
  runtime::Isolate isolate;
  const auto result = test::RunInteger(FieldsSource(kDeclarationCount), isolate);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, kDeclarationCount - 1);
}

TEST(NativeScaleTest, ReusesDeadTemporariesInLongFunctions) {
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
  const auto result = test::Execute(*compiled.instance);
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsInteger(), 500500);
}

TEST(NativeScaleTest, KeepsManyManagedValuesLive) {
  std::string source = "class Box { value: int; } func main(): int {\n";
  for (std::size_t index = 0; index < kDeclarationCount; ++index) {
    source += "const b" + std::to_string(index) + "=Box(value: " + std::to_string(index) + ");\n";
  }
  source += "return ";
  for (std::size_t index = 0; index < kDeclarationCount; ++index) {
    if (index != 0) {
      source += '+';
    }
    source += "b" + std::to_string(index) + ".value";
  }
  source += "; }";
  runtime::Isolate isolate;
  const auto compiled = test::CompileSource(source, isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = test::ExecuteInteger(*compiled.instance);
  ASSERT_TRUE(result) << result.error();
  EXPECT_EQ(*result, (kDeclarationCount * (kDeclarationCount - 1)) / 2);
}

std::string ParametersSource(std::size_t count) {
  std::string source = "func pick(";
  for (std::size_t index = 0; index < count; ++index) {
    if (index != 0) {
      source += ',';
    }
    source += "p" + std::to_string(index) + ": int";
  }
  source +=
      "): int { return p" + std::to_string(count - 1) + "; }\nfunc main(): int { return pick(";
  for (std::size_t index = 0; index < count; ++index) {
    if (index != 0) {
      source += ',';
    }
    source += std::to_string(index);
  }
  return source + "); }";
}

TEST(NativeScaleTest, AcceptsLargeParameterLists) {
  runtime::Isolate isolate;
  for (const auto count : {kDeclarationCount - 1, kDeclarationCount}) {
    const auto result = test::RunInteger(ParametersSource(count), isolate);
    ASSERT_TRUE(result) << result.error();
    EXPECT_EQ(*result, count - 1);
  }
}

}  // namespace
}  // namespace vela::compiler
