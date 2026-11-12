#include "compiler/test_support/development_test_support.h"

#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

#include "base/testing/directory.h"
#include "compiler/development/development_files.h"
#include "compiler/development/development_repl.h"
#include "compiler/development/development_session.h"
#include "compiler/development/source_session.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/repl/repl_source.h"
#include "runtime/isolate/isolate.h"

namespace vela::compiler::test {

DevelopmentResult ApplyUpdate(DevelopmentSession& session,
                              SourceUpdate update,
                              DevelopmentOperation operation) {
  auto candidate = session.Prepare(std::move(update), operation);
  if (!candidate) {
    return candidate.error();
  }
  auto result = session.Submit(std::move(*candidate));
  while (result.status == DevelopmentStatus::WaitingForSafepoint) {
    result = session.Pump();
  }
  return result;
}

void ExpectAccepted(const DevelopmentResult& result) {
  ASSERT_EQ(result.status, DevelopmentStatus::Accepted)
      << result.message << frontend::DiagnosticsText(result.diagnostics);
}

void DevelopmentReplTest::SetUp() {
  SetUp({});
}

void DevelopmentReplTest::SetUp(runtime::NativeBindings bindings) {
  vela::test::DirectoryTest::SetUp();
  ASSERT_FALSE(HasFatalFailure());
  LoadSession(std::move(bindings));
}

void DevelopmentReplTest::LoadSession(runtime::NativeBindings bindings) {
  repl_.reset();
  session_.reset();
  ASSERT_NO_FATAL_FAILURE(Write("main.vela", "func main(): void {}\n"));
  files_ = std::make_unique<DevelopmentFiles>(Path("main.vela"));
  auto update = files_->Capture(CompiledSourceSnapshot{});
  ASSERT_TRUE(update) << frontend::DiagnosticsText(update.error());
  DevelopmentOptions options;
  options.gc_threshold = 1;
  auto created =
      DevelopmentSession::Create(files_->entry(), std::move(bindings), std::move(options));
  ASSERT_TRUE(created);
  session_ = std::move(*created);
  session_->isolate().SetOutput(output_);
  ASSERT_NO_FATAL_FAILURE(
      ExpectAccepted(ApplyUpdate(*session_, std::move(*update), DevelopmentOperation::Load)));
  repl_ = std::make_unique<DevelopmentRepl>(*session_, Path("__repl__.vela"));
}

void DevelopmentReplTest::TearDown() {
  repl_.reset();
  session_.reset();
  files_.reset();
  vela::test::DirectoryTest::TearDown();
}

void DevelopmentReplTest::Execute(std::string_view input) {
  const auto result = repl_->Submit(input);
  EXPECT_FALSE(frontend::HasErrors(result.diagnostics))
      << frontend::DiagnosticsText(result.diagnostics);
  if (result.update) {
    EXPECT_FALSE(frontend::HasErrors(result.update->diagnostics))
        << frontend::DiagnosticsText(result.update->diagnostics);
  }
  EXPECT_EQ(result.status, SubmissionStatus::Executed)
      << (result.runtime_diagnostic ? result.runtime_diagnostic->message : "");
}

std::expected<DevelopmentResult, frontend::Diagnostics> DevelopmentReplTest::Reset() {
  return files_->Capture(*session_->snapshot()).transform([&](SourceUpdate update) {
    return repl_->Reset(std::move(update));
  });
}

void DevelopmentReplTest::ExpectReset(DevelopmentStatus status) {
  const auto reset = Reset();
  ASSERT_TRUE(reset) << frontend::DiagnosticsText(reset.error());
  ASSERT_EQ(reset->status, status);
}

std::expected<DevelopmentResult, frontend::Diagnostics> DevelopmentReplTest::UpdateFiles(
    DevelopmentOperation operation) {
  return files_->Capture(*session_->snapshot()).transform([&](SourceUpdate update) {
    return ApplyUpdate(*session_, std::move(update), operation);
  });
}

}  // namespace vela::compiler::test
