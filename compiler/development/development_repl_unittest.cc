#include "compiler/development/development_repl.h"

#include <string>

#include <gtest/gtest.h>

#include "compiler/bindgen/fixture/bindingfixture_bindings.h"
#include "compiler/development/development_session.h"
#include "compiler/repl/repl_source.h"
#include "compiler/test_support/development_test_support.h"
#include "runtime/heap/host_root.h"
#include "runtime/isolate/external_resources.h"
#include "runtime/isolate/isolate.h"
#include "runtime/value/value.h"

namespace vela::compiler {
namespace {

TEST(ReplSourceTest, ExternalFunctionsAcceptAnOmittedSemicolonAlongsideExternalClasses) {
  const ReplSource source;
  for (const auto* const input : {
           "external func answer(): int",
           "export external func answer(): int",
           "external class Counter = 'app.Counter' {}",
       }) {
    SCOPED_TRACE(input);
    const auto candidate = source.Prepare(input);
    ASSERT_TRUE(candidate);
    EXPECT_FALSE(candidate->entry.wrapper);
  }
}

using test::DevelopmentReplTest;

TEST_F(DevelopmentReplTest, StructDeclarationsAndArrayValuesPersistAcrossSubmissions) {
  Execute("struct Position { x: int; y: int; }");
  Execute("let point = Position(x: 10, y: 20)");
  Execute("let values = [point]");
  Execute("point.x = 99");
  Execute("values[0].x += 12");
  Execute("values[0].x + values[0].y");
  EXPECT_EQ(output().str(), "99\n22\n42\n");
}

TEST_F(DevelopmentReplTest, InteractiveCommentsDoNotClaimDiskBindingProducts) {
  Execute("// Binding generation: pasted\nconst answer = 42;");
  Execute("answer");
  EXPECT_EQ(output().str(), "42\n");
}

class NativeClassReplTest : public DevelopmentReplTest {
 protected:
  void SetUp() override {
    runtime::Isolate registrations;
    ASSERT_TRUE(bindings::module_bindingfixture::Register(registrations));
    DevelopmentReplTest::SetUp(registrations.FreezeBindings());
  }
};

TEST_F(NativeClassReplTest, NativeClassDeclarationsAndBoundMethodsUseTheRegisteredSnapshot) {
  // 交互声明与生成模块使用同一份完整公开契约。
  Execute(R"(external class Counter = 'fixture.Counter' {
    constructor(initial: int);
    add(delta: int): int;
    get value(): int;
    set value(value: int): void;
    Collect(delta: int): int;
    Text(text: string): string;
    Equal(other: Counter?): bool;
    static Shared(initial: int): Counter?;
    static Empty(): Counter?;
    Checked(fail: bool): int;
    static Echo(values: int[]): int[];
  })");
  Execute("let counter = Counter(initial: 40);");
  Execute("const bound = counter.Collect;");
  Execute("bound(2)");
  EXPECT_EQ(output().str(), "42\n");
}

TEST_F(DevelopmentReplTest, AppendsDeclarationsAndStatementsWithoutReplayingTheirEffects) {
  EXPECT_EQ(repl().Submit("func value(): int {").status, SubmissionStatus::Incomplete);
  EXPECT_TRUE(repl().source().empty());
  Execute("func initial(): int { print('once'); return 40; }");
  Execute("let value = initial();");
  Execute("value = value + 1;");
  Execute("value + 1");
  EXPECT_EQ(output().str(), "once\n42\n");
  const auto revision = session().snapshot()->revision;
  const auto history = std::string(repl().source());
  EXPECT_EQ(repl().Submit("func unfinished()").status, SubmissionStatus::Incomplete);
  EXPECT_EQ(repl().Submit("let value = 999;").status, SubmissionStatus::Rejected);
  EXPECT_EQ(session().snapshot()->revision, revision);
  EXPECT_EQ(repl().source(), history);
  Execute("value");
  Execute("print('done')");
  EXPECT_EQ(output().str(), "once\n42\n41\ndone\n");
}

TEST_F(DevelopmentReplTest, RetiredEntryClosuresKeepTheirCaptureLayoutsUnderGcPressure) {
  Execute("let saved: () => int = (): int => 0;");
  Execute("{ let n = 40; saved = (): int => { n = n + 1; return n; }; }");
  constexpr int kSubmissions = 12;
  Execute("saved();");
  Execute("saved();");
  session().isolate().heap().Collect();
  const auto steady = session().code_resources();
  for (int index = 2; index < kSubmissions; ++index) {
    Execute("saved();");
    session().isolate().heap().Collect();
    const auto current = session().code_resources();
    // 独立函数机器码可以来自较早批次；必须随编辑保持有界，而不是假定整映像重编译。
    EXPECT_EQ(current.batches, steady.batches);
    EXPECT_EQ(current.functions, steady.functions);
    EXPECT_EQ(current.images, 2U);
  }
  Execute("saved()");
  EXPECT_EQ(output().str(), "53\n");
}

TEST_F(DevelopmentReplTest, FileChangesRequireExplicitUpdatesAndPreserveCapturedFunctionVersions) {
  Write("library.vela", "export func answer(): int { return 42; }");
  Execute("import 'library.vela' as lib;");
  Execute("const saved = lib.answer;");
  Execute("lib.answer()");
  Write("library.vela", "export func answer(): int { return 100; }");
  Execute("lib.answer()");
  EXPECT_EQ(output().str(), "42\n42\n");
  const auto updated = UpdateFiles(DevelopmentOperation::HotReload);
  ASSERT_TRUE(updated);
  EXPECT_EQ(updated->status, DevelopmentStatus::Accepted);
  Execute("lib.answer()");
  Execute("saved()");
  EXPECT_EQ(output().str(), "42\n42\n100\n42\n");
}

TEST_F(DevelopmentReplTest, ReloadRetainsDeclarationsAndDoesNotRunHistoricalStatements) {
  Execute("let value = 40;");
  Execute("value = 42; print('statement');");
  const auto updated = UpdateFiles(DevelopmentOperation::Reload);
  ASSERT_TRUE(updated);
  EXPECT_EQ(updated->status, DevelopmentStatus::Accepted);
  EXPECT_EQ(session().generation(), 2U);
  Execute("value");
  EXPECT_EQ(output().str(), "statement\n40\n");
}

TEST_F(DevelopmentReplTest, ResetWaitsForHostRootsAndOnlyClearsAcceptedHistoryAfterSwitching) {
  Execute("let value = 42;");
  const auto source = std::string(repl().source());
  {
    const auto blocker = runtime::HostRoot::Create(session().isolate(), runtime::Value::Null());
    ASSERT_TRUE(blocker);
    const auto reset = Reset();
    ASSERT_TRUE(reset);
    ASSERT_EQ(reset->status, DevelopmentStatus::WaitingForRelease);
    ASSERT_EQ(reset->blockers.size(), 1U);
    EXPECT_EQ(reset->blockers.front().kind, runtime::ExternalResourceKind::Persistent);
    EXPECT_EQ(repl().source(), source);
    ASSERT_TRUE(session().Cancel());
    EXPECT_EQ(repl().source(), source);
    Execute("value");
  }
  ASSERT_NO_FATAL_FAILURE(ExpectAcceptedReset());
  EXPECT_TRUE(repl().source().empty());
  EXPECT_EQ(repl().Submit("value").status, SubmissionStatus::Rejected);
  Execute("let value = 100;");
  Execute("value");
  EXPECT_EQ(output().str(), "42\n100\n");
}

TEST_F(DevelopmentReplTest, ResetAcceptsAnExplicitDiskUpdateBeforeReimportingModules) {
  Write("library.vela", "export func answer(): int { return 42; }");
  Execute("import 'library.vela' as lib;");
  Write("library.vela", "export func answer(): int { return 100; }");
  Execute("lib.answer()");
  ASSERT_NO_FATAL_FAILURE(ExpectAcceptedReset());
  Execute("import 'library.vela' as lib;");
  Execute("lib.answer()");
  EXPECT_EQ(output().str(), "42\n100\n");
}

TEST_F(DevelopmentReplTest, RuntimeErrorsKeepCommittedDeclarationsAndCarrySubmissionLocations) {
  const auto failed = repl().Submit("let zero = 0;\nlet value = 1 / zero;");
  ASSERT_EQ(failed.status, SubmissionStatus::RuntimeError);
  const auto& diagnostic = failed.runtime_diagnostic;
  if (!diagnostic) {
    FAIL() << "runtime failure did not preserve its diagnostic";
  }
  const auto& trace = diagnostic->stack_trace;
  ASSERT_FALSE(trace.empty());
  const auto& frame = trace.front();
  EXPECT_EQ(frame.source_path, "<repl:1>");
  EXPECT_EQ(frame.line, 2U);
  Execute("zero = 1;");
  Execute("value");
  EXPECT_EQ(output().str(), "1\n");
}

}  // namespace
}  // namespace vela::compiler
