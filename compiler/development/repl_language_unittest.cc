#include "compiler/development/development_repl.h"

#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>

#include "compiler/development/development_session.h"
#include "compiler/repl/repl_source.h"
#include "compiler/test_support/development_test_support.h"
#include "compiler/test_support/program_test_support.h"
#include "gtest/gtest.h"

#include "runtime/async/future.h"
#include "runtime/heap/heap.h"
#include "runtime/heap/host_root.h"
#include "runtime/isolate/external_resources.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_function.h"
#include "runtime/value/value.h"

namespace vela::compiler {
namespace {

std::int64_t PersistentHostAnswer() noexcept {
  constexpr std::int64_t kAnswer = 42;
  return kAnswer;
}

class ReplLanguageTest : public test::DevelopmentReplTest {
 protected:
  void SetUp() override {
    runtime::Isolate bindings;
    ASSERT_TRUE(bindings.RegisterNative(
        runtime::NativeFunction::Bind<PersistentHostAnswer>("host.answer")));
    test::DevelopmentReplTest::SetUp(bindings.FreezeBindings());
  }
};

TEST_F(ReplLanguageTest, GlobalReflectionDescriptorsFollowTheReplPublication) {
  Execute("class Box { value: int = 42; }");
  Execute("const info = Reflect.typeInfo<Box>();");
  Execute("info.name");
  Execute("info.isRegistered");
  Execute("const before = Reflect.revision();");
  Execute("Reflect.revision() > before");
  Execute(
      "func shadow(): int { const Reflect = (value: int): int => value + 1; return Reflect(41); }");
  Execute("shadow()");
  EXPECT_EQ(output().str(), "Box\nfalse\ntrue\n42\n");
  session().isolate().heap().Collect();
  EXPECT_EQ(session().code_resources().images, 1U);
}

TEST_F(ReplLanguageTest, BoundedGenericMethodsAndInterfacesPersistAcrossSubmissions) {
  Execute("interface Reader<E> { read(): E; }");
  Execute("class Number implements Reader<int> { read(): int { return 41; } }");
  Execute("class Mapper { map<T extends Reader<E>, E>(value: T): E { return value.read(); } }");
  Execute("func read<T extends Reader<E>, E>(value: T): E { return value.read(); }");
  Execute("const mapper = Mapper();");
  Execute("const operation: (Number) => int = mapper.map;");
  EXPECT_EQ(repl().Submit("func unfinished<T extends Reader<").status,
            SubmissionStatus::Incomplete);
  EXPECT_EQ(repl().Submit("const invalid = mapper.map(42);").status, SubmissionStatus::Rejected);
  session().isolate().heap().Collect();
  Execute("operation(Number()) + 1");
  Execute("read(Number()) + 1");
  EXPECT_EQ(output().str(), "42\n42\n");
}

TEST_F(ReplLanguageTest, MapDeclarationsWithoutSemicolonsPersistAcrossMultilineSubmissions) {
  EXPECT_EQ(repl().Submit("const values: Map<string, int> = {'a':").status,
            SubmissionStatus::Incomplete);
  Execute("const values: Map<string, int> = {'a': 40, 'b': 2}");
  Execute("const empty: Map<string, int> = {}");
  Execute("const make: () => Map<string, int> = () => ({'answer': 42})");
  session().isolate().heap().Collect();
  Execute("(values.get('a') ?? 0) + (values.get('b') ?? 0)");
  Execute("make().get('answer')");
  EXPECT_EQ(output().str(), "42\n42\n");
}

TEST_F(ReplLanguageTest, PersistsValuesAndInitializesOnceWithoutReplayingHistory) {
  Execute("func initial(): int { print(\"once\"); return 40; }");
  Execute("let value = initial();");
  Execute("value = value + 1;");
  Execute("value + 1");
  Execute("print(\"done\")");
  EXPECT_EQ(output().str(), "once\n42\ndone\n");
}

TEST_F(ReplLanguageTest, LambdasInLoopIterablesKeepTheirTargetsAcrossSubmissions) {
  Execute("const callbacks: (() => int)[] = [];");
  Execute("for (const callback in [(): int => 20, (): int => 22]) { callbacks.push(callback); }");
  Execute("for (const callback in [(): int => 100, (): int => 200]) { callbacks.push(callback); }");
  session().isolate().heap().Collect();
  Execute("callbacks[0]() + callbacks[1]()");
  Execute("callbacks[2]() + callbacks[3]()");
  EXPECT_EQ(output().str(), "42\n300\n");
}

TEST_F(ReplLanguageTest, DynamicJsonValuesAndBoundMethodsSurviveGcAndLaterSubmissions) {
  Execute(R"(const root: dynamic = JSON.parse('{"name":"Vela","items":[40,2]}');)");
  Execute("const read = root.get;");
  session().isolate().heap().Collect();
  Execute("const name: string = read('name');");
  Execute("const values: dynamic[] = root.get('items');");
  Execute("values[0] + values[1]");
  Execute("root.set('name', name.toUpperCase());");
  session().isolate().heap().Collect();
  Execute("JSON.stringify(root)");
  EXPECT_EQ(output().str(), "42\n{\"name\":\"VELA\",\"items\":[40,2]}\n");
}

TEST_F(ReplLanguageTest, TypedJSONResultsAndSpecializedFunctionsSurviveLaterSubmissions) {
  Execute("class User { name: string; age: int = 42; }");
  Execute("const parse: (string) => User = JSON.parse<User>;");
  Execute(R"(const result = JSON.safeParse<User>('{"name":"Vela"}');)");
  session().isolate().heap().Collect();
  Execute("result.value.age");
  Execute(R"(parse('{"name":"Other","age":43}').age)");
  Execute("const failure = JSON.safeParse<User>('{}');");
  session().isolate().heap().Collect();
  Execute("failure.error.path");
  EXPECT_EQ(output().str(), "42\n43\n$.name\n");
}

TEST_F(ReplLanguageTest, ByteViewsAndMethodsRetainTheirOwnersAcrossSubmissions) {
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

TEST_F(ReplLanguageTest, RegExpCapturesAndBoundMethodsRetainOwnersAcrossSubmissions) {
  Execute("const re = RegExp('(?<digits>[0-9]+)');");
  Execute("const found = re.firstMatch('😀42');");
  Execute("const all: (string) => RegExpMatch[] = re.allMatches;");
  session().isolate().heap().Collect();
  Execute("found?.namedGroup('digits')");
  Execute("all('1 2').length");
  Execute("found?.start");
  EXPECT_EQ(output().str(), "42\n2\n1\n");
}

TEST_F(ReplLanguageTest, BuiltinMethodValuesKeepReceiversAndTargetsAcrossSubmissions) {
  Execute("const values = [20];");
  Execute("const append: (int) => int = values.push;");
  Execute("const copy: () => int[] = values.slice;");
  Execute("func unrelated(): int { return 100; }");
  session().isolate().heap().Collect();
  Execute("append(22);");
  Execute("copy().reduce((sum, value) => sum + value, 0)");
  EXPECT_EQ(output().str(), "42\n");
}

TEST_F(ReplLanguageTest, ScalarIntrinsicsAndDatePartsPersistAcrossSubmissions) {
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

TEST_F(ReplLanguageTest, ExistingClosuresKeepTargetsAndCapturesAfterNewFunctionsAndGlobals) {
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

TEST_F(ReplLanguageTest, RetiredEntryLambdasRemainCallableWithoutAccumulatingCurrentEntries) {
  Execute("let saved: () => int = (): int => 0;");
  Execute("{ let n = 40; saved = (): int => { n = n + 1; return n; }; }");
  // 先退休创建闭包的交互入口，再比较后续同形提交的当前程序规模。
  Execute("saved();");
  const auto count = session().snapshot()->program.functions.size();
  constexpr int kSubmissions = 300;
  for (int index = 1; index < kSubmissions; ++index) {
    Execute("saved();");
  }
  EXPECT_EQ(session().snapshot()->program.functions.size(), count);
  session().isolate().heap().Collect();
  Execute("saved()");
  EXPECT_EQ(output().str(), "341\n");
}

TEST_F(ReplLanguageTest, ImplicitConstructorsRemainAvailableAfterTheCallingEntryRetires) {
  Execute("class Box { value: int = 42; }");
  Execute("Box().value");
  const auto functions = session().snapshot()->program.functions.size();
  Execute("print(1);");
  EXPECT_EQ(session().snapshot()->program.functions.size(), functions);
  Execute("Box(value: 40).value + 2");
  EXPECT_EQ(output().str(), "42\n1\n42\n");
}

TEST_F(ReplLanguageTest, KeepsClassArrayCollectionAndInterfaceIdentityForOldValues) {
  Execute("interface Readable { read(): int; }");
  Execute("class Box implements Readable { value: int; read(): int { return this.value; } }");
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

TEST_F(ReplLanguageTest, DistinguishesGenericLambdaInstantiationsWithSameSourcePosition) {
  Execute("func make<T>(value: T): () => T { return (): T => value; }");
  Execute("const number = make<int>(42);");
  Execute("const text = make<string>(\"ok\");");
  session().isolate().heap().Collect();
  Execute("number()");
  Execute("text()");
  EXPECT_EQ(output().str(), "42\nok\n");
}

TEST_F(ReplLanguageTest, FailedCompilationRollsBackStateAndLayoutAllocations) {
  Execute("let value = 40;");
  const auto before = session().isolate().layout_counts();
  const auto old_source = std::string(repl().source());
  const std::string bad =
      "class Pending { field: int = 0; } "
      "func construct(): int { const value = Pending(); return false; }";
  EXPECT_EQ(repl().Submit(bad).status, SubmissionStatus::Rejected);
  EXPECT_EQ(session().isolate().layout_counts(), before);
  EXPECT_EQ(repl().source(), old_source);
  EXPECT_EQ(repl().Submit("let wrong: int = true;").status, SubmissionStatus::Rejected);
  Execute("let wrong: int = 2;");
  Execute("value + wrong");
  EXPECT_EQ(output().str(), "42\n");
}

TEST_F(ReplLanguageTest, RuntimeErrorsKeepEarlierEffectsAndPermitInitializerRetry) {
  Execute("let divisor = 0;");
  EXPECT_EQ(repl().Submit("let answer = 42 / divisor;").status, SubmissionStatus::RuntimeError);
  Execute("divisor = 1;");
  Execute("answer");
  const auto failed = repl().Submit("divisor = 2; print(1 / 0);");
  EXPECT_EQ(failed.status, SubmissionStatus::RuntimeError);
  Execute("divisor");
  EXPECT_EQ(output().str(), "42\n2\n");
}

TEST_F(ReplLanguageTest, MapsHistoricalFunctionsAndCurrentCallerToSubmissionLocations) {
  Execute("func fail(): int {\n  return 1 / 0;\n}");
  const auto failed = repl().Submit("fail()");
  ASSERT_EQ(failed.status, SubmissionStatus::RuntimeError);
  if (!failed.runtime_diagnostic) {
    FAIL() << "runtime failure did not include a diagnostic";
  }
  const auto& trace = failed.runtime_diagnostic->stack_trace;
  ASSERT_EQ(trace.size(), 2U);
  EXPECT_EQ(trace.front().source_path, "<repl:1>");
  EXPECT_EQ(trace.front().line, 2U);
  EXPECT_EQ(trace.back().source_path, "<repl:2>");
  EXPECT_EQ(trace.back().line, 1U);
}

TEST_F(ReplLanguageTest, IncompleteInputAndResetHaveNoEffects) {
  EXPECT_EQ(repl().Submit("func value(): int {").status, SubmissionStatus::Incomplete);
  EXPECT_TRUE(repl().source().empty());
  Execute("const value = 42;");
  ASSERT_NO_FATAL_FAILURE(ExpectReset(DevelopmentStatus::Accepted));
  session().isolate().SetOutput(output());
  EXPECT_EQ(repl().Submit("value").status, SubmissionStatus::Rejected);
  Execute("const value = 3;");
  Execute("value");
  EXPECT_EQ(output().str(), "3\n");
}

TEST_F(ReplLanguageTest, SourceAttributesRegisterInteractiveDeclarationsWithoutChangingArrays) {
  Execute("class Button extends Reflect { label: string = 'run'; }");
  Execute(
      "[Reflect] class Counter { value: int = 42; [Button] read(): int { return this.value; } }");
  Execute("Reflect.findMethods<Button>()[0].attribute.label");
  Execute("Reflect.typeInfo<Counter>().getFields().length");
  Execute("[40, 2][0] + 2");
  EXPECT_EQ(output().str(), "run\n1\n42\n");
}

TEST_F(ReplLanguageTest, ResetRejectsHostRootsAndKeepsTheCurrentSessionRunning) {
  Execute("let value = 40;");
  const auto source = std::string(repl().source());
  {
    const auto root_storage =
        runtime::HostRoot::Create(session().isolate(), runtime::Value::Null());
    ASSERT_TRUE(root_storage);
    const auto reset = Reset();
    ASSERT_TRUE(reset);
    ASSERT_EQ(reset->status, DevelopmentStatus::WaitingForRelease);
    ASSERT_EQ(reset->blockers.size(), 1U);
    EXPECT_EQ(reset->blockers.front().kind, runtime::ExternalResourceKind::Persistent);
    ASSERT_TRUE(session().Cancel());
    EXPECT_EQ(repl().source(), source);
    Execute("value + 2");
  }
  ASSERT_NO_FATAL_FAILURE(ExpectReset(DevelopmentStatus::Accepted));
  EXPECT_TRUE(repl().source().empty());
  EXPECT_EQ(output().str(), "42\n");
}

TEST_F(ReplLanguageTest, ExternalNativeInstancesBlockResetEvenWithoutGlobals) {
  auto compiled = test::CompileSource("func answer(): int { return 42; }", session().isolate());
  ASSERT_TRUE(compiled.Succeeded());
  const auto refused = Reset();
  ASSERT_TRUE(refused);
  ASSERT_EQ(refused->status, DevelopmentStatus::WaitingForRelease);
  ASSERT_EQ(refused->blockers.size(), 1U);
  EXPECT_EQ(refused->blockers.front().kind, runtime::ExternalResourceKind::NativeInstance);
  ASSERT_TRUE(session().Cancel());
  compiled.instance.reset();
  ASSERT_NO_FATAL_FAILURE(ExpectReset(DevelopmentStatus::Accepted));
}

TEST_F(ReplLanguageTest, ResetStopsSuspendedTimersWithoutTreatingInternalRootsAsHostHandles) {
  Execute("async func wait(): Future<void> { await sleep(10000); }");
  Execute("const pending = wait();");
  EXPECT_EQ(session().isolate().scheduler().suspended_frame_count(), 1U);
  ASSERT_NO_FATAL_FAILURE(ExpectReset(DevelopmentStatus::Accepted));
  EXPECT_EQ(session().isolate().scheduler().suspended_frame_count(), 0U);
  EXPECT_TRUE(session().isolate().ReloadBlockers().empty());
}

TEST_F(ReplLanguageTest, ResetPreservesTheFixedNativeBindingSnapshot) {
  ASSERT_NE(session().isolate().FindNative("host.answer"), nullptr);
  Execute("external func answer(): int = 'host.answer';");
  Execute("answer()");
  const auto refused = session().isolate().RegisterNative(
      runtime::NativeFunction::Bind<PersistentHostAnswer>("host.other"));
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error(), runtime::NativeRegistrationError::BindingsFrozen);
  ASSERT_NO_FATAL_FAILURE(ExpectReset(DevelopmentStatus::Accepted));
  session().isolate().SetOutput(output());
  Execute("external func answer(): int = 'host.answer';");
  Execute("answer()");
  EXPECT_EQ(output().str(), "42\n42\n");
}

TEST_F(ReplLanguageTest, SuspendedFunctionsShareGlobalsAcrossLaterSubmissions) {
  Execute("let result = 0;");
  Execute("const completer = Completer<int>();");
  Execute("async func task(): Future<void> { result = await completer.future; }");
  Execute("task();");
  Execute("let added = 1;");
  Execute("completer.complete(42);");
  while (session().isolate().scheduler().PumpOne() == runtime::FuturePumpStep::Executed) {
  }
  Execute("result");
  EXPECT_EQ(output().str(), "42\n");
}

TEST_F(ReplLanguageTest, TrailingCommentsAndBlockLambdaExpressionsKeepTheirSyntax) {
  Execute("let value = 40 // comment");
  Execute("value + 2 // comment");
  Execute("let operation: () => int = (): int => 0;");
  Execute("operation = (): int => { return value + 2; }");
  Execute("operation()");
  EXPECT_TRUE(output().str().starts_with("42\n"));
  EXPECT_TRUE(output().str().ends_with("42\n"));
}

TEST_F(ReplLanguageTest, PinsAcceptedModulesAndKeepsBindingsWhenNewImportsChangeLoadOrder) {
  Write("leaf.vela",
        "export class Box { value: int; } "
        "export func read(): int { return 40; }");
  Write("parent.vela",
        "import \"leaf.vela\" as leaf; "
        "export func get(): int { return leaf.read(); } "
        "export func capture(): () => int { return leaf.read; }");
  Write("added.vela", "export func get(): int { return 2; }");
  {
    session().isolate().SetOutput(output());
    Execute("import \"parent.vela\" as parent;");
    Execute("const saved = parent.capture();");
    Write("leaf.vela", "export func read(): int { return 100; }");
    Execute("import \"added.vela\" as added;");
    Execute("saved() + added.get()");
    EXPECT_EQ(output().str(), "42\n");
    ASSERT_NO_FATAL_FAILURE(ExpectReset(DevelopmentStatus::Accepted));
    session().isolate().SetOutput(output());
    Execute("import \"parent.vela\" as parent;");
    Execute("parent.get()");
    EXPECT_EQ(output().str(), "42\n100\n");
  }
}

TEST_F(ReplLanguageTest, FailedCompilationDoesNotPinNewModuleSources) {
  Write("leaf.vela", "export func read(): int { return true; }");
  EXPECT_EQ(repl().Submit("import \"leaf.vela\" as leaf;").status, SubmissionStatus::Rejected);
  Write("leaf.vela", "export func read(): int { return 42; }");
  Execute("import \"leaf.vela\" as leaf;");
}

}  // namespace
}  // namespace vela::compiler
