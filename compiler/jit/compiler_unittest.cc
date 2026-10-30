#include "compiler/jit/compiler.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <semaphore>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>

#include <gtest/gtest.h>

#include "base/declaration.h"
#include "base/execution_status.h"
#include "base/native_program_mode.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/source_session.h"
#include "runtime/future.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/native_context.h"
#include "runtime/native_dispatch.h"
#include "runtime/native_function.h"
#include "runtime/native_image.h"
#include "runtime/native_instance.h"
#include "runtime/rooted_value.h"
#include "runtime/value.h"

namespace vela::compiler::jit {
namespace {

std::string DiagnosticsText(const frontend::Diagnostics& diagnostics) {
  std::string text;
  for (const auto& diagnostic : diagnostics) {
    text += diagnostic.source_path + ':' + std::to_string(diagnostic.location.line) + ": " +
            diagnostic.message + '\n';
  }
  return text;
}

std::expected<CompiledImage, frontend::Diagnostics> CompileText(
    Compiler& compiler,
    std::string source,
    NativeCodeTier tier = NativeCodeTier::Baseline) {
  SourceSession session("main.vela");
  const auto prepared = session.Prepare(
      {.changes = {{.path = "main.vela", .text = std::move(source)}}, .added_roots = {}});
  if (!prepared) {
    return std::unexpected(prepared.error().diagnostics);
  }
  return compiler.Compile(prepared->snapshot().program, {.tier = tier, .preserve_ir = true});
}

TEST(JitCompilerTest, MaterializesTheCompleteImageBeforePublishingNamedEntries) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
  const auto compiled = CompileText(**compiler,
                                    "func twice(value: int): int { return value * 2; } "
                                    "export func answer(): int { return twice(21); }");
  ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
  EXPECT_EQ((*compiler)->resource_counts().batches, 1U);
  EXPECT_NE(compiled->llvm_ir.find("@vela_native_v3_call_declaration"), std::string::npos);
  const auto functions = compiled->image->functions.values();
  const auto target = std::ranges::find(functions, "answer",
                                        [](const auto& function) { return function.name.text(); });
  ASSERT_NE(target, functions.end());
  EXPECT_EQ(target->identity.revision, 1U);
  runtime::Isolate isolate(1);
  auto publication = isolate.native_dispatch().Prepare(0, compiled->image);
  ASSERT_TRUE(publication) << publication.error().message;
  ASSERT_TRUE(isolate.native_dispatch().Publish(std::move(*publication)));
  const auto entry = isolate.native_dispatch().FindEntry("main.vela", "answer");
  ASSERT_TRUE(entry);
  const auto result = entry->Invoke({});
  ASSERT_TRUE(result);
  EXPECT_EQ(result->AsInteger(), 42);
}

TEST(JitCompilerTest, InferredGenericsExecuteInBothNativeTiers) {
  for (const auto tier : {NativeCodeTier::Baseline, NativeCodeTier::Optimized}) {
    auto compiler = Compiler::Create();
    ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
    const auto compiled = CompileText(**compiler, R"(
func identity<T>(value: T): T { return value; }
class Box<T> { value: T; }
export func answer(): int {
  const operation: (int) => int = identity;
  const box = Box(value: identity(40));
  return box.value + operation(2);
}
)",
                                      tier);
    ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
    runtime::Isolate isolate(1);
    auto publication = isolate.native_dispatch().Prepare(0, compiled->image);
    ASSERT_TRUE(publication) << publication.error().message;
    ASSERT_TRUE(isolate.native_dispatch().Publish(std::move(*publication)));
    const auto entry = isolate.native_dispatch().FindEntry("main.vela", "answer");
    ASSERT_TRUE(entry);
    const auto result = entry->Invoke({});
    ASSERT_TRUE(result);
    EXPECT_EQ(result->AsInteger(), 42);
  }
}

TEST(JitCompilerTest, VirtualClassFieldsRemainRootedAcrossUnrelatedAllocations) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
  const auto compiled = CompileText(**compiler, R"(
class Point { x: int; text: string; }
export func read(): int {
  const point = Point(x: 40, text: 'a' + 'b');
  const values = ['unrelated'];
  return point.x + point.text.length + values.length - 1;
}
)",
                                    NativeCodeTier::Optimized);
  ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
  const auto functions = compiled->image->functions.values();
  const auto found = std::ranges::find(functions, "read",
                                       [](const auto& function) { return function.name.text(); });
  ASSERT_NE(found, functions.end());
  const auto index = static_cast<std::size_t>(found - functions.begin());
  EXPECT_EQ(compiled->function_metrics.at(index).eliminated_allocations, 1U);
  EXPECT_NE(compiled->llvm_ir.find("_virtual_"), std::string::npos);
  runtime::Isolate isolate(1);
  auto publication = isolate.native_dispatch().Prepare(0, compiled->image);
  ASSERT_TRUE(publication) << publication.error().message;
  ASSERT_TRUE(isolate.native_dispatch().Publish(std::move(*publication)));
  const auto entry = isolate.native_dispatch().FindEntry("main.vela", "read");
  ASSERT_TRUE(entry);
  const auto result = entry->Invoke({});
  ASSERT_TRUE(result);
  EXPECT_EQ(result->AsInteger(), 42);
}

