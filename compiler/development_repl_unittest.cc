#include "compiler/development_repl.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <gtest/gtest.h>

#include "base/file.h"
#include "base/path.h"
#include "compiler/development_files.h"
#include "compiler/development_session.h"
#include "compiler/repl_source.h"
#include "compiler/source_session.h"
#include "runtime/rooted_value.h"
#include "runtime/value.h"

namespace vela::compiler {
namespace {

class DevelopmentReplTest : public testing::Test {
 protected:
  void SetUp() override {
    CreateDirectory();
    ASSERT_FALSE(HasFatalFailure());
    LoadSession();
  }

  void CreateDirectory() {
    std::error_code error;
    const auto temporary = std::filesystem::temp_directory_path(error);
    ASSERT_FALSE(error);
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    directory_ = temporary / ("vela-development-repl-" + std::to_string(suffix));
    ASSERT_TRUE(std::filesystem::create_directory(directory_, error));
    ASSERT_FALSE(error);
  }

  void LoadSession() {
    Write("main.vela", "func main(): void {}\n");
    files_ = std::make_unique<DevelopmentFiles>(Path("main.vela"));
    auto update = files_->Capture(CompiledSourceSnapshot{});
    ASSERT_TRUE(update);
    DevelopmentOptions options;
    options.gc_threshold = 1;
    auto created = DevelopmentSession::Create(files_->entry(), {}, std::move(options));
    ASSERT_TRUE(created);
    session_ = std::move(*created);
    session_->isolate().SetOutput(output_);
    auto candidate = session_->Prepare(std::move(*update), DevelopmentOperation::Load);
    ASSERT_TRUE(candidate);
    (void)session_->Submit(std::move(*candidate));
    ASSERT_EQ(session_->Pump().status, DevelopmentStatus::Accepted);
    repl_ = std::make_unique<DevelopmentRepl>(*session_, Path("__repl__.vela"));
  }

  void TearDown() override {
    repl_.reset();
    session_.reset();
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
    EXPECT_FALSE(error);
  }

  void Write(std::string_view path, std::string_view text) {
    ASSERT_TRUE(WriteFileAtomically(directory_ / Utf8Path(path), std::as_bytes(std::span(text))));
  }
  [[nodiscard]] std::string Path(std::string_view path) const {
    return PathText(directory_ / Utf8Path(path));
  }
  void Execute(std::string_view input) {
    const auto result = repl_->Submit(input);
    for (const auto& diagnostic : result.diagnostics) {
      ADD_FAILURE() << diagnostic.message;
    }
    if (result.update) {
      for (const auto& diagnostic : result.update->diagnostics) {
        ADD_FAILURE() << diagnostic.message;
      }
    }
    EXPECT_EQ(result.status, SubmissionStatus::Executed);
  }
  DevelopmentResult UpdateFiles(DevelopmentOperation operation) {
    auto update = files_->Capture(*session_->snapshot());
    EXPECT_TRUE(update);
    if (!update) {
      DevelopmentResult result;
      result.status = DevelopmentStatus::SourceError;
      return result;
    }
    auto candidate = session_->Prepare(std::move(*update), operation);
    EXPECT_TRUE(candidate);
    if (!candidate) {
      return std::move(candidate.error());
    }
    (void)session_->Submit(std::move(*candidate));
    return session_->Pump();
  }

  DevelopmentResult Reset() {
    auto update = files_->Capture(*session_->snapshot());
    EXPECT_TRUE(update);
    if (!update) {
      DevelopmentResult result;
      result.status = DevelopmentStatus::SourceError;
      return result;
    }
    return repl_->Reset(std::move(*update));
  }

  [[nodiscard]] DevelopmentRepl& repl() const { return *repl_; }
  [[nodiscard]] DevelopmentSession& session() const { return *session_; }
  [[nodiscard]] std::string output() const { return output_.str(); }

