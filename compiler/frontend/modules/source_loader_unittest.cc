#include "compiler/frontend/modules/source_loader.h"

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "base/path.h"
#include "base/testing/directory.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/native_binding_source.h"
#include "compiler/frontend/modules/source_progress.h"
#include "compiler/frontend/syntax/parser.h"

namespace vela::frontend {
namespace {

class SourceCaptureTest : public vela::test::DirectoryTest {};

TEST(SourceProgressTest, CountsEachUserFileOnceAfterDiscoveringTheWholeGraph) {
  const std::array inputs{
      SourceInput{.path = "main.vela", .text = "import 'left'; import 'right'; import 'sdk:test';"},
      SourceInput{.path = "left.vela", .text = "import 'shared';"},
      SourceInput{.path = "right.vela", .text = "import 'shared';"},
      SourceInput{.path = "shared.vela", .text = "export func value(): int { return 42; }"},
  };
  std::vector<std::string> paths;
  bool loading = false;
  const auto parsed = LoadSources(inputs, "main.vela", {}, {}, [&](const SourceProgress& event) {
    if (event.kind == SourceProgressKind::Loading) {
      loading = true;
      EXPECT_EQ(event.completed, inputs.size());
      EXPECT_EQ(event.total, inputs.size());
      return;
    }
    EXPECT_FALSE(loading);
    paths.emplace_back(event.path);
    EXPECT_EQ(event.completed, paths.size());
    EXPECT_EQ(event.total, inputs.size());
  });
  EXPECT_FALSE(HasErrors(parsed.diagnostics)) << DiagnosticsText(parsed.diagnostics);
  EXPECT_EQ(paths,
            (std::vector<std::string>{"main.vela", "left.vela", "right.vela", "shared.vela"}));
  EXPECT_TRUE(loading);
}

TEST_F(SourceCaptureTest, CachedDiskLoadingReportsTheSameRealFileCount) {
  ASSERT_NO_FATAL_FAILURE(Write("main.vela", "import 'library';"));
  ASSERT_NO_FATAL_FAILURE(Write("library.vela", "export func value(): int { return 42; }"));
  ModuleCache cache;
  const auto entry = PathText(directory() / "main.vela");
  for (int pass = 0; pass < 2; ++pass) {
    std::size_t completed = 0;
    const auto parsed = LoadProgram(entry, cache, {}, SourceLoadMode::Compilation,
                                    [&](const SourceProgress& event) {
                                      if (event.kind == SourceProgressKind::File) {
                                        EXPECT_EQ(event.completed, ++completed);
                                        EXPECT_EQ(event.total, 2U);
                                      }
                                    });
    EXPECT_FALSE(HasErrors(parsed.diagnostics)) << DiagnosticsText(parsed.diagnostics);
    EXPECT_EQ(completed, 2U);
  }
}

TEST(SourceProgressTest, SyntaxFailureDoesNotReportUnprocessedFilesAsCompleted) {
  const std::array inputs{SourceInput{.path = "main.vela", .text = "func broken("}};
  std::size_t notifications = 0;
  const auto parsed =
      LoadSources(inputs, "main.vela", {}, {}, [&](const SourceProgress&) { ++notifications; });
  EXPECT_TRUE(HasErrors(parsed.diagnostics));
  EXPECT_EQ(notifications, 0U);
}

TEST_F(SourceCaptureTest, BindingManifestReportsJsonParseFailure) {
  ASSERT_NO_FATAL_FAILURE(Write("generated.bindings.json", "{"));
  const auto result =
      VerifyNativeBindingSource(directory() / "generated.vela", "// Binding generation: token\n");
  ASSERT_FALSE(result);
  EXPECT_NE(result.error().find("invalid binding manifest:"), std::string::npos);
  EXPECT_NE(result.error().find("at byte "), std::string::npos);
}

TEST_F(SourceCaptureTest, ExplicitImportsResolveIdenticallyInDiskAndCapturedMemoryGraphs) {
  ASSERT_NO_FATAL_FAILURE(
      Write("main.vela", "import 'ui'; export func read(): int { return answer(); }"));
  ASSERT_NO_FATAL_FAILURE(Write("generated.vela", "export func answer(): int { return 42; }"));
  const auto entry = PathText(directory() / "main.vela");
  const auto dependency = PathText(directory() / "generated.vela");
  const ModulePaths imports{{"ui", dependency}};
  const auto disk = LoadProgram(entry, {}, SourceLoadMode::Compilation, {}, imports);
  ASSERT_FALSE(HasErrors(disk.diagnostics)) << DiagnosticsText(disk.diagnostics);
  ModuleCache cache;
  cache.imports = imports;
  const std::array roots{entry};
  const auto captured = CaptureSourceFiles(roots, cache, {});
  ASSERT_TRUE(captured.diagnostics.empty()) << DiagnosticsText(captured.diagnostics);
  ASSERT_EQ(captured.sources.size(), 2U);
  const std::array memory{
      SourceInput{.path = entry, .text = captured.sources[0].text},
      SourceInput{.path = dependency, .text = captured.sources[1].text},
  };
  const auto loaded = LoadSources(memory, entry, {}, imports);
  EXPECT_FALSE(HasErrors(loaded.diagnostics)) << DiagnosticsText(loaded.diagnostics);
  EXPECT_EQ(ResolveImportPath(directory() / "nested/main.vela", "ui", imports),
            directory() / "generated.vela");
}

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
  EXPECT_TRUE(complete.diagnostics.front().message.starts_with("cannot read binding manifest:"));

  const std::array overlays{SourceInput{.path = dependency, .text = generated->source->text}};
  const auto overridden = CaptureSourceFiles(roots, candidate, overlays, &accepted);
  ASSERT_EQ(overridden.diagnostics.size(), 1U);
  EXPECT_TRUE(overridden.diagnostics.front().message.starts_with("cannot read binding manifest:"));
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