TEST(JitCompilerTest, AsyncResumptionKeepsItsMachineCodeAfterTheCompilerIsDestroyed) {
  runtime::Isolate isolate(1);
  {
    auto compiler = Compiler::Create();
    ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
    const auto compiled =
        CompileText(**compiler,
                    "async func later(): Future<int> { await sleep(1); return 42; } "
                    "export func start(): Future<int> { return later(); }");
    ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
    auto prepared = isolate.native_dispatch().Prepare(0, compiled->image);
    ASSERT_TRUE(prepared) << prepared.error().message;
    ASSERT_TRUE(isolate.native_dispatch().Publish(std::move(*prepared)));
  }
  const auto entry = isolate.native_dispatch().FindEntry("main.vela", "start");
  ASSERT_TRUE(entry);
  const auto started = entry->Invoke({});
  ASSERT_TRUE(started);
  const runtime::RootedValue root(isolate, *started);
  auto* future = runtime::Heap::GetFuture(*root.value().AsObject());
  ASSERT_NE(future, nullptr);
  ASSERT_TRUE(isolate.scheduler().PumpUntil(*future, runtime::FuturePumpMode::WaitForHost));
  const auto completed = future->value();
  if (!completed) {
    FAIL() << "Future did not complete with a value";
  }
  EXPECT_EQ(completed->AsInteger(), 42);
}

TEST(JitCompilerTest, VirtualObjectsEndBeforeAwaitAndLiveObjectsUseHeapContinuationState) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
  const auto compiled = CompileText(**compiler, R"(
class Box { value: int; text: string; }
export async func dead(): Future<int> {
  const box = Box(value: 40, text: 'a' + 'b');
  const result = box.value + box.text.length;
  await sleep(1);
  return result;
}
export async func live(): Future<int> {
  const box = Box(value: 40, text: 'a' + 'b');
  await sleep(1);
  return box.value + box.text.length;
}
)",
                                    NativeCodeTier::Optimized);
  ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
  runtime::Isolate isolate(1);
  auto publication = isolate.native_dispatch().Prepare(0, compiled->image);
  ASSERT_TRUE(publication) << publication.error().message;
  ASSERT_TRUE(isolate.native_dispatch().Publish(std::move(*publication)));
  const auto functions = compiled->image->functions.values();
  for (const auto* const name : {"dead", "live"}) {
    const auto found = std::ranges::find(functions, name,
                                         [](const auto& function) { return function.name.text(); });
    ASSERT_NE(found, functions.end());
    const auto& metrics =
        compiled->function_metrics.at(static_cast<std::size_t>(found - functions.begin()));
    EXPECT_EQ(metrics.eliminated_allocations, std::string_view(name) == "dead" ? 1U : 0U);
    EXPECT_EQ(metrics.retained_allocations, std::string_view(name) == "live" ? 1U : 0U);
    const auto entry = isolate.native_dispatch().FindEntry("main.vela", name);
    ASSERT_TRUE(entry);
    const auto started = entry->Invoke({});
    ASSERT_TRUE(started);
    const runtime::RootedValue root(isolate, *started);
    auto* future = runtime::Heap::GetFuture(*root.value().AsObject());
    ASSERT_NE(future, nullptr);
    isolate.heap().Collect();
    ASSERT_TRUE(isolate.scheduler().PumpUntil(*future, runtime::FuturePumpMode::WaitForHost));
    const auto completed = future->value();
    if (completed) {
      EXPECT_EQ(completed->AsInteger(), 42);
    } else {
      FAIL() << "Future did not complete with a value";
    }
  }
  isolate.heap().Collect();
  EXPECT_EQ(isolate.scheduler().suspended_frame_count(), 0U);
}

TEST(JitCompilerTest, ComparingVirtualAndOverlappingLoopInstancesPreservesFields) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
  const std::string source = R"(
class Box { value: int; ignored: int; }
export func read(): int {
  const first = Box(value: 14, ignored: 0);
  let previous = Box(value: 0, ignored: 0);
  let sum = 0;
  for (let index = 0; index < 3; index++) {
    const current = Box(value: index, ignored: 0);
    sum += previous.ignored;
    sum += first != current ? first.value : 0;
    previous = current;
  }
  return sum;
}
)";
  for (const auto tier : {NativeCodeTier::Baseline, NativeCodeTier::Optimized}) {
    const auto compiled = CompileText(**compiler, source, tier);
    ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
    runtime::Isolate isolate(1);
    auto publication = isolate.native_dispatch().Prepare(0, compiled->image);
    ASSERT_TRUE(publication) << publication.error().message;
    ASSERT_TRUE(isolate.native_dispatch().Publish(std::move(*publication)));
    const auto entry = isolate.native_dispatch().FindEntry("main.vela", "read");
    ASSERT_TRUE(entry);
    const auto result = entry->Invoke({});
    ASSERT_TRUE(result) << ExecutionStatusName(result.error());
    EXPECT_EQ(result->AsInteger(), 42);
  }
}

TEST(JitCompilerTest, DiscardedCompiledCandidatesRetireTheirOrcResources) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
  {
    const auto compiled = CompileText(**compiler, "export func answer(): int { return 42; }");
    ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
    EXPECT_EQ((*compiler)->resource_counts().batches, 1U);
    EXPECT_GT((*compiler)->resource_counts().functions, 0U);
  }
  EXPECT_EQ((*compiler)->resource_counts().batches, 0U);
  EXPECT_EQ((*compiler)->resource_counts().functions, 0U);
}

