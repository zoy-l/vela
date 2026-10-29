#include "compiler/ir/preparation.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

#include "base/layout_description.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/source_loader.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/ir/ir.h"

namespace vela::ir {
namespace {

std::expected<Program, frontend::Diagnostics> Prepare(std::string_view source) {
  const frontend::SourceInput input{.path = "main.vela", .text = source};
  return PrepareParsed(frontend::LoadSources(std::span(&input, 1), input.path));
}

TEST(PreparationTest, PreservesUnboundNativeImportsWithoutRuntimeState) {
  auto result = Prepare(R"(
external func read(): int = "host.read";
func main(): int { return read() + 2; }
)");
  ASSERT_TRUE(result.has_value());
  const auto& functions = result->functions;
  const auto found = std::ranges::find(functions, "host.read", &Function::native_symbol);
  ASSERT_NE(found, functions.end());
  ASSERT_TRUE(found->signature.has_value());
  EXPECT_TRUE(found->instructions.empty());
  EXPECT_EQ(found->return_type.kind, frontend::TypeDescriptor::Kind::Int);
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
  ASSERT_TRUE(result.has_value());
  const auto& functions = result->functions;
  const auto main = std::ranges::find(functions, "main", &Function::name);
  ASSERT_NE(main, functions.end());
  EXPECT_GT(main->register_count, 256U);
  EXPECT_TRUE(std::ranges::any_of(main->instructions, [](const Instruction& instruction) {
    return instruction.opcode == Opcode::NewArray && instruction.immediate == kArrayLength;
  }));
}

TEST(PreparationTest, OwnsLayoutsAndSourcePathsAfterFrontendDestructionAndResultMoves) {
  auto first = Prepare(R"(
class Box { value: int; }
func main(): int { return Box(value: 42).value; }
)");
  ASSERT_TRUE(first.has_value());
  auto prepared = std::move(*first);
  first = std::unexpected(frontend::Diagnostics{});
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
  ASSERT_FALSE(result.has_value());
  ASSERT_FALSE(result.error().empty());
  EXPECT_TRUE(result.error().front().source_path.ends_with("library.vela"));
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
  ASSERT_TRUE(prepared.has_value());
  const auto& functions = prepared->functions;
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
  for (const auto body : {"", "constructor {}", "constructor() {}"}) {
    SCOPED_TRACE(body);
    const auto prepared = Prepare("class Unused { value: string = 'unused-constructor-literal'; " +
                                  std::string(body) + " } func main(): int { return 42; }");
    ASSERT_TRUE(prepared.has_value());
    EXPECT_FALSE(std::ranges::any_of(prepared->functions, [](const Function& function) {
      return function.name == "Unused.$constructor";
    }));
    EXPECT_EQ(std::ranges::find(prepared->strings, "unused-constructor-literal"),
              prepared->strings.end());
  }
}

}  // namespace
}  // namespace vela::ir