 private:
  std::filesystem::path directory_;
  std::ostringstream output_;
  std::unique_ptr<DevelopmentFiles> files_;
  std::unique_ptr<DevelopmentSession> session_;
  std::unique_ptr<DevelopmentRepl> repl_;
};

TEST_F(DevelopmentReplTest, AppendsDeclarationsAndStatementsWithoutReplayingTheirEffects) {
  Execute("func initial(): int { print('once'); return 40; }");
  Execute("let value = initial();");
  Execute("value = value + 1;");
  Execute("value + 1");
  EXPECT_EQ(output(), "once\n42\n");
  const auto revision = session().snapshot()->revision;
  const auto history = std::string(repl().source());
  EXPECT_EQ(repl().Submit("func unfinished()").status, SubmissionStatus::Incomplete);
  EXPECT_EQ(repl().Submit("let value = 999;").status, SubmissionStatus::Rejected);
  EXPECT_EQ(session().snapshot()->revision, revision);
  EXPECT_EQ(repl().source(), history);
  Execute("value");
  EXPECT_EQ(output(), "once\n42\n41\n");
}

TEST_F(DevelopmentReplTest, RetiredEntryClosuresKeepTheirCaptureLayoutsUnderGcPressure) {
  Execute("let saved: () => int = (): int => 0;");
  Execute("{ let n = 40; saved = (): int => { n = n + 1; return n; }; }");
  constexpr int kSubmissions = 12;
  for (int index = 0; index < kSubmissions; ++index) {
    Execute("saved();");
  }
  session().isolate().heap().Collect();
  EXPECT_LE(session().code_resources().batches, 2U);
  Execute("saved()");
  EXPECT_EQ(output(), "53\n");
}

TEST_F(DevelopmentReplTest, FileChangesRequireExplicitUpdatesAndPreserveCapturedFunctionVersions) {
  Write("library.vela", "export func answer(): int { return 42; }");
  Execute("import 'library.vela' as lib;");
  Execute("const saved = lib.answer;");
  Execute("lib.answer()");
  Write("library.vela", "export func answer(): int { return 100; }");
  Execute("lib.answer()");
  EXPECT_EQ(output(), "42\n42\n");
  EXPECT_EQ(UpdateFiles(DevelopmentOperation::HotReload).status, DevelopmentStatus::Accepted);
  Execute("lib.answer()");
  Execute("saved()");
  EXPECT_EQ(output(), "42\n42\n100\n42\n");
}

TEST_F(DevelopmentReplTest, ReloadRetainsDeclarationsAndDoesNotRunHistoricalStatements) {
  Execute("let value = 40;");
  Execute("value = 42; print('statement');");
  EXPECT_EQ(UpdateFiles(DevelopmentOperation::Reload).status, DevelopmentStatus::Accepted);
  EXPECT_EQ(session().generation(), 2U);
  Execute("value");
  EXPECT_EQ(output(), "statement\n40\n");
}

TEST_F(DevelopmentReplTest, ResetWaitsForHostRootsAndOnlyClearsAcceptedHistoryAfterSwitching) {
  Execute("let value = 42;");
  {
    runtime::RootedValue blocker(session().isolate(), runtime::Value::Null());
    EXPECT_EQ(Reset().status, DevelopmentStatus::WaitingForRelease);
    EXPECT_FALSE(repl().source().empty());
    ASSERT_TRUE(session().Cancel());
  }
  Execute("value");
  EXPECT_EQ(Reset().status, DevelopmentStatus::Accepted);
  EXPECT_TRUE(repl().source().empty());
  EXPECT_EQ(repl().Submit("value").status, SubmissionStatus::Rejected);
  Execute("let value = 100;");
  Execute("value");
  EXPECT_EQ(output(), "42\n100\n");
}

TEST_F(DevelopmentReplTest, ResetAcceptsAnExplicitDiskUpdateBeforeReimportingModules) {
  Write("library.vela", "export func answer(): int { return 42; }");
  Execute("import 'library.vela' as lib;");
  Write("library.vela", "export func answer(): int { return 100; }");
  Execute("lib.answer()");
  ASSERT_EQ(Reset().status, DevelopmentStatus::Accepted);
  Execute("import 'library.vela' as lib;");
  Execute("lib.answer()");
  EXPECT_EQ(output(), "42\n100\n");
}

TEST_F(DevelopmentReplTest, RuntimeErrorsKeepCommittedDeclarationsAndCarrySubmissionLocations) {
  const auto failed = repl().Submit("let zero = 0;\nlet value = 1 / zero;");
  ASSERT_EQ(failed.status, SubmissionStatus::RuntimeError);
  const auto& optional_diagnostic = failed.runtime_diagnostic;
  if (!optional_diagnostic) {
    FAIL() << "runtime failure did not preserve its diagnostic";
  }
  const auto& diagnostic = *optional_diagnostic;
  ASSERT_FALSE(diagnostic.stack_trace.empty());
  EXPECT_EQ(diagnostic.stack_trace.front().source_path, "<repl:1>");
  EXPECT_EQ(diagnostic.stack_trace.front().line, 2U);
  Execute("zero = 1;");
  Execute("value");
  EXPECT_EQ(output(), "1\n");
}

}  // namespace
}  // namespace vela::compiler