TEST(JitCompilerTest, CapturedMachineCodeKeepsItsBodyAndCallsTheLatestNamedImplementation) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
  SourceSession source("main.vela");
  runtime::Isolate isolate(1);
  auto& dispatch = isolate.native_dispatch();
  const auto first = source.Prepare({
      .changes =
          {
              {
                  .path = "main.vela",
                  .text = "export func answer(): int { return 40; } "
                          "export func caller(): int { return answer(); }",
              },
          },
      .added_roots = {},
  });
  ASSERT_TRUE(first) << DiagnosticsText(first.error().diagnostics);
  {
    const auto compiled = (*compiler)->Compile(first->snapshot().program);
    ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
    auto prepared = dispatch.Prepare(0, compiled->image);
    ASSERT_TRUE(prepared) << prepared.error().message;
    ASSERT_TRUE(dispatch.Publish(std::move(*prepared)));
    ASSERT_TRUE(source.Commit(*first));
  }
  const auto answer = dispatch.FindEntry("main.vela", "answer");
  const auto caller = dispatch.FindEntry("main.vela", "caller");
  ASSERT_TRUE(answer);
  ASSERT_TRUE(caller);
  std::optional<runtime::NativeVersionEntry> old_answer = *answer->Capture();
  std::optional<runtime::NativeVersionEntry> old_caller = *caller->Capture();
  const auto next = source.Prepare({
      .base_revision = 1,
      .changes =
          {
              {
                  .path = "main.vela",
                  .text = "export func answer(): int { return 42; } "
                          "export func caller(): int { return answer() + 100; }",
              },
          },
      .added_roots = {},
  });
  ASSERT_TRUE(next) << DiagnosticsText(next.error().diagnostics);
  {
    const auto compiled = (*compiler)->Compile(next->snapshot().program);
    ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
    auto prepared = dispatch.Prepare(1, compiled->image);
    ASSERT_TRUE(prepared) << prepared.error().message;
    ASSERT_TRUE(dispatch.Publish(std::move(*prepared)));
    ASSERT_TRUE(source.Commit(*next));
  }
  EXPECT_EQ(answer->Invoke({})->AsInteger(), 42);
  EXPECT_EQ(caller->Invoke({})->AsInteger(), 142);
  EXPECT_EQ(old_answer->Invoke({})->AsInteger(), 40);
  EXPECT_EQ(old_caller->Invoke({})->AsInteger(), 42);
  isolate.heap().Collect();
  EXPECT_EQ((*compiler)->resource_counts().batches, 2U);
  old_answer.reset();
  old_caller.reset();
  isolate.heap().Collect();
  EXPECT_EQ((*compiler)->resource_counts().batches, 1U);
}

TEST(JitCompilerTest, GlobalAndStaticSlotsAreSharedByOldAndNewCodeAndPendingCandidates) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
  SourceSession source("main.vela");
  runtime::Isolate isolate(1);
  auto& dispatch = isolate.native_dispatch();
  const auto first = source.Prepare({
      .changes =
          {
              {
                  .path = "main.vela",
                  .text = "let score = 10; const title = 'old'; let cold = 20; "
                          "class Counter { static value: int = 4; } "
                          "export func answer(): int { "
                          "return score + Counter.value + (title == 'old' ? 1 : 1000); } "
                          "export func edit(): void { score = 42; Counter.value = 7; } "
                          "export func bump(): int { score++; return score; } "
                          "export func readCold(): int { return cold; }",
              },
          },
      .added_roots = {},
  });
  ASSERT_TRUE(first) << DiagnosticsText(first.error().diagnostics);
  {
    const auto compiled = (*compiler)->Compile(first->snapshot().program);
    ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
    auto prepared = dispatch.Prepare(0, compiled->image);
    ASSERT_TRUE(prepared) << prepared.error().message;
    ASSERT_TRUE(dispatch.Publish(std::move(*prepared)));
    ASSERT_TRUE(source.Commit(*first));
  }
  const auto answer = dispatch.FindEntry("main.vela", "answer");
  const auto edit = dispatch.FindEntry("main.vela", "edit");
  const auto bump = dispatch.FindEntry("main.vela", "bump");
  const auto cold = dispatch.FindEntry("main.vela", "readCold");
  ASSERT_TRUE(answer);
  ASSERT_TRUE(edit);
  ASSERT_TRUE(bump);
  ASSERT_TRUE(cold);
  const auto old_answer = answer->Capture();
  const auto old_bump = bump->Capture();
  const auto old_cold = cold->Capture();
  ASSERT_TRUE(old_answer);
  ASSERT_TRUE(old_bump);
  ASSERT_TRUE(old_cold);
  const auto initial = answer->Invoke({});
  ASSERT_TRUE(initial);
  EXPECT_EQ(initial->AsInteger(), 15);
  const auto next = source.Prepare({
      .base_revision = 1,
      .changes =
          {
              {
                  .path = "main.vela",
                  .text = "let added = 3; let cold = 200; const title = 'new'; let score = 100; "
                          "class Counter { static value: int = 400; } "
                          "export func answer(): int { "
                          "return score + Counter.value + (title == 'old' ? 1 : 1000) + 100; } "
                          "export func edit(): void { score = 1000; } "
                          "export func bump(): int { score += 100; return score; } "
                          "export func readCold(): int { return cold + added; }",
              },
          },
      .added_roots = {},
  });
  ASSERT_TRUE(next) << DiagnosticsText(next.error().diagnostics);
  const auto compiled = (*compiler)->Compile(next->snapshot().program);
  ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
  auto prepared = dispatch.Prepare(1, compiled->image);
  ASSERT_TRUE(prepared) << prepared.error().message;
  // 候选已准备时，旧程序仍可继续写入；发布不能用 Prepare 时的值覆盖这些写入。
  ASSERT_TRUE(edit->Invoke({}));
  isolate.heap().Collect();
  ASSERT_TRUE(dispatch.Publish(std::move(*prepared)));
  ASSERT_TRUE(source.Commit(*next));
  const auto current = answer->Invoke({});
  const auto previous = old_answer->Invoke({});
  ASSERT_TRUE(current);
  ASSERT_TRUE(previous);
  EXPECT_EQ(current->AsInteger(), 150);
  EXPECT_EQ(previous->AsInteger(), 50);
  const auto changed = old_bump->Invoke({});
  ASSERT_TRUE(changed);
  EXPECT_EQ(changed->AsInteger(), 43);
  const auto after_write = answer->Invoke({});
  ASSERT_TRUE(after_write);
  EXPECT_EQ(after_write->AsInteger(), 151);
  // 未初始化槽从旧函数首次读取也必须选用新初始化器。
  const auto lazy = old_cold->Invoke({});
  ASSERT_TRUE(lazy);
  EXPECT_EQ(lazy->AsInteger(), 200);
  const auto added = cold->Invoke({});
  ASSERT_TRUE(added);
  EXPECT_EQ(added->AsInteger(), 203);
}

