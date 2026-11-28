#include "compiler/cli/compilation_progress.h"

#include <cstddef>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

#include "compiler/development/development_session.h"
#include "compiler/development/source_session.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_progress.h"
#include "compiler/jit/compiler.h"
#include "compiler/jit/source_compiler.h"

namespace vela::tooling {
namespace {

TEST(CompilationProgressTest, InMemoryCompilationForwardsFileProgressToTheCaller) {
  auto compiler = compiler::jit::Compiler::Create();
  ASSERT_TRUE(compiler);
  std::size_t files = 0;
  std::size_t loading = 0;
  const auto result =
      compiler::jit::CompileSource(**compiler, "func main(): int { return 42; }", {},
                                   [&](const frontend::SourceProgress& event) {
                                     if (event.kind == frontend::SourceProgressKind::File) {
                                       ++files;
                                       EXPECT_EQ(event.path, "main.vela");
                                       EXPECT_EQ(event.completed, 1U);
                                       EXPECT_EQ(event.total, 1U);
                                     } else {
                                       ++loading;
                                     }
                                   });
  EXPECT_TRUE(result.Succeeded()) << frontend::DiagnosticsText(result.diagnostics);
  EXPECT_EQ(files, 1U);
  EXPECT_EQ(loading, 1U);
}

TEST(CompilationProgressTest, DevelopmentPreparationAcceptsAnEmptyObserver) {
  auto session = compiler::DevelopmentSession::Create("main.vela");
  ASSERT_TRUE(session);
  compiler::SourceUpdate update;
  update.changes.push_back({.path = "main.vela", .text = "func main(): void {}"});
  const auto candidate =
      (*session)->Prepare(std::move(update), compiler::DevelopmentOperation::Load, {});
  ASSERT_TRUE(candidate) << frontend::DiagnosticsText(candidate.error().diagnostics);
}

TEST(CompilationProgressTest, PlainOutputContainsOnlyFileCountersAndLoading) {
  constexpr std::size_t kCompletedFiles = 12;
  constexpr std::size_t kTotalFiles = 40;
  std::ostringstream output;
  CompilationProgressDisplay display(output, CompilationProgressOutput::Plain);
  display.Report({
      .kind = frontend::SourceProgressKind::File,
      .path = "src/player.vela",
      .completed = kCompletedFiles,
      .total = kTotalFiles,
  });
  display.Report({.kind = frontend::SourceProgressKind::Loading, .path = {}});
  display.Report({.kind = frontend::SourceProgressKind::Loading, .path = {}});
  display.Finish();
  display.Finish();
  display.Report({
      .kind = frontend::SourceProgressKind::File,
      .path = "ignored.vela",
      .completed = kTotalFiles,
      .total = kTotalFiles,
  });
  EXPECT_EQ(output.str(), "loading\n[12/40 · 30%] src/player.vela\nloading\n");
}

TEST(CompilationProgressTest, InteractiveOutputClearsTheStatusBeforeDiagnostics) {
  std::ostringstream output;
  {
    CompilationProgressDisplay display(output, CompilationProgressOutput::Interactive);
    display.Report({
        .kind = frontend::SourceProgressKind::File,
        .path = "src/a_long_player_file.vela",
        .completed = 1,
        .total = 1,
    });
    display.Report({.kind = frontend::SourceProgressKind::Loading, .path = {}});
    display.Finish();
  }
  const auto status = output.str();
  EXPECT_NE(status.find("[1/1 · 100%] src/a_long_player_file.vela"), std::string::npos);
  EXPECT_NE(status.find("loading |"), std::string::npos);
  EXPECT_EQ(status.find('\n'), std::string::npos);
  ASSERT_FALSE(status.empty());
  EXPECT_EQ(status.back(), '\r');
  output << "diagnostic\n";
  EXPECT_TRUE(output.str().ends_with("\rdiagnostic\n"));
}

TEST(CompilationProgressTest, FileNamesCannotInjectAdditionalStatusLines) {
  std::ostringstream output;
  CompilationProgressDisplay display(output, CompilationProgressOutput::Plain);
  display.Report({
      .kind = frontend::SourceProgressKind::File,
      .path = "src/player\n\x1b.vela",
      .completed = 1,
      .total = 1,
  });
  display.Finish();
  EXPECT_EQ(output.str(), "loading\n[1/1 · 100%] src/player??.vela\n");
}

}  // namespace
}  // namespace vela::tooling
