#include "compiler/ir/preparation.h"

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

#include "compiler/frontend/source_loader.h"

namespace vela::ir {
namespace {

std::expected<PreparedProgram, frontend::Diagnostics> Prepare(std::string_view source) {
  const frontend::SourceInput input{.path = "main.vela", .text = source};
  return PrepareParsed(frontend::LoadSources(std::span(&input, 1), input.path));
}

TEST(PreparationTest, PreservesUnboundNativeImportsWithoutRuntimeState) {
  auto result = Prepare(R"(
external func read(): int = "host.read";
func main(): int { return read() + 2; }
)");
  ASSERT_TRUE(result.has_value());
  const auto& functions = result->program.functions;
  const auto found = std::ranges::find(functions, "host.read", &Function::native_symbol);
  ASSERT_NE(found, functions.end());
  ASSERT_TRUE(found->signature.has_value());
  EXPECT_TRUE(found->instructions.empty());
  EXPECT_EQ(found->return_type.kind, frontend::TypeDescriptor::Kind::Int);
}

TEST(PreparationTest, KeepsWideVirtualRegistersBeforeBackendAllocation) {
  std::string source = "func main(): int { const values = [";
  for (int index = 0; index < 300; ++index) {
    if (index != 0) {
      source += ',';
    }
    source += std::to_string(index);
  }
  source += "]; return values[299]; }";
  auto result = Prepare(source);
  ASSERT_TRUE(result.has_value());
  const auto& functions = result->program.functions;
  const auto main = std::ranges::find(functions, "main", &Function::name);
  ASSERT_NE(main, functions.end());
  EXPECT_GT(main->register_count, 256U);
  EXPECT_TRUE(std::ranges::any_of(main->instructions, [](const Instruction& instruction) {
    return instruction.opcode == Opcode::NewArray && instruction.immediate == 300;
  }));
}

TEST(PreparationTest, OwnsSemanticReferencesAfterSourceAndResultMoves) {
  auto first = Prepare(R"(
class Box { param value: int; }
func main(): int { return Box(value: 42).value; }
)");
  ASSERT_TRUE(first.has_value());
  auto prepared = std::move(*first);
  first = std::unexpected(frontend::Diagnostics{});
  const auto& descriptor = prepared.model.classes.at("Box");
  ASSERT_NE(descriptor.declaration, nullptr);
  EXPECT_EQ(descriptor.declaration->name, "Box");
  EXPECT_EQ(descriptor.field_order.front(), "value");
  EXPECT_FALSE(prepared.program.classes.empty());
}

TEST(PreparationTest, ReportsImportedTypeErrorsBeforeLowering) {
  const frontend::SourceInput sources[] = {
      {.path = "main.vela", .text = "import 'library'; func main(): int { return answer(); }"},
      {.path = "library.vela", .text = "export func answer(): int { return true; }"},
  };
  const auto result = PrepareParsed(frontend::LoadSources(sources, "main.vela"));
  ASSERT_FALSE(result.has_value());
  ASSERT_FALSE(result.error().empty());
  EXPECT_TRUE(result.error().front().source_path.ends_with("library.vela"));
}

}  // namespace
}  // namespace vela::ir