TEST(JitCompilerTest, SharedGlobalInitializationRetriesFailuresAndDetectsCycles) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
  const auto compiled =
      CompileText(**compiler,
                  "let divisor = 0; let answer = 84 / divisor; let cycle = readCycle(); "
                  "func readCycle(): int { return cycle; } "
                  "export func repair(): void { divisor = 2; } "
                  "export func read(operation: int): int { if (operation == 0) { return answer; } "
                  "return cycle; }");
  ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
  runtime::Isolate isolate(1);
  auto& dispatch = isolate.native_dispatch();
  auto prepared = dispatch.Prepare(0, compiled->image);
  ASSERT_TRUE(prepared) << prepared.error().message;
  ASSERT_TRUE(dispatch.Publish(std::move(*prepared)));
  const auto read = dispatch.FindEntry("main.vela", "read");
  const auto repair = dispatch.FindEntry("main.vela", "repair");
  ASSERT_TRUE(read);
  ASSERT_TRUE(repair);
  const std::array answer{runtime::Value::Integer(0)};
  EXPECT_FALSE(read->Invoke(answer));
  EXPECT_TRUE(isolate.native_control().CanPublish());
  ASSERT_TRUE(repair->Invoke({}));
  const auto retried = read->Invoke(answer);
  ASSERT_TRUE(retried);
  EXPECT_EQ(retried->AsInteger(), 42);
  const std::array cycle{runtime::Value::Integer(1)};
  EXPECT_FALSE(read->Invoke(cycle));
  EXPECT_FALSE(read->Invoke(cycle));
  EXPECT_TRUE(isolate.native_control().CanPublish());
}

class JitContractTest : public testing::TestWithParam<std::string_view> {};

TEST(JitCompilerTest, NominalTypesInHostCallsAndContainersSurviveLayoutVersions) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
  SourceSession source("main.vela");
  runtime::Isolate isolate(1);
  auto& dispatch = isolate.native_dispatch();
  const std::string declarations =
      "interface Reader { read(): int; } "
      "class Box implements Reader { value: int; read(): int { return this.value; } } "
      "export func make(): Box { return Box(value: 40); } "
      "export func wrap(value: Box): Box[] { return [value]; } "
      "export func check(values: Box[], value: dynamic): bool { "
      "return values[0] == value && value is Box && value is Reader; } ";
  const auto first = source.Prepare({
      .changes =
          {
              {
                  .path = "main.vela",
                  .text =
                      declarations + "export func read(value: Box): int { return value.value; }",
              },
          },
      .added_roots = {},
  });
  ASSERT_TRUE(first) << DiagnosticsText(first.error().diagnostics);
  {
    const auto compiled = (*compiler)->Compile(first->snapshot().program);
    ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
    auto prepared = dispatch.Prepare(0, compiled->image);
    ASSERT_TRUE(prepared) << prepared.error().message;
    ASSERT_TRUE(dispatch.Publish(std::move(*prepared)));
    ASSERT_TRUE(source.Commit(*first));
  }
  const auto make = dispatch.FindEntry("main.vela", "make");
  const auto wrap = dispatch.FindEntry("main.vela", "wrap");
  const auto read = dispatch.FindEntry("main.vela", "read");
  const auto check = dispatch.FindEntry("main.vela", "check");
  ASSERT_TRUE(make);
  ASSERT_TRUE(wrap);
  ASSERT_TRUE(read);
  ASSERT_TRUE(check);
  const auto created = make->Invoke({});
  ASSERT_TRUE(created);
  const runtime::RootedValue object(isolate, *created);
  const std::array object_argument{object.value()};
  const auto wrapped = wrap->Invoke(object_argument);
  ASSERT_TRUE(wrapped);
  const runtime::RootedValue array(isolate, *wrapped);
  const auto next = source.Prepare({
      .base_revision = 1,
      .changes =
          {
              {
                  .path = "main.vela",
                  .text = "class Added { other: string = 'new'; } " + declarations +
                          "export func read(value: Box): int { return value.value + 2; }",
              },
          },
      .added_roots = {},
  });
  ASSERT_TRUE(next) << DiagnosticsText(next.error().diagnostics);
  {
    const auto compiled = (*compiler)->Compile(next->snapshot().program);
    ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
    auto prepared = dispatch.Prepare(1, compiled->image);
    ASSERT_TRUE(prepared) << prepared.error().message;
    ASSERT_TRUE(dispatch.Publish(std::move(*prepared)));
    ASSERT_TRUE(source.Commit(*next));
  }
  isolate.heap().Collect();
  EXPECT_EQ((*compiler)->resource_counts().batches, 1U);
  const auto result = read->Invoke(object_argument);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->AsInteger(), 42);
  const std::array checked{array.value(), object.value()};
  const auto matches = check->Invoke(checked);
  ASSERT_TRUE(matches);
  EXPECT_TRUE(matches->AsBoolean());
  const auto newer = make->Invoke({});
  ASSERT_TRUE(newer);
  const auto* previous_layout = runtime::Heap::ClassOf(*object.value().AsObject());
  const auto* current_layout = runtime::Heap::ClassOf(*newer->AsObject());
  EXPECT_EQ(previous_layout, current_layout);
  EXPECT_EQ(&previous_layout->nominal_type(), &current_layout->nominal_type());
}

