#include "compiler/aot/native_compiler.h"

#include <array>
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
  ASSERT_EQ(result->exports.size(), 1U);
  EXPECT_EQ(result->exports.front().symbol, "vela_aot_export_1");
  EXPECT_NE(result->header.find("VelaAotStatus vela_aot_export_1"), std::string::npos);
  EXPECT_NE(result->header.find("Call_main"), std::string::npos);
  EXPECT_EQ(result->instantiate_symbol, "vela_aot_instantiate");
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
  const auto sources = std::to_array<frontend::SourceInput>({
      {.path = "main.vela", .text = "import 'library'; func hidden(): int { return 1; }"},
      {.path = "library.vela", .text = "export func imported(): int { return 2; }"},
  });
  const auto prepared = ir::PrepareParsed(frontend::LoadSources(sources, "main.vela"));
  ASSERT_TRUE(prepared.has_value());
  const auto private_export = Compile(*prepared, {.exports = {"hidden"}});
  ASSERT_FALSE(private_export.has_value());
  EXPECT_NE(private_export.error().front().message.find("public entry-module"), std::string::npos);
  const auto imported_export = Compile(*prepared, {.exports = {"imported"}});
  ASSERT_FALSE(imported_export.has_value());
}

TEST(NativeCompilerTest, RejectsModuleNamesThatCannotProduceValidCHeaders) {
  const auto prepared = Prepare("export func value(): int { return 42; }");
  ASSERT_TRUE(prepared.has_value());
  const auto result = Compile(*prepared, {.module_name = "invalid-module", .exports = {"value"}});
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().front().message.find("C identifier"), std::string::npos);
}

TEST(NativeCompilerTest, EmitsPrivateAsyncMainAndRejectsCustomHostImports) {
  const auto prepared = Prepare("async func main(): Future<int> { return await 42; }");
  ASSERT_TRUE(prepared.has_value());
  const auto result =
      Compile(*prepared, {.exports = {}, .entry_mode = NativeEntryMode::Executable});
  ASSERT_TRUE(result.has_value()) << result.error().front().message;
  EXPECT_NE(result->llvm_ir.find("define i32 @main()"), std::string::npos);
  EXPECT_NE(result->llvm_ir.find("@vela_native_v1_main"), std::string::npos);
  EXPECT_TRUE(result->exports.empty());

  const auto custom = Prepare("external func host(): int; func main(): int { return host(); }");
  ASSERT_TRUE(custom.has_value());
  const auto rejected =
      Compile(*custom, {.exports = {}, .entry_mode = NativeEntryMode::Executable});
  ASSERT_FALSE(rejected.has_value());
  EXPECT_NE(rejected.error().front().message.find("custom host binding"), std::string::npos);
  EXPECT_TRUE(Compile(*custom).has_value());
}

TEST(NativeCompilerTest, RemovesUnusedImportsAndRetainsOnlySelectedEntryCode) {
  const auto prepared = Prepare(R"(
external func unused(): int;
func removed(): int { return unused(); }
export func answer(): int { return 42; }
)");
  ASSERT_TRUE(prepared.has_value());
  const auto result = Compile(*prepared, {.exports = {"answer"}});
  ASSERT_TRUE(result.has_value()) << result.error().front().message;
  ASSERT_EQ(result->program.functions.size(), 1U);
  EXPECT_EQ(result->program.functions.front().name, "answer");
  EXPECT_EQ(result->exports.front().function, 0U);
  EXPECT_EQ(result->llvm_ir.find("unused"), std::string::npos);
}

}  // namespace
}  // namespace vela::compiler::aot
