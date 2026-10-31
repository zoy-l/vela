#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "gtest/gtest.h"

#include "compiler/test_support/program_test_support.h"
#include "compiler/frontend/modules/source_file.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/frontend/typing/checker.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_program_state.h"

namespace vela::compiler {
namespace {

TEST(SdkModulesTest, SharesIdentityAcrossMemoryModulesAndNativeInstances) {
  const auto sources = std::to_array<frontend::SourceInput>({
      {
          .path = "main.vela",
          .text = R"(
import 'sdk:test' as t;
import 'support.vela';
func main(): int {
  const timeout = t.Timeout(milliseconds: 42);
  if (timeout.milliseconds != 42 || t.Timeout.factor(2.0).multiplier != 2.0) { return 0; }
  if (t.Timeout.none().mode != 2 || !valid()) { return 1; }
  try { t.fail('original message'); }
  catch (error: t.TestFailure) { if (error.message == 'original message') { return 42; } }
  return 2;
})",
      },
      {
          .path = "support.vela",
          .text = R"(
import 'sdk:test';
class Custom implements Matcher {
  match(actual: dynamic): MatchResult { return MatchResult(success: true, description: 'custom'); }
}
export func valid(): bool {
  const matcher: Matcher = Custom();
  return matcher.match(42).success;
})",
      },
  });
  auto loaded = frontend::LoadSources(sources, "main.vela");
  ASSERT_TRUE(loaded.diagnostics.empty());
  EXPECT_EQ(std::ranges::count(loaded.program.sources, "sdk:test", &frontend::SourceFile::path), 1);
  runtime::Isolate isolate(1);
  const auto compiled = test::CompileParsed(std::move(loaded), isolate);
  for (const auto& diagnostic : compiled.diagnostics) {
    ADD_FAILURE() << diagnostic.source_path << ':' << diagnostic.location.line << ':'
                  << diagnostic.location.column << ' ' << diagnostic.message;
  }
  ASSERT_TRUE(compiled.Succeeded());
  const auto image = runtime::Heap::NativeProgram(*compiled.instance->state().front().AsObject()).image_owner;
  runtime::Isolate restored(1);
  const auto program = runtime::NativeInstance::Instantiate(restored, image);
  ASSERT_TRUE(program);
  const auto executed = test::Execute(**program);
  ASSERT_TRUE(executed.Succeeded()) << executed.message;
  EXPECT_EQ(executed.value.AsInteger(), 42);
}

TEST(SdkModulesTest, RejectsOverridesUnknownModulesAndUnrequestedDiskReads) {
  const auto overrides = std::to_array<frontend::SourceInput>({
      {.path = "main.vela", .text = "import 'sdk:test'; func main(): void {}"},
      {.path = "sdk:test", .text = "export class Fake {}"},
  });
  EXPECT_FALSE(frontend::CheckParsed(frontend::LoadSources(overrides, "main.vela")).Succeeded());
  const auto sources = std::to_array<frontend::SourceInput>({
      {.path = "main.vela", .text = "import 'sdk:missing'; func main(): void {}"},
  });
  const auto unknown = frontend::LoadSources(sources, "main.vela");
  ASSERT_EQ(unknown.diagnostics.size(), 1U);
  EXPECT_EQ(unknown.diagnostics.front().message, "unknown SDK module 'sdk:missing'");
  const auto disk = std::to_array<frontend::SourceInput>({
      {.path = "main.vela", .text = "import 'compiler/sdk/test.vela'; func main(): void {}"},
  });
  EXPECT_FALSE(frontend::LoadSources(disk, "main.vela").diagnostics.empty());
}

}  // namespace
}  // namespace vela::compiler
