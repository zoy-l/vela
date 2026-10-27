#include "compiler/aot/native_compiler.h"

#include <expected>
#include <span>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/source_loader.h"
#include "compiler/ir/ir.h"
#include "compiler/ir/preparation.h"

namespace vela::compiler::aot {
namespace {

std::expected<ir::Program, frontend::Diagnostics> Prepare(std::string_view source) {
  const frontend::SourceInput input{.path = "main.vela", .text = source};
  return ir::PrepareParsed(frontend::LoadSources(std::span(&input, 1), input.path));
}

TEST(NativeCompilerTest, EmitsOfflineObjectWithScalarCallsAndControlFlow) {
  const auto prepared = Prepare(R"(
func twice(value: int): int { return value * 2; }
export func main(): int {
  let sum = 0;
  for (let index = 0; index < 10; index++) { sum += twice(index); }
  return sum;
}
)");
  ASSERT_TRUE(prepared.has_value());
  const auto result = Compile(*prepared, {.exports = {"main"}});
  ASSERT_TRUE(result.has_value()) << result.error().front().message;
  EXPECT_FALSE(result->object.empty());
  EXPECT_FALSE(result->target.empty());
  EXPECT_NE(result->llvm_ir.find("vela_aot_entry_"), std::string::npos);
}

TEST(NativeCompilerTest, VerifiesBooleanNegationAndEmptyVoidBodies) {
  const auto prepared = Prepare(R"(
export func empty(): void {}
export func negate(value: bool): bool { return !value; }
)");
  ASSERT_TRUE(prepared.has_value());
  const auto result = Compile(*prepared, {.exports = {"empty", "negate"}});
  ASSERT_TRUE(result.has_value()) << result.error().front().message;
  EXPECT_FALSE(result->object.empty());
}

TEST(NativeCompilerTest, ExportsOnlyPublicFunctionsFromTheEntryModule) {
  const frontend::SourceInput sources[] = {
      {.path = "main.vela", .text = "import 'library'; func hidden(): int { return 1; }"},
      {.path = "library.vela", .text = "export func imported(): int { return 2; }"},
  };
  const auto prepared = ir::PrepareParsed(frontend::LoadSources(sources, "main.vela"));
  ASSERT_TRUE(prepared.has_value());
  const auto private_export = Compile(*prepared, {.exports = {"hidden"}});
  ASSERT_FALSE(private_export.has_value());
  EXPECT_NE(private_export.error().front().message.find("public entry-module"), std::string::npos);
  const auto imported_export = Compile(*prepared, {.exports = {"imported"}});
  ASSERT_FALSE(imported_export.has_value());
}

}  // namespace
}  // namespace vela::compiler::aot