TEST_P(JitContractTest, RejectsIncompatibleCallsAndSlotsWithoutChangingThePublishedRun) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
  SourceSession source("main.vela");
  runtime::Isolate isolate(1);
  auto& dispatch = isolate.native_dispatch();
  const auto first = source.Prepare({
      .changes =
          {
              {
                  .path = "main.vela",
                  .text = "let value = 40; export func answer(input: int): int { "
                          "return value + input; }",
              },
          },
      .added_roots = {},
  });
  ASSERT_TRUE(first) << DiagnosticsText(first.error().diagnostics);
  {
    const auto compiled = (*compiler)->Compile(first->snapshot().program);
    ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
    auto prepared = dispatch.Prepare(0, compiled->image);
    ASSERT_TRUE(prepared) << prepared.error().message;
    ASSERT_TRUE(dispatch.Publish(std::move(*prepared)));
    ASSERT_TRUE(source.Commit(*first));
  }
  const auto answer = dispatch.FindEntry("main.vela", "answer");
  ASSERT_TRUE(answer);
  const auto previous = answer->Capture();
  ASSERT_TRUE(previous);
  const std::array arguments{runtime::Value::Integer(2)};
  const auto initialized = answer->Invoke(arguments);
  ASSERT_TRUE(initialized);
  EXPECT_EQ(initialized->AsInteger(), 42);
  const auto counts = isolate.layout_counts();
  const auto next = source.Prepare({
      .base_revision = 1,
      .changes = {{.path = "main.vela", .text = std::string(GetParam())}},
      .added_roots = {},
  });
  ASSERT_TRUE(next) << DiagnosticsText(next.error().diagnostics);
  {
    const auto compiled = (*compiler)->Compile(next->snapshot().program);
    ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
    const auto prepared = dispatch.Prepare(1, compiled->image);
    ASSERT_FALSE(prepared);
    EXPECT_EQ(prepared.error().status, runtime::NativePublicationStatus::RequiresReload);
    EXPECT_NE(prepared.error().declaration, DeclarationId{});
    EXPECT_FALSE(prepared.error().message.empty());
  }
  EXPECT_EQ(dispatch.revision(), 1U);
  EXPECT_EQ(source.snapshot()->revision, 1U);
  EXPECT_EQ(isolate.layout_counts(), counts);
  isolate.heap().Collect();
  EXPECT_EQ((*compiler)->resource_counts().batches, 1U);
  const auto current = answer->Invoke(arguments);
  const auto captured = previous->Invoke(arguments);
  ASSERT_TRUE(current);
  ASSERT_TRUE(captured);
  EXPECT_EQ(current->AsInteger(), 42);
  EXPECT_EQ(captured->AsInteger(), 42);
}

INSTANTIATE_TEST_SUITE_P(
    IncompatibleChanges,
    JitContractTest,
    testing::Values(
        "let value = 100; export func answer(other: int): int { return value + other; }",
        "let value = 100; export func answer(input: double): int { return value + input.toInt(); }",
        "let value = 100; export func answer(input: int?): int { return value + (input ?? 0); }",
        "let value = 100; export func answer(input: int): double { return 1.5; }",
        "let value = 100; export async func answer(input: int): Future<int> { return value + "
        "input; }",
        "let value = 'new'; export func answer(input: int): int { return value.length + input; }",
        "let value: int? = null; export func answer(input: int): int { return (value ?? 0) + "
        "input; }",
        "const value = 100; export func answer(input: int): int { return value + input; }"));

struct NominalChange final {
  std::string_view before;
  std::string_view after;
};

class JitNominalContractTest : public testing::TestWithParam<NominalChange> {};

TEST_P(JitNominalContractTest, RejectsTypeContractChangesIncludingUninstantiatedDeclarations) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
  SourceSession source("main.vela");
  runtime::Isolate isolate(1);
  auto& dispatch = isolate.native_dispatch();
  const auto change = GetParam();
  const auto first = source.Prepare({
      .changes =
          {
              {
                  .path = "main.vela",
                  .text = std::string(change.before) + " export func answer(): int { return 42; }",
              },
          },
      .added_roots = {},
  });
  ASSERT_TRUE(first) << DiagnosticsText(first.error().diagnostics);
  {
    const auto compiled = (*compiler)->Compile(first->snapshot().program);
    ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
    auto prepared = dispatch.Prepare(0, compiled->image);
    ASSERT_TRUE(prepared) << prepared.error().message;
    ASSERT_TRUE(dispatch.Publish(std::move(*prepared)));
    ASSERT_TRUE(source.Commit(*first));
  }
  const auto counts = isolate.layout_counts();
  const auto next = source.Prepare({
      .base_revision = 1,
      .changes =
          {
              {
                  .path = "main.vela",
                  .text = std::string(change.after) + " export func answer(): int { return 0; }",
              },
          },
      .added_roots = {},
  });
  if (!next) {
    EXPECT_EQ(next.error().status, SourceCandidateStatus::RequiresReload);
    EXPECT_NE(next.error().declaration, DeclarationId{});
  } else {
    const auto compiled = (*compiler)->Compile(next->snapshot().program);
    ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
    const auto prepared = dispatch.Prepare(1, compiled->image);
    ASSERT_FALSE(prepared);
    EXPECT_EQ(prepared.error().status, runtime::NativePublicationStatus::RequiresReload);
    EXPECT_NE(prepared.error().declaration, DeclarationId{});
  }
  EXPECT_EQ(isolate.layout_counts(), counts);
  EXPECT_EQ(dispatch.revision(), 1U);
  const auto answer = dispatch.FindEntry("main.vela", "answer");
  ASSERT_TRUE(answer);
  const auto result = answer->Invoke({});
  ASSERT_TRUE(result);
  EXPECT_EQ(result->AsInteger(), 42);
}

