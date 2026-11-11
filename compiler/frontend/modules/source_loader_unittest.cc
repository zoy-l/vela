#include "compiler/frontend/modules/source_loader.h"

#include <array>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "base/path.h"
#include "base/testing/directory.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/syntax/parser.h"

namespace vela::frontend {
namespace {

class SourceCaptureTest : public vela::test::DirectoryTest {};

TEST_F(SourceCaptureTest, AcceptedGeneratedDependenciesRetainTheirVerifiedSnapshot) {
  ASSERT_NO_FATAL_FAILURE(Write("main.vela", "import 'generated.vela';\n"));
  const auto entry = PathText(directory() / "main.vela");
  const auto dependency = PathText(directory() / "generated.vela");
  const auto generated = std::make_shared<const ParsedSource>(ParseSourceSnapshot(
      "// Binding generation: accepted\nexport func answer(): int { return 42; }"));
  ASSERT_TRUE(generated->diagnostics.empty());
  // 模拟此前已验证并接纳的生成快照；本次磁盘上没有这批产品。
  ModuleCache accepted;
  accepted.sources.emplace(dependency, generated);
  auto candidate = accepted;
  const std::array roots{entry};
  const auto captured = CaptureSourceFiles(roots, candidate, {}, &accepted);
  ASSERT_TRUE(captured.diagnostics.empty()) << DiagnosticsText(captured.diagnostics);
  ASSERT_EQ(captured.sources.size(), 1U);
  EXPECT_EQ(captured.sources.front().path, entry);
  EXPECT_EQ(candidate.sources.at(dependency), generated);

  const auto complete = CaptureSourceFiles(roots, candidate, {});
  ASSERT_EQ(complete.diagnostics.size(), 1U);
  EXPECT_TRUE(complete.diagnostics.front().message.starts_with("missing binding manifest:"));

  const std::array overlays{SourceInput{.path = dependency, .text = generated->source->text}};
  const auto overridden = CaptureSourceFiles(roots, candidate, overlays, &accepted);
  ASSERT_EQ(overridden.diagnostics.size(), 1U);
  EXPECT_TRUE(overridden.diagnostics.front().message.starts_with("missing binding manifest:"));
}

TEST_F(SourceCaptureTest, PreparedRootsKeepMemoryOriginAndCaptureFileDependencies) {
  ASSERT_NO_FATAL_FAILURE(Write("dependency.vela", "export func answer(): int { return 42; }\n"));
  const auto entry = PathText(directory() / "__repl__.vela");
  const auto dependency = PathText(directory() / "dependency.vela");
  const auto prepared = std::make_shared<const ParsedSource>(ParseSourceSnapshot(
      "// Binding generation: ordinary user comment\nimport 'dependency.vela';\n"));
  ModuleCache cache;
  const auto captured = CapturePreparedSourceFiles(entry, prepared, cache);
  ASSERT_TRUE(captured.diagnostics.empty()) << DiagnosticsText(captured.diagnostics);
  ASSERT_EQ(captured.sources.size(), 2U);
  EXPECT_EQ(captured.sources.front().path, entry);
  EXPECT_EQ(captured.sources.front().text, prepared->source->text);
  EXPECT_EQ(cache.sources.at(entry), prepared);
  EXPECT_TRUE(cache.sources.contains(dependency));

  const auto missing = std::make_shared<const ParsedSource>(ParseSourceSnapshot(
      "// Binding generation: ordinary user comment\nimport 'missing.vela';\n"));
  const auto rejected = CapturePreparedSourceFiles(entry, missing, cache);
  ASSERT_EQ(rejected.diagnostics.size(), 1U);
  EXPECT_EQ(rejected.diagnostics.front().kind, DiagnosticKind::Io);
}

}  // namespace
}  // namespace vela::frontend
