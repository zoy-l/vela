#include <cstdint>
#include <string>
#include <string_view>

#include "gtest/gtest.h"

#include "compiler/compiler.h"
#include "runtime/heap.h"

namespace vela::compiler {
namespace {

constexpr std::int64_t kExpectedSum = 42;

using vela::runtime::Isolate;
using vela::runtime::RootRange;
using vela::runtime::Value;
using vela::vm::ExecutionStatus;
using vela::vm::VirtualMachine;

TEST(CompilerTest, CompilesIntegerFunction) {
  constexpr std::string_view source = R"(
int main() {
  int x = 20;
  int y = 22;
  return x + y;
}
)";

  Isolate isolate;
  const auto compiled = CompileSource(source, isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto* function = compiled.program.FindFunction("main");
  ASSERT_NE(function, nullptr);
  const auto result = VirtualMachine(isolate).Execute(function->View());
  ASSERT_EQ(result.status, ExecutionStatus::Returned);
  ASSERT_TRUE(result.value.IsInteger());
  EXPECT_EQ(result.value.AsInteger(), kExpectedSum);
}

TEST(CompilerTest, CompilesNestedClassConstruction) {
  constexpr std::string_view source = R"(
class Theme {
  int brightness;
}

class App {
  Theme theme;
}

App main() {
  return App(theme: Theme(brightness: 1));
}
)";

  Isolate isolate;
  const auto compiled = CompileSource(source, isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto* function = compiled.program.FindFunction("main");
  ASSERT_NE(function, nullptr);
  const auto result = VirtualMachine(isolate).Execute(function->View());
  ASSERT_EQ(result.status, ExecutionStatus::Returned);
  ASSERT_TRUE(result.value.IsObject());
  ASSERT_NE(result.value.AsObject(), nullptr);

  Value result_root = result.value;
  RootRange root(isolate.heap(), &result_root, 1);
  const Value* theme = vela::runtime::Heap::GetField(*result_root.AsObject(), 0);
  ASSERT_NE(theme, nullptr);
  ASSERT_TRUE(theme->IsObject());
  ASSERT_NE(theme->AsObject(), nullptr);
  const Value* brightness = vela::runtime::Heap::GetField(*theme->AsObject(), 0);
  ASSERT_NE(brightness, nullptr);
  ASSERT_TRUE(brightness->IsInteger());
  EXPECT_EQ(brightness->AsInteger(), 1);

  result_root = Value::Null();
  isolate.heap().Collect();
  EXPECT_EQ(isolate.heap().object_count(), 0U);
}

TEST(CompilerTest, CompilesFunctionCall) {
  constexpr std::string_view source = R"(
int add(int left, int right) {
  return left + right;
}

int main() {
  return add(20, 22);
}
)";