INSTANTIATE_TEST_SUITE_P(
    IncompatibleTypes,
    JitNominalContractTest,
    testing::Values(NominalChange{"class A {}", ""},
                    NominalChange{"interface A { read(): int; }", ""},
                    NominalChange{"class A {}", "interface A { read(): int; }"},
                    NominalChange{"class A {} class B {} class C extends A {}",
                                  "class A {} class B {} class C extends B {}"},
                    NominalChange{
                        "interface A { read(): int; } class C { read(): int { return 1; } }",
                        "interface A { read(): int; } class C implements A { "
                        "read(): int { return 1; } }"},
                    NominalChange{"enum A { first, second }", "enum A { second, first }"},
                    NominalChange{"enum A { first, second }", "enum A { first, second, third }"},
                    NominalChange{"func same<T>(value: T): T { return value; }",
                                  "func same<T, U>(value: T): T { return value; }"},
                    NominalChange{"class Box<T> { value: T; }", "class Box<T, U> { value: T; }"}));

TEST(JitCompilerTest, OldInterfaceCallsSelectCurrentMethodsByStableSelectors) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
  SourceSession source("main.vela");
  runtime::Isolate isolate(1);
  auto& dispatch = isolate.native_dispatch();
  const auto first = source.Prepare({
      .changes =
          {
              {
                  .path = "main.vela",
                  .text = "interface Reader { read(): int; other(): int; } "
                          "class Box implements Reader { "
                          "read(): int { return 40; } other(): int { return 1; } } "
                          "export func make(): Box { return Box(); } "
                          "export func call(value: Reader): int { return value.read(); }",
              },
          },
      .added_roots = {},
  });
  ASSERT_TRUE(first) << DiagnosticsText(first.error().diagnostics);
  {
    const auto compiled = (*compiler)->Compile(first->snapshot().program);
    ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
    auto prepared = dispatch.Prepare(0, compiled->image);
    ASSERT_TRUE(prepared) << prepared.error().message;
    ASSERT_TRUE(dispatch.Publish(std::move(*prepared)));
    ASSERT_TRUE(source.Commit(*first));
  }
  const auto make = dispatch.FindEntry("main.vela", "make");
  const auto call = dispatch.FindEntry("main.vela", "call");
  ASSERT_TRUE(make);
  ASSERT_TRUE(call);
  const auto old_call = call->Capture();
  ASSERT_TRUE(old_call);
  const auto created = make->Invoke({});
  ASSERT_TRUE(created);
  const runtime::RootedValue receiver(isolate, *created);
  const auto next = source.Prepare({
      .base_revision = 1,
      .changes =
          {
              {
                  .path = "main.vela",
                  .text = "interface Reader { other(): int; read(): int; } "
                          "class Box implements Reader { "
                          "other(): int { return 100; } read(): int { return 42; } } "
                          "export func make(): Box { return Box(); } "
                          "export func call(value: Reader): int { return value.read() + 1; }",
              },
          },
      .added_roots = {},
  });
  ASSERT_TRUE(next) << DiagnosticsText(next.error().diagnostics);
  const auto compiled = (*compiler)->Compile(next->snapshot().program);
  ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
  auto prepared = dispatch.Prepare(1, compiled->image);
  ASSERT_TRUE(prepared) << prepared.error().message;
  ASSERT_TRUE(dispatch.Publish(std::move(*prepared)));
  ASSERT_TRUE(source.Commit(*next));
  isolate.heap().Collect();
  const std::array arguments{receiver.value()};
  const auto previous = old_call->Invoke(arguments);
  const auto current = call->Invoke(arguments);
  ASSERT_TRUE(previous);
  ASSERT_TRUE(current);
  EXPECT_EQ(previous->AsInteger(), 42);
  EXPECT_EQ(current->AsInteger(), 43);
}

class JitLanguageTest
    : public testing::TestWithParam<std::tuple<std::string_view, NativeCodeTier>> {};

TEST_P(JitLanguageTest, ExecutesNativeProductionPathsWithPreciseRoots) {
  const auto [source, tier] = GetParam();
  SCOPED_TRACE(source);
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
  const auto compiled = CompileText(**compiler, std::string(source), tier);
  ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
  runtime::Isolate isolate(1);
  auto prepared = isolate.native_dispatch().Prepare(0, compiled->image);
  ASSERT_TRUE(prepared) << prepared.error().message;
  ASSERT_TRUE(isolate.native_dispatch().Publish(std::move(*prepared)));
  const auto entry = isolate.native_dispatch().FindEntry("main.vela", "main");
  ASSERT_TRUE(entry);
  const auto result = entry->Invoke({});
  ASSERT_TRUE(result) << ExecutionStatusName(result.error());
  const runtime::RootedValue root(isolate, *result);
  auto value = root.value();
  if (value.IsObject()) {
    auto* future = runtime::Heap::GetFuture(*value.AsObject());
    ASSERT_NE(future, nullptr);
    ASSERT_TRUE(isolate.scheduler().PumpUntil(*future, runtime::FuturePumpMode::WaitForHost));
    ASSERT_EQ(future->state(), runtime::FutureState::Succeeded);
    value = *future->value();
  }
  ASSERT_TRUE(value.IsInteger());
  EXPECT_EQ(value.AsInteger(), 42);
}

