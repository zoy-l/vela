#include "compiler/ir/preparation.h"

#include <algorithm>
#include <array>
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

}  // namespace
}  // namespace vela::ir
