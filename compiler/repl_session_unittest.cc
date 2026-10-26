#include "compiler/repl_session.h"

#include <chrono>
#include <filesystem>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

#include "base/file.h"
#include "gtest/gtest.h"

#include "runtime/heap.h"

namespace vela::compiler {
namespace {

class ReplSessionTest : public testing::Test {
 protected:
  ReplSessionTest() : session_(std::filesystem::path(".")) {
    session_.isolate().SetOutput(output_);
  }

  void Execute(std::string_view text) {
    const auto result = session_.Submit(text);
    for (const auto& diagnostic : result.diagnostics) {
      ADD_FAILURE() << diagnostic.source_path << ':' << diagnostic.location.line << ' '
                    << diagnostic.message;
    }
    EXPECT_EQ(result.status, SubmissionStatus::Executed) << result.execution.message;
  }

  ReplSession& session() { return session_; }
  std::ostringstream& output() { return output_; }

 private:
  std::ostringstream output_;
  ReplSession session_;
};

TEST_F(ReplSessionTest, PersistsValuesAndInitializesOnceWithoutReplayingHistory) {
  Execute("func initial(): int { print(\"once\"); return 40; }");
  Execute("let value = initial();");
  Execute("value = value + 1;");
  Execute("value + 1");
  Execute("print(\"done\")");
  EXPECT_EQ(output().str(), "once\n42\ndone\n");
}

TEST_F(ReplSessionTest, ByteViewsAndMethodsRetainTheirOwnersAcrossSubmissions) {
  session().isolate().heap().SetGcThreshold(1);
  Execute("const bytes = ByteArray.from([0,0,0,0]);");
  Execute("const view = ByteData.view(bytes);");
  Execute("const write: (int,int) => void = view.setInt32;");
  Execute("const read: (int) => int = view.getInt32;");
  Execute("write(0,42);");
  session().isolate().heap().Collect();
  Execute("const views: ByteData[] = [view];");
  Execute("read(0)");
  Execute("bytes[3]");
  Execute("views[0].getInt32(0)");
  EXPECT_EQ(output().str(), "42\n42\n42\n");
}

TEST_F(ReplSessionTest, BuiltinMethodValuesKeepReceiversAndTargetsAcrossSubmissions) {
  session().isolate().heap().SetGcThreshold(1);
  Execute("const values = [20];");
  Execute("const append: (int) => int = values.push;");
  Execute("const copy: () => int[] = values.slice;");
  Execute("func unrelated(): int { return 100; }");
  session().isolate().heap().Collect();
  Execute("append(22);");
  Execute("copy().reduce((sum, value) => sum + value, 0)");
  EXPECT_EQ(output().str(), "42\n");
}

TEST_F(ReplSessionTest, ScalarIntrinsicsAndDatePartsPersistAcrossSubmissions) {
  session().isolate().heap().SetGcThreshold(1);
  Execute("const parse = int.parse;");
  Execute("const upper = 'straße'.toUpperCase;");
  Execute("const parts = Date.toUTC(-1);");
  Execute("const format = Date.toISOString;");
  Execute("func unrelated(): int { return 100; }");
  session().isolate().heap().Collect();
  Execute("parse('2a', 16)");
  Execute("upper()");
  Execute("parts.millisecond");
  Execute("format(Date.fromUTC(1970, 1, 1))");
  EXPECT_EQ(output().str(), "42\nSTRASSE\n999\n1970-01-01T00:00:00.000Z\n");
}

TEST_F(ReplSessionTest, ExistingClosuresKeepTargetsAndCapturesAfterNewFunctionsAndGlobals) {
  session().isolate().heap().SetGcThreshold(1);
  Execute(R"(func counter(): () => int {
    let value = 40;
    return (): int => { value = value + 1; return value; };
  })");
  Execute("const next = counter();");
  Execute("const direct = counter;");
  Execute("func inserted(): int { return 500; }");
  Execute("const added: string = \"new\";");
  session().isolate().heap().Collect();
  Execute("next()");
  Execute("next()");
  Execute("direct()()");
  EXPECT_EQ(output().str(), "41\n42\n41\n");
}

TEST_F(ReplSessionTest, RetiredEntryLambdasRemainCallableAndEntriesReuseOneSlot) {
  Execute("let saved: () => int = (): int => 0;");
  Execute("{ let n = 40; saved = (): int => { n = n + 1; return n; }; }");
  const auto count = session().function_count();
  constexpr int kSubmissions = 300;
  for (int index = 0; index < kSubmissions; ++index) {
    Execute("saved();");
  }
  EXPECT_EQ(session().function_count(), count);
  session().isolate().heap().Collect();
  Execute("saved()");
  EXPECT_EQ(output().str(), "341\n");
}

TEST_F(ReplSessionTest, KeepsClassArrayCollectionAndInterfaceIdentityForOldValues) {
  session().isolate().heap().SetGcThreshold(1);
  Execute("interface Readable { read(): int; }");
  Execute("class Box implements Readable { param value: int; read(): int { return this.value; } }");
  Execute("const box = Box(value: 42);");
  Execute("const items: Box[][] = [[box]];");
  Execute("const map: Map<string, Box> = Map<string, Box>();");
  Execute("map.set(\"key\", box);");
  Execute("const view: Readable = box;");
  Execute("interface Added { other(): int; }");
  session().isolate().heap().Collect();
  Execute("view.read()");
  Execute("items[0][0] == box");
  Execute("box is Box");
  Execute("map.get(\"key\") == box");
  Execute("const bound = view.read;");
  Execute("bound()");
  EXPECT_EQ(output().str(), "42\ntrue\ntrue\ntrue\n42\n");
}

TEST_F(ReplSessionTest, DistinguishesGenericLambdaInstantiationsWithSameSourcePosition) {
  Execute("func make<T>(value: T): () => T { return (): T => value; }");
  Execute("const number = make<int>(42);");
  Execute("const text = make<string>(\"ok\");");
  session().isolate().heap().Collect();
  Execute("number()");
  Execute("text()");
  EXPECT_EQ(output().str(), "42\nok\n");
}

TEST_F(ReplSessionTest, FailedCompilationRollsBackStateAndLayoutAllocations) {
  Execute("let value = 40;");
  const auto before = session().isolate().layout_counts();
  const auto old_source = std::string(session().source());
  std::string bad = "class Huge { ";
  constexpr int kUnencodableFieldCount = 257;
  for (int index = 0; index < kUnencodableFieldCount; ++index) {
    bad += "field" + std::to_string(index) + ": int = 0; ";
  }
  bad += "} func construct(): void { const value = Huge(); }";
  EXPECT_EQ(session().Submit(bad).status, SubmissionStatus::Rejected);
  EXPECT_EQ(session().isolate().layout_counts(), before);
  EXPECT_EQ(session().source(), old_source);
  EXPECT_EQ(session().Submit("let wrong: int = true;").status, SubmissionStatus::Rejected);
  Execute("let wrong: int = 2;");
  Execute("value + wrong");
  EXPECT_EQ(output().str(), "42\n");
}

TEST_F(ReplSessionTest, RuntimeErrorsKeepEarlierEffectsAndPermitInitializerRetry) {
  Execute("let divisor = 0;");
  EXPECT_EQ(session().Submit("let answer = 42 / divisor;").status, SubmissionStatus::RuntimeError);
  Execute("divisor = 1;");
  Execute("answer");
  const auto failed = session().Submit("divisor = 2; print(1 / 0);");
  EXPECT_EQ(failed.status, SubmissionStatus::RuntimeError);
  Execute("divisor");
  EXPECT_EQ(output().str(), "42\n2\n");
}

TEST_F(ReplSessionTest, MapsHistoricalFunctionsAndCurrentCallerToSubmissionLocations) {
  Execute("func fail(): int {\n  return 1 / 0;\n}");
  const auto failed = session().Submit("fail()");
  ASSERT_EQ(failed.status, SubmissionStatus::RuntimeError);
  ASSERT_EQ(failed.execution.stack_trace.size(), 2U);
  EXPECT_EQ(failed.execution.stack_trace.front().source_path, "<repl:1>");
  EXPECT_EQ(failed.execution.stack_trace.front().location.line, 2U);
  EXPECT_EQ(failed.execution.stack_trace.back().source_path, "<repl:2>");
  EXPECT_EQ(failed.execution.stack_trace.back().location.line, 1U);
}

TEST_F(ReplSessionTest, IncompleteInputAndResetHaveNoEffects) {
  EXPECT_EQ(session().Submit("func value(): int {").status, SubmissionStatus::Incomplete);
  EXPECT_TRUE(session().source().empty());
  Execute("const value = 42;");
  session().Reset();
  session().isolate().SetOutput(output());
  EXPECT_EQ(session().Submit("value").status, SubmissionStatus::Rejected);
  Execute("const value = 3;");
  Execute("value");
  EXPECT_EQ(output().str(), "3\n");
}

TEST_F(ReplSessionTest, SuspendedFunctionsShareGlobalsAcrossLaterSubmissions) {
  Execute("let result = 0;");
  Execute("const completer = Completer<int>();");
  Execute("async func task(): Future<void> { result = await completer.future; }");
  Execute("task();");
  Execute("let added = 1;");
  Execute("completer.complete(42);");
  while (session().isolate().scheduler().PumpOne()) {
  }
  Execute("result");
  EXPECT_EQ(output().str(), "42\n");
}

TEST_F(ReplSessionTest, TrailingCommentsAndBlockLambdaExpressionsKeepTheirSyntax) {
  Execute("let value = 40 // comment");
  Execute("value + 2 // comment");
  Execute("let operation: () => int = (): int => 0;");
  Execute("operation = (): int => { return value + 2; }");
  Execute("operation()");
  EXPECT_TRUE(output().str().starts_with("42\n"));
  EXPECT_TRUE(output().str().ends_with("42\n"));
}

class ReplModuleSessionTest : public testing::Test {
 protected:
  void SetUp() override {
    std::error_code error;
    const auto temporary = std::filesystem::temp_directory_path(error);
    ASSERT_FALSE(error);
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    directory_ = temporary / ("vela-repl-" + std::to_string(suffix));
    created_ = std::filesystem::create_directory(directory_, error);
    ASSERT_TRUE(created_);
    ASSERT_FALSE(error);
  }
  void TearDown() override {
    if (created_) {
      std::error_code error;
      std::filesystem::remove_all(directory_, error);
      EXPECT_FALSE(error);
    }
  }
  void Write(std::string_view name, std::string_view text) {
    ASSERT_TRUE(WriteFileAtomically(directory_ / name, std::as_bytes(std::span(text))));
  }
  static void Execute(ReplSession& session, std::string_view text) {
    EXPECT_EQ(session.Submit(text).status, SubmissionStatus::Executed);
  }
  [[nodiscard]] const std::filesystem::path& directory() const { return directory_; }

