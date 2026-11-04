#include "compiler/ir/preparation.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

#include "base/builtin.h"
#include "base/layout_description.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/ir/ir.h"
#include "compiler/ir/lowerer.h"
#include "compiler/test_support/preparation_test_support.h"

namespace vela::ir {
namespace {

using test::Prepare;

TEST(PreparationTest, ScalarValueAwaitSkipsOnlyAnUnobservableFactory) {
  const auto prepared = Prepare(R"(
func keep(value: Future<int>): Future<int> { return value; }
async func integer(value: int): Future<int> { return await Future.value(value); }
async func decimal(value: double): Future<double> { return await Future.value(value); }
async func boolean(value: bool): Future<bool> { return await Future.value(value); }
async func nullable(value: int?): Future<int?> { return await Future.value(value); }
async func named(value: int): Future<int> {
  const ready = Future.value(value); return await ready;
}
async func multiple(value: int): Future<int> {
  const ready = Future.value(value); return await ready + await ready;
}
async func escaped(value: int): Future<int> { return await keep(Future.value(value)); }
async func adopted(value: Future<int>): Future<int> { return await Future.value(value); }
async func erased(value: dynamic): Future<int> { return await Future.value<int>(value); }
async func failed(value: Error): Future<int> { return await Future.error<int>(value); }
)");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  constexpr std::array<std::string_view, 6> kPreserved{
      "named", "multiple", "escaped", "adopted", "erased", "failed",
  };
  for (const auto& function : prepared.program.functions) {
    const auto factories =
        std::ranges::count(function.instructions, Opcode::NewFuture, &Instruction::opcode);
    EXPECT_EQ(factories, std::ranges::contains(kPreserved, function.name) ? 1 : 0) << function.name;
  }
}

TEST(PreparationTest, FusesOnlyUnobservedScalarTextUsedForItsLength) {
  const auto prepared = Prepare(R"(
func length(value: double): int { const text = value.toString(); return text.length; }
func bytes(value: int): int { return value.toString().byteLength; }
func observed(value: int): int { const text = value.toString(); print(text); return text.length; }
func reassigned(value: int): int { let text = value.toString(); text = 'kept'; return text.length; }
)");
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  for (const auto& function : prepared.program.functions) {
    const auto fused = std::ranges::count_if(function.instructions, [](const auto& instruction) {
      return instruction.opcode == Opcode::BuiltinCall &&
             instruction.index == static_cast<std::uint32_t>(Builtin::ScalarTextLength);
    });
    EXPECT_EQ(fused, function.name == "length" || function.name == "bytes" ? 1 : 0)
        << function.name;
  }
}

TEST(PreparationTest, PreservesUnboundNativeImportsWithoutRuntimeState) {
  auto result = Prepare(R"(
external func read(): int = "host.read";
func main(): int { return read() + 2; }
)");
  ASSERT_TRUE(result.Succeeded());
  const auto& functions = result.program.functions;
  const auto found = std::ranges::find(functions, "host.read", &Function::native_symbol);
  ASSERT_NE(found, functions.end());
  ASSERT_TRUE(found->signature.has_value());
  EXPECT_TRUE(found->instructions.empty());
  EXPECT_EQ(found->return_type.kind, frontend::TypeDescriptor::Kind::Int);
}

TEST(PreparationTest, RuntimeInterfaceLayoutsExcludeAbstractTemplateApplications) {
  const auto prepared = Prepare(R"(
interface Reader<T> { read(): T; }
class Value implements Reader<int> { read(): int { return 42; } }
class Operations {
  read<T extends Reader<int>>(value: T): int { return value.read(); }
}
func main(): int { return Operations().read(Value()); }
)");
  ASSERT_TRUE(prepared.Succeeded());
  const auto& interfaces = prepared.program.layouts.interfaces;
  ASSERT_EQ(interfaces.size(), 1U);
  EXPECT_EQ(interfaces.front(), "Reader<int>");
}

TEST(PreparationTest, KeepsWideVirtualRegistersBeforeBackendAllocation) {
  constexpr int kArrayLength = 300;
  std::string source = "func main(): int { const values = [";
  for (int index = 0; index < kArrayLength; ++index) {
    if (index != 0) {
      source += ',';
    }
    source += std::to_string(index);
  }
  source += "]; return values[299]; }";
  auto result = Prepare(source);
  ASSERT_TRUE(result.Succeeded());
  const auto& functions = result.program.functions;
  const auto main = std::ranges::find(functions, "main", &Function::name);
  ASSERT_NE(main, functions.end());
  EXPECT_GT(main->register_count, 256U);
  EXPECT_TRUE(std::ranges::any_of(main->instructions, [](const Instruction& instruction) {
    return instruction.opcode == Opcode::NewArray && instruction.immediate == kArrayLength;
  }));
}

TEST(PreparationTest, ClassInterfaceMetadataHasCanonicalOrder) {
  constexpr std::uint32_t kInterfaces = 8;
  std::string source;
  for (std::uint32_t index = 0; index < kInterfaces; ++index) {
    source += "interface I" + std::to_string(index) + " { read(): int; }";
  }
  source += "class Value implements ";
  for (std::uint32_t index = kInterfaces; index > 0; --index) {
    source += "I" + std::to_string(index - 1) + (index == 1 ? "" : ",");
  }
  source += "{ read(): int { return 42; } } func main(): int { return Value().read(); }";
  const auto prepared = Prepare(source);
  ASSERT_TRUE(prepared.Succeeded()) << frontend::DiagnosticsText(prepared.diagnostics);
  const auto& classes = prepared.program.layouts.classes;
  const auto found = std::ranges::find(classes, "Value", &layout::Class::name);
  ASSERT_NE(found, classes.end());
  ASSERT_EQ(found->interfaces.size(), kInterfaces);
  for (std::uint32_t index = 0; index < kInterfaces; ++index) {
    EXPECT_EQ(found->interfaces.at(index), index);
  }
}

TEST(PreparationTest, OwnsLayoutsAndSourcePathsAfterFrontendDestructionAndResultMoves) {
  auto first = Prepare(R"(
class Box { value: int; }
func main(): int { return Box(value: 42).value; }
)");
  ASSERT_TRUE(first.Succeeded());
  auto prepared = std::move(first.program);
  first = {};
  const auto& classes = prepared.layouts.classes;
  const auto found = std::ranges::find(classes, "Box", &layout::Class::name);
  ASSERT_NE(found, classes.end());
  EXPECT_EQ(found->field_count, 1U);
  EXPECT_TRUE(found->reference_fields.empty());
  EXPECT_FALSE(prepared.classes.empty());
  EXPECT_TRUE(std::ranges::any_of(
      prepared.source_paths, [](const std::string& path) { return path.ends_with("main.vela"); }));
}

TEST(PreparationTest, ReportsImportedTypeErrorsBeforeLowering) {
  const auto sources = std::to_array<frontend::SourceInput>({
      {.path = "main.vela", .text = "import 'library'; func main(): int { return answer(); }"},
      {.path = "library.vela", .text = "export func answer(): int { return true; }"},
  });
  const auto result = PrepareParsed(frontend::LoadSources(sources, "main.vela"));
  ASSERT_FALSE(result.Succeeded());
  ASSERT_FALSE(result.diagnostics.empty());
  EXPECT_TRUE(result.diagnostics.front().source_path.ends_with("library.vela"));
}

TEST(PreparationTest, EmitsFieldDefaultsOnceInTheConstructorAcrossManyCallSites) {
  std::string source =
      "external func initial(): int; class Box { value: int = initial(); } "
      "func main(): void {";
  constexpr int kConstructions = 50;
  for (int index = 0; index < kConstructions; ++index) {
    source += "Box();";
  }
  source += "}";
  const auto prepared = Prepare(source);
  ASSERT_TRUE(prepared.Succeeded());
  const auto& functions = prepared.program.functions;
  const auto initial = std::ranges::find(functions, "initial", &Function::name);
  ASSERT_NE(initial, functions.end());
  const auto target = static_cast<std::uint32_t>(initial - functions.begin());
  std::size_t default_calls = 0;
  for (const auto& function : functions) {
    for (const auto& instruction : function.instructions) {
      if (instruction.opcode == Opcode::Call && instruction.index == target) {
        ++default_calls;
        EXPECT_EQ(function.name, "Box.$constructor");
      }
    }
  }
  EXPECT_EQ(default_calls, 1U);
}

TEST(PreparationTest, UnusedConstructorsDoNotIntroduceBodiesOrStringRoots) {
  for (const auto* const body : {"", "constructor {}", "constructor() {}"}) {
    SCOPED_TRACE(body);
    const auto prepared = Prepare("class Unused { value: string = 'unused-constructor-literal'; " +
                                  std::string(body) + " } func main(): int { return 42; }");
    ASSERT_TRUE(prepared.Succeeded());
    EXPECT_FALSE(std::ranges::any_of(prepared.program.functions, [](const Function& function) {
      return function.name == "Unused.$constructor";
    }));
    EXPECT_EQ(std::ranges::find(prepared.program.strings, "unused-constructor-literal"),
              prepared.program.strings.end());
  }
}

}  // namespace
}  // namespace vela::ir