  Isolate isolate;
  const auto compiled = CompileSource(source, isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto* function = compiled.program.FindFunction("main");
  ASSERT_NE(function, nullptr);
  const auto entry = static_cast<std::uint32_t>(function - compiled.program.functions.data());
  const auto result = VirtualMachine(isolate).Execute(compiled.program.View(), entry);
  ASSERT_EQ(result.status, ExecutionStatus::Returned);
  ASSERT_TRUE(result.value.IsInteger());
  EXPECT_EQ(result.value.AsInteger(), kExpectedSum);
}

TEST(CompilerTest, CompilesLoopControlFlow) {
  constexpr std::int64_t kExpectedLoopSum = 10;
  constexpr std::string_view source = R"(
int main() {
  int i = 0;
  int sum = 0;
  while (i != 5) {
    sum = sum + i;
    i = i + 1;
  }
  return sum;
}

)";
  Isolate isolate;
  const auto compiled = CompileSource(source, isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto* function = compiled.program.FindFunction("main");
  ASSERT_NE(function, nullptr);
  const auto result = VirtualMachine(isolate).Execute(function->View());
  ASSERT_EQ(result.status, ExecutionStatus::Returned);
  ASSERT_TRUE(result.value.IsInteger());
  EXPECT_EQ(result.value.AsInteger(), kExpectedLoopSum);
}

TEST(CompilerTest, ExecutesLanguageFeaturesAndNamedMethodArguments) {
  constexpr std::string_view source = R"(
class Counter {
  int value;
  int add(int amount) {
    this.value = this.value + amount;
    return this.value;
  }
  int digits(int tens, int ones) {
    return tens * 10 + ones;
  }
}

bool main() {
  int sum = 0;
  for (int i = 0; i < 5; i = i + 1) {
    if (i == 2) continue;
    if (i == 4) break;
    sum = sum + i;
  }
  Counter counter = Counter(value: sum);
  string text = "a" + "b";
  int[] values = [1, 2, 3];
  return counter.add(values[1]) == 6 && text == "ab" &&
         counter.digits(ones: 2, tens: 3) == 32;
}
)";
  Isolate isolate;
  const auto compiled = CompileSource(source, isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto* function = compiled.program.FindFunction("main");
  ASSERT_NE(function, nullptr);
  const auto result = VirtualMachine(isolate).Execute(
      compiled.program.View(),
      static_cast<std::uint32_t>(function - compiled.program.functions.data()));
  ASSERT_EQ(result.status, ExecutionStatus::Returned);
  ASSERT_TRUE(result.value.IsBoolean());
  EXPECT_TRUE(result.value.AsBoolean());
}

TEST(CompilerTest, ExecutesLoopWithWideJumpTarget) {
  constexpr std::int64_t kUpdateCount = 70;
  std::string source = "int main() { int value = 0; int counter = 0; while (counter < 1) {";
  for (std::int64_t index = 0; index < kUpdateCount; ++index) {
    source += "value = value + 1;";
  }
  source += "counter = counter + 1; } return value; }";
  Isolate isolate;
  const auto compiled = CompileSource(source, isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto* function = compiled.program.FindFunction("main");
  ASSERT_NE(function, nullptr);
  const auto result = VirtualMachine(isolate).Execute(function->View());
  ASSERT_TRUE(result.Succeeded());
  ASSERT_TRUE(result.value.IsInteger());
  EXPECT_EQ(result.value.AsInteger(), kUpdateCount);
}

TEST(CompilerTest, PreservesSymbolsAndShadowingAcrossTableGrowth) {
  constexpr std::int64_t kSymbolCount = 64;
  constexpr std::int64_t kLocalCount = 32;
  constexpr std::int64_t kExpectedResult = (kLocalCount * 2) - 1;
  std::string source;
  for (std::int64_t index = 0; index < kSymbolCount; ++index) {
    const std::string suffix = std::to_string(index);
    source += "enum SymbolEnum" + suffix + " { first, second }\n";
    source += "class SymbolClass" + suffix + " { int value; int read() { return this.value; } }\n";
    source += "int symbol_function_" + suffix + "(int value) { return value; }\n";
  }
  source += "int main() { int sum = 0;\n";
  for (std::int64_t index = 0; index < kLocalCount; ++index) {
    const std::string suffix = std::to_string(index);
    source.append("int local_variable_with_a_long_name_")
        .append(suffix)
        .append(" = ")
        .append(suffix)
        .append(";\n");
  }
  source += "{\n";
  for (std::int64_t index = 0; index < kLocalCount; ++index) {
    source += "int local_variable_with_a_long_name_" + std::to_string(index) + " = " +
              std::to_string(index + 1) + ";\n";
  }
  const std::string last_local =
      "local_variable_with_a_long_name_" + std::to_string(kLocalCount - 1);
  source += "sum = " + last_local + "; } sum = sum + " + last_local + ";\n";
  const std::string last_class = "SymbolClass" + std::to_string(kSymbolCount - 1);
  source += last_class + " instance = " + last_class + "(value: sum);\n";
  source += "return symbol_function_0(instance.read()); }\n";

  Isolate isolate;
  const auto compiled = CompileSource(source, isolate);
  for (const auto& diagnostic : compiled.diagnostics) {
    ADD_FAILURE() << diagnostic.message;
  }
  ASSERT_TRUE(compiled.Succeeded());
  const auto* entry = compiled.program.FindFunction("main");
  ASSERT_NE(entry, nullptr);
  const auto entry_index = static_cast<std::uint32_t>(entry - compiled.program.functions.data());
  const auto result = VirtualMachine(isolate).Execute(compiled.program.View(), entry_index);
  ASSERT_TRUE(result.Succeeded());
  ASSERT_TRUE(result.value.IsInteger());
  EXPECT_EQ(result.value.AsInteger(), kExpectedResult);
}

}  // namespace
}  // namespace vela::compiler