INSTANTIATE_TEST_SUITE_P(NativeSemantics,
                         JitLanguageTest,
                         testing::Combine(testing::Values(
                                              R"(
func sum(count: int): int { if (count == 0) { return 0; } return count + sum(count - 1); }
export func main(): int { let value = 0; for (let i = 0; i < 6; i++) { value += i; }
  switch (value) { case 15: return sum(6) << 1; default: return 0; } }
)",
                                              R"(
func descend(depth: int): int {
  const text = '${depth}';
  if (depth == 0) { return text.length + 41; }
  const answer = descend(depth - 1);
  return text.length > 0 ? answer : 0;
}
export func main(): int { return descend(512); }
)",
                                              R"(
export func main(): int {
  const text = 'A中😀e\u0301\0';
  const joined = [text, text].join('界');
  const absent: string? = null;
  const formatted = '${text}/${42}/${true}/${absent}';
  const builder = String(); builder.append(text); builder.append('ß');
  const snapshot = builder.toString();
  const forward = String(); const backward = String();
  const dynamicText: dynamic = text;
  for (let i = 0; i < dynamicText.length; i++) { forward.append(dynamicText[i]); }
  for (let i = text.length - 1; i >= 0; i--) { backward.append(text[i]); }
  return joined.length == 13 && formatted.length == 19 && snapshot.length == 7 &&
         snapshot.toUpperCase().length == 8 && forward.toString() == text &&
         backward.toString() == '\0\u0301e😀中A' && joined.slice(6, 9) == '界A中' ? 42 : 0;
}
)",
                                              R"(
func counter(value: int): () => int { return (): int => { value++; return value; }; }
export func main(): int { const next = counter(40); const values: (() => int)[] = [next];
  next(); return values[0](); }
)",
                                              R"(
interface Reader { read(): int; }
class Base { stored: int = 40; read(): int { return this.stored; } }
class Child extends Base implements Reader { override read(): int { return super.read() + 2; } }
export func main(): int { const reader: Reader = Child(); const callback = reader.read;
  return callback(); }
)",
                                              R"(
func same<T>(value: T): T { return value; }
class Box<T> { private stored: T; constructor(value: T) { this.stored = value; }
  read(): T { return this.stored; } }
type IntegerBox = Box<int>;
export func main(): int { return same<int>(IntegerBox(42).read()); }
)",
                                              R"(
let count = 0;
func initial(): int { count++; return 40; }
const value = initial();
export func main(): int { const first = value; return first + value - 39 + count; }
)",
                                              R"(
func divide(value: int, denominator: int): int { return value / denominator; }
export func main(): int { const result = divide(84, 2);
  try { const zero = 0; return 1 / zero; }
  catch (error: Error) { return error.code == 'DivisionByZero' ? result : 0; } }
)",
                                              R"(
enum Shade { light, dark }
func choose(value: int?): int { return match (value) { null => 0, let present => present }; }
export func main(): int { const selected = Shade.dark;
  return match (selected) { Shade.light => 0, Shade.dark => choose(42) }; }
)",
                                              R"(
export func main(): int { const values = [1, 2, 3]; values.push(4);
  const mapped = values.map((value: int): int => value * 2);
  return mapped.reduce((sum: int, value: int): int => sum + value, 0) + 22; }
)",
                                              R"(
class Item { value: int; }
export func main(): int { const key = Item(value: 40); const values = Map<Item, string>();
  values.set(key, 'ab'); const text = values.get(key);
  if (text != null && !values.has(Item(value: 40))) { return key.value + text.length; }
  return 0; }
)",
                                              R"(
export func main(): int { const bytes = ByteArray(length: 8); const data = ByteData.view(bytes);
  const write: (int,int) => void = data.setInt32; write(0, 42);
  return data.getInt32(0) == 42 && bytes[3] == 42 ? 42 : 0; }
)",
                                              R"(
export func main(): int { const root: dynamic = Json.parse('{"items":[40,2]}');
  const values: dynamic[] = root.get('items'); return (values[0] as int) + (values[1] as int); }
)",
                                              R"(
export func main(): int { const found = RegExp('(?<digits>[0-9]+)').firstMatch('😀42');
  if (found != null) { return int.parse(found.namedGroup('digits') ?? '0'); } return 0; }
)",
                                              R"(
export async func main(): Future<int> {
  const values = await Future.wait([Future.value(20), Future.delayed(1, (): int => 22)]);
  return values[0] + values[1]; }
)",
                                              R"(
class Failure { value: int; }
async func failed(): Future<int> { await sleep(1); throw Failure(value: 40); }
export async func main(): Future<int> { let value = 0;
  try { value = await failed(); } catch (error: Failure) { value = error.value; }
  finally { value += 2; } return value; }
)"),
                                          testing::Values(NativeCodeTier::Baseline,
                                                          NativeCodeTier::Optimized)));

std::binary_semaphore& StopSignal() {
  static std::binary_semaphore signal{0};
  return signal;
}

void Entered(runtime::NativeContext& context) noexcept {
  EXPECT_TRUE(context.isolate().native_control().HasHostBorrow());
  StopSignal().release();
}

void Finalized() noexcept {
  ADD_FAILURE() << "Stopping a run must not execute its finalizers";
}

std::int64_t Twice(std::int64_t value) noexcept {
  return value * 2;
}

