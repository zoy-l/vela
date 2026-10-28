#include <string>
#include <string_view>

#include "gtest/gtest.h"

#include "compiler/compiler.h"
#include "runtime/isolate.h"
#include "vm/vm.h"

namespace vela::compiler {
namespace {

TEST(InterfaceTest, UnusedInterfacesDoNotExhaustCallSelectorEncoding) {
  constexpr int kUnusedInterfaces = 300;
  std::string source;
  for (int index = 0; index < kUnusedInterfaces; ++index) {
    source += "interface Ignored" + std::to_string(index) + " { read(): int; }\n";
  }
  source += R"(
interface Used { read(): int; }
class Answer implements Used { read(): int { return 42; } }
func main(): int { const answer: Used = Answer(); return answer.read(); }
)";
  runtime::Isolate isolate;
  const auto compiled = CompileSource(source, isolate);
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  ASSERT_TRUE(result.Succeeded());
  EXPECT_EQ(result.value.AsInteger(), 42);
}

TEST(InterfaceTest, DispatchesConcreteImplementationsAndBindsMethodValues) {
  runtime::Isolate isolate(1);
  const auto compiled = CompileSource(R"(
interface Measurable { measure(): int; }
class Rectangle implements Measurable {
  width: int;
  height: int;
  measure(): int { return this.width * this.height; }
}
class Constant implements Measurable {
  measure(): int { return 2; }
}
func area(value: Measurable): int { return value.measure(); }
func main(): int {
  const rectangle = Rectangle(width: 5, height: 8);
  const value: Measurable = rectangle;
  const items: Measurable[] = [value, Constant()];
  const bound: () => int = items[1].measure;
  return area(items[0]) + bound();
}
)",
                                      isolate);
  for (const auto& diagnostic : compiled.diagnostics) {
    ADD_FAILURE() << diagnostic.location.line << ": " << diagnostic.message;
  }
  ASSERT_TRUE(compiled.Succeeded());
  const auto result = vm::VirtualMachine(isolate).Execute(compiled.program.FindFunction("main"));
  ASSERT_TRUE(result.Succeeded()) << static_cast<int>(result.status);
  EXPECT_EQ(result.value.AsInteger(), 42);
}

class InterfaceErrorTest : public testing::TestWithParam<std::string_view> {};

TEST_P(InterfaceErrorTest, RejectsMissingOrIncompatibleExplicitImplementations) {
  runtime::Isolate isolate;
  EXPECT_FALSE(CompileSource(GetParam(), isolate).Succeeded());
}

INSTANTIATE_TEST_SUITE_P(
    Contracts,
    InterfaceErrorTest,
    testing::Values(
        "interface I { read(): int; } class A implements I {}",
        "interface I { read(): int; } class A implements I { read(): double { return 1.0; } }",
        "interface I { read(): int; } class A implements I { private read(): int { return 1; } }",
        "interface I { read(): int; } class A implements I { static read(): int { return 1; } }",
        "interface I {} class A {} func main(): I { return A(); }",
        "class A {} class B implements A {}",
        "interface I { read(): int; } func main(value: I?): int { return value.read(); }"));

}  // namespace
}  // namespace vela::compiler