 private:
  std::filesystem::path directory_;
  bool created_ = false;
};

TEST_F(ReplModuleSessionTest, PinsAcceptedModulesAndKeepsBindingsWhenNewImportsChangeLoadOrder) {
  Write("leaf.vela",
        "export class Box { param value: int; } "
        "export func read(): int { return 40; }");
  Write("parent.vela",
        "import \"leaf.vela\" as leaf; "
        "export func get(): int { return leaf.read(); } "
        "export func capture(): () => int { return leaf.read; }");
  Write("added.vela", "export func get(): int { return 2; }");
  {
    ReplSession session(directory());
    std::ostringstream output;
    session.isolate().SetOutput(output);
    Execute(session, "import \"parent.vela\" as parent;");
    Execute(session, "const saved = parent.capture();");
    Write("leaf.vela", "export func read(): int { return 100; }");
    Execute(session, "import \"added.vela\" as added;");
    Execute(session, "saved() + added.get()");
    EXPECT_EQ(output.str(), "42\n");
    session.Reset();
    session.isolate().SetOutput(output);
    Execute(session, "import \"parent.vela\" as parent;");
    Execute(session, "parent.get()");
    EXPECT_EQ(output.str(), "42\n100\n");
  }
}

TEST_F(ReplModuleSessionTest, FailedCompilationDoesNotPinNewModuleSources) {
  Write("leaf.vela", "export func read(): int { return true; }");
  ReplSession session(directory());
  EXPECT_EQ(session.Submit("import \"leaf.vela\" as leaf;").status, SubmissionStatus::Rejected);
  Write("leaf.vela", "export func read(): int { return 42; }");
  Execute(session, "import \"leaf.vela\" as leaf;");
}

}  // namespace
}  // namespace vela::compiler
