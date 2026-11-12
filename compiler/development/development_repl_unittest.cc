#include "compiler/development/development_repl.h"

#include <string>

#include <gtest/gtest.h>

#include "compiler/bindgen/fixture/bindingfixture_bindings.h"
#include "compiler/development/development_session.h"
#include "compiler/repl/repl_source.h"
#include "compiler/test_support/development_test_support.h"
#include "runtime/heap/host_root.h"
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

TEST_F(DevelopmentReplTest, InteractiveCommentsDoNotClaimDiskBindingProducts) {
  Execute("// Binding generation: pasted\nconst answer = 42;");
  Execute("answer");
  EXPECT_EQ(output().str(), "42\n");
}

TEST_F(DevelopmentReplTest, BoundedGenericMethodsRetainApplicationsAfterRejectedSubmissions) {
  Execute("interface Reader<E> { read(): E; }");
  Execute("class Number implements Reader<int> { read(): int { return 41; } }");
  Execute("class Mapper { map<T extends Reader<E>, E>(value: T): E { return value.read(); } }");
  Execute("const mapper = Mapper();");
  Execute("const operation: (Number) => int = mapper.map;");
  const auto history = std::string(repl().source());
  EXPECT_EQ(repl().Submit("func unfinished<T extends Reader<").status,
            SubmissionStatus::Incomplete);
  EXPECT_EQ(repl().Submit("const invalid = mapper.map(42);").status, SubmissionStatus::Rejected);
  EXPECT_EQ(repl().source(), history);
  session().isolate().heap().Collect();
  Execute("operation(Number()) + 1");
  Execute("mapper.map(Number()) + 1");
  EXPECT_EQ(output().str(), "42\n42\n");
}

TEST_F(DevelopmentReplTest, NativeClassDeclarationsAndBoundMethodsUseTheRegisteredSnapshot) {
  runtime::Isolate registrations;
  ASSERT_TRUE(bindings::module_bindingfixture::Register(registrations));
  ASSERT_NO_FATAL_FAILURE(LoadSession(registrations.FreezeBindings()));
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
  EXPECT_EQ(output().str(), "once\n42\n41\n");
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
  {
    const auto blocker = runtime::HostRoot::Create(session().isolate(), runtime::Value::Null());
    ASSERT_TRUE(blocker);
    ASSERT_NO_FATAL_FAILURE(ExpectReset(DevelopmentStatus::WaitingForRelease));
    EXPECT_FALSE(repl().source().empty());
    ASSERT_TRUE(session().Cancel());
  }
  Execute("value");
  ASSERT_NO_FATAL_FAILURE(ExpectReset(DevelopmentStatus::Accepted));
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
  ASSERT_NO_FATAL_FAILURE(ExpectReset(DevelopmentStatus::Accepted));
  Execute("import 'library.vela' as lib;");
  Execute("lib.answer()");
  EXPECT_EQ(output().str(), "42\n100\n");
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
  EXPECT_EQ(output().str(), "1\n");
}

}  // namespace
}  // namespace vela::compiler