std::string CopyText(runtime::NativeContext& context, std::string_view text) noexcept {
  EXPECT_TRUE(context.isolate().native_control().HasHostBorrow());
  context.isolate().heap().Collect();
  return std::string(text);
}

std::expected<void, runtime::NativeError> Reject() noexcept {
  return std::unexpected(runtime::NativeError{.message = "rejected"});
}

TEST(JitCompilerTest, FixedHostBindingsUseDirectCheckedAndManagedCallingConventions) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
  const auto compiled = CompileText(**compiler, R"(
external func twice(value: int): int;
external func copyText(text: string): string;
external func reject(): void;
export func main(): int {
  const copy = copyText;
  const text = copy(['4', '0'].join(''));
  try { reject(); return 0; }
  catch (error: Error) {
    if (error.code != 'NativeFailure' || error.message != 'rejected') { return 0; }
  }
  return twice(20) + text.length;
}
)");
  ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
  runtime::Isolate isolate(1);
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<&Twice>("twice")));
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<&CopyText>("copyText")));
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<&Reject>("reject")));
  static_cast<void>(isolate.FreezeBindings());
  auto prepared = isolate.native_dispatch().Prepare(0, compiled->image);
  ASSERT_TRUE(prepared) << prepared.error().message;
  ASSERT_TRUE(isolate.native_dispatch().Publish(std::move(*prepared)));
  const auto entry = isolate.native_dispatch().FindEntry("main.vela", "main");
  ASSERT_TRUE(entry);
  const auto result = entry->Invoke({});
  ASSERT_TRUE(result) << ExecutionStatusName(result.error());
  EXPECT_EQ(result->AsInteger(), 42);
  EXPECT_FALSE(isolate.native_control().HasHostBorrow());
}

std::int64_t UnexpectedInitialization() noexcept {
  ADD_FAILURE() << "Load must not execute a user initializer";
  return 0;
}

TEST(JitCompilerTest, AddedModulesExposeTheirExportsWithoutRunningUserInitializers) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
  SourceSession source("main.vela");
  runtime::Isolate isolate(1);
  ASSERT_TRUE(isolate.RegisterNative(
      runtime::NativeFunction::Bind<&UnexpectedInitialization>("initialized")));
  static_cast<void>(isolate.FreezeBindings());
  auto& dispatch = isolate.native_dispatch();
  const auto first = source.Prepare({
      .changes = {{.path = "main.vela", .text = "export func answer(): int { return 40; }"}},
      .added_roots = {},
  });
  ASSERT_TRUE(first) << DiagnosticsText(first.error().diagnostics);
  const auto initial_code = (*compiler)->Compile(first->snapshot().program);
  ASSERT_TRUE(initial_code) << DiagnosticsText(initial_code.error());
  auto initial = dispatch.Prepare(0, initial_code->image);
  ASSERT_TRUE(initial) << initial.error().message;
  ASSERT_TRUE(dispatch.Publish(std::move(*initial)));
  ASSERT_TRUE(source.Commit(*first));
  const auto original = dispatch.FindEntry("main.vela", "answer");
  ASSERT_TRUE(original);
  const auto added = source.Prepare({
      .base_revision = 1,
      .changes =
          {
              {
                  .path = "added.vela",
                  .text = "external func initialized(): int; let unused = initialized(); "
                          "export func answer(): int { return 42; }",
              },
          },
      .added_roots = {"added.vela"},
  });
  ASSERT_TRUE(added) << DiagnosticsText(added.error().diagnostics);
  const auto code = (*compiler)->Compile(added->snapshot().program);
  ASSERT_TRUE(code) << DiagnosticsText(code.error());
  auto prepared = dispatch.Prepare(1, code->image);
  ASSERT_TRUE(prepared) << prepared.error().message;
  ASSERT_TRUE(dispatch.Publish(std::move(*prepared)));
  ASSERT_TRUE(source.Commit(*added));
  const auto loaded = dispatch.FindEntry("added.vela", "answer");
  ASSERT_TRUE(loaded);
  EXPECT_EQ(loaded->Invoke({})->AsInteger(), 42);
  EXPECT_EQ(original->Invoke({})->AsInteger(), 40);
}

TEST(JitCompilerTest, CrossThreadStopExitsANonAllocatingLoopWithoutRunningFinally) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler) << DiagnosticsText(compiler.error());
  const auto compiled = CompileText(**compiler, R"(
external func entered(): void;
external func finalized(): void;
export func main(): void { entered(); try { while (true) {} } finally { finalized(); } }
)");
  ASSERT_TRUE(compiled) << DiagnosticsText(compiled.error());
  runtime::Isolate isolate;
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<&Entered>("entered")));
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<&Finalized>("finalized")));
  static_cast<void>(isolate.FreezeBindings());
  auto prepared = isolate.native_dispatch().Prepare(0, compiled->image);
  ASSERT_TRUE(prepared) << prepared.error().message;
  ASSERT_TRUE(isolate.native_dispatch().Publish(std::move(*prepared)));
  const auto entry = isolate.native_dispatch().FindEntry("main.vela", "main");
  ASSERT_TRUE(entry);
  std::jthread request([&] {
    constexpr auto kEntryTimeout = std::chrono::seconds(10);
    const auto entered = StopSignal().try_acquire_for(kEntryTimeout);
    EXPECT_TRUE(entered);
    if (entered) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    isolate.native_control().RequestStop();
  });
  const auto result = entry->Invoke({});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), ExecutionStatus::ExecutionStopped);
  EXPECT_FALSE(isolate.native_control().HasHostBorrow());
}

}  // namespace
}  // namespace vela::compiler::jit
