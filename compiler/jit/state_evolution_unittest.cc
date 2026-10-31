#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

#include "base/execution_status.h"
#include "base/native_program_mode.h"
#include "compiler/development/source_session.h"
#include "compiler/jit/compiler.h"
#include "compiler/jit/test_support.h"
#include "runtime/heap/heap.h"
#include "runtime/heap/rooted_value.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_context.h"
#include "runtime/native/native_dispatch.h"
#include "runtime/native/native_function.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::compiler::jit {
namespace {

struct JitStateTest : public testing::TestWithParam<NativeCodeTier> {
  void SetUp() override {
    auto compiler = Compiler::Create();
    ASSERT_TRUE(compiler);
    compiler_ = std::move(*compiler);
  }

  std::expected<void, std::string> Update(std::string text) {
    const auto retained = isolate_.native_dispatch().RetainedDeclarations();
    return test::UpdateSource(*compiler_, source_, isolate_, std::move(text), GetParam(), retained);
  }

  runtime::NativeNamedEntry Entry(std::string_view name) {
    return *isolate_.native_dispatch().FindEntry("main.vela", name);
  }

  std::unique_ptr<Compiler> compiler_;
  SourceSession source_{"main.vela"};
  runtime::Isolate isolate_{1};
};

bool IsConstructing(runtime::NativeContext& context) noexcept {
  return context.isolate().native_control().IsInitializing();
}

std::int64_t FreezeMapHeap(runtime::NativeContext& context) noexcept {
  auto& heap = context.isolate().heap();
  heap.SetAllocationLimit(heap.allocated_bytes());
  return 1;
}

TEST_P(JitStateTest, MapLiteralAllocationAndInsertionFailuresStopWithoutPublishingPartialMaps) {
  ASSERT_TRUE(isolate_.RegisterNative(runtime::NativeFunction::Bind<FreezeMapHeap>("freeze")));
  const auto update = Update(R"(
external func freeze(): int;
let calls = 0;
func key(): int { calls++; return 1; }
func item(): int { calls++; return 42; }
export func allocate(): int { const map = {key(): item()}; return map.size; }
export func insert(): int { const map = {freeze(): item(), key(): item()}; return map.size; }
export func count(): int { return calls; }
)");
  ASSERT_TRUE(update) << update.error();
  EXPECT_EQ(Entry("count").Invoke({})->AsInteger(), 0);
  auto& heap = isolate_.heap();
  heap.Collect();
  const auto baseline = heap.allocated_bytes();
  heap.SetAllocationLimit(baseline);
  const auto allocation = Entry("allocate").Invoke({});
  heap.SetAllocationLimit(std::numeric_limits<std::size_t>::max());
  ASSERT_FALSE(allocation);
  EXPECT_EQ(allocation.error(), ExecutionStatus::OutOfMemory);
  EXPECT_EQ(Entry("count").Invoke({})->AsInteger(), 0);
  const auto insertion = Entry("insert").Invoke({});
  heap.SetAllocationLimit(std::numeric_limits<std::size_t>::max());
  ASSERT_FALSE(insertion);
  EXPECT_EQ(insertion.error(), ExecutionStatus::OutOfMemory);
  EXPECT_EQ(Entry("count").Invoke({})->AsInteger(), 1);
  heap.Collect();
  EXPECT_EQ(heap.allocated_bytes(), baseline);
}

TEST_P(JitStateTest, MapLiteralContinuationsKeepTheirEntriesAndGlobalsAcrossReload) {
  const std::string common =
      "const ready = Completer<int>(); let calls = 0; "
      "func key(): string { calls++; return 'answer'; } "
      "const global = {key(): 7}; "
      "export func count(): int { return calls; } "
      "export func stored(): int { return global.get('answer') ?? 0; } "
      "export func release(): void { ready.complete(40); } ";
  auto update = Update(common +
                       "export async func start(): Future<int> { "
                       "const map = {key(): await ready.future, 'extra': 2}; "
                       "return (map.get('answer') ?? 0) + (map.get('extra') ?? 0); }");
  ASSERT_TRUE(update) << update.error();
  EXPECT_EQ(Entry("stored").Invoke({})->AsInteger(), 7);
  const runtime::RootedValue pending(isolate_, *Entry("start").Invoke({}));
  EXPECT_EQ(Entry("count").Invoke({})->AsInteger(), 2);
  update = Update(common +
                  "export async func start(): Future<int> { "
                  "const map = {'new': 100}; return map.get('new') ?? 0; }");
  ASSERT_TRUE(update) << update.error();
  isolate_.heap().Collect();
  ASSERT_TRUE(Entry("release").Invoke({}));
  EXPECT_TRUE(test::CompletesWithInteger(isolate_, pending.value(), 42));
  EXPECT_TRUE(test::CompletesWithInteger(isolate_, *Entry("start").Invoke({}), 100));
  EXPECT_EQ(Entry("stored").Invoke({})->AsInteger(), 7);
  EXPECT_EQ(Entry("count").Invoke({})->AsInteger(), 2);
}

TEST_P(JitStateTest, ParentExpansionAndReorderingKeepIdentitySlotsAndOldReaders) {
  const std::string common =
      "export func make(): Child { return Child(); } "
      "const keys = Map<Child, int>(); "
      "export func keep(value: Child): void { keys.set(value, 9); } "
      "export func key(value: Child): int { return keys.get(value) ?? 0; } ";
  auto update = Update(
      "class Base { left: int = 40; } "
      "class Child extends Base { right: int = 2; } " +
      common + "export func read(value: Child): int { return value.left + value.right; }");
  ASSERT_TRUE(update) << update.error();
  const runtime::RootedValue object(isolate_, *Entry("make").Invoke({}));
  const auto* address = object.value().AsObject();
  const auto* old_layout = runtime::Heap::ClassOf(*address);
  const auto old_fields = old_layout->field_declarations();
  const auto left = old_layout->FieldIndex(old_fields.front());
  const auto old_reader = Entry("read").Capture();
  ASSERT_TRUE(old_reader);
  const std::array arguments{object.value()};
  ASSERT_TRUE(Entry("keep").Invoke(arguments));
  update = Update(
      "class Base { added: int = 7; left: int = 100; } "
      "class Child extends Base { right: int = 20; } " +
      common +
      "export func read(value: Child): int { return value.left + value.right + value.added; }");
  ASSERT_TRUE(update) << update.error();
  EXPECT_EQ(object.value().AsObject(), address);
  const auto* layout = runtime::Heap::ClassOf(*address);
  EXPECT_EQ(layout->storage(), runtime::ClassStorage::Reloadable);
  EXPECT_EQ(layout->FieldIndex(old_fields.front()), left);
  EXPECT_EQ(old_reader->Invoke(arguments)->AsInteger(), 42);
  EXPECT_EQ(Entry("read").Invoke(arguments)->AsInteger(), 49);
  EXPECT_EQ(Entry("key").Invoke(arguments)->AsInteger(), 9);
  isolate_.heap().Collect();
  EXPECT_EQ(Entry("read").Invoke(arguments)->AsInteger(), 49);
}

TEST_P(JitStateTest, NewFieldsAreLazyPerObjectAndFirstWritesSkipTheirDefaults) {
  const std::string common =
      "let calls = 0; func initial(): int { calls++; return 7; } "
      "export func count(): int { return calls; } "
      "export func make(): Box { return Box(); } ";
  auto update = Update("class Box { value: int = 40; } " + common);
  ASSERT_TRUE(update) << update.error();
  const runtime::RootedValue first(isolate_, *Entry("make").Invoke({}));
  const runtime::RootedValue second(isolate_, *Entry("make").Invoke({}));
  update = Update(
      "class Box { value: int = 100; extra: int = initial(); items: int[] = []; } " + common +
      "export func read(value: Box): int { return value.value + value.extra; } "
      "export func write(value: Box): void { value.extra = 2; } "
      "export func push(value: Box): int { value.items.push(1); return value.items.length; }");
  ASSERT_TRUE(update) << update.error();
  EXPECT_EQ(Entry("count").Invoke({})->AsInteger(), 0);
  const std::array first_argument{first.value()};
  const std::array second_argument{second.value()};
  ASSERT_TRUE(Entry("write").Invoke(first_argument));
  EXPECT_EQ(Entry("read").Invoke(first_argument)->AsInteger(), 42);
  EXPECT_EQ(Entry("count").Invoke({})->AsInteger(), 0);
  EXPECT_EQ(Entry("read").Invoke(second_argument)->AsInteger(), 47);
  EXPECT_EQ(Entry("count").Invoke({})->AsInteger(), 1);
  EXPECT_EQ(Entry("push").Invoke(first_argument)->AsInteger(), 1);
  EXPECT_EQ(Entry("push").Invoke(first_argument)->AsInteger(), 2);
  EXPECT_EQ(Entry("push").Invoke(second_argument)->AsInteger(), 1);
}

TEST_P(JitStateTest, CyclicNewFieldInitializationCanRetryWithTheLatestInitializer) {
  const std::string common =
      "let saved: Box? = null; "
      "export func make(): Box { return Box(); } "
      "export func keep(value: Box): void { saved = value; } ";
  auto update = Update("class Box { value: int = 40; } " + common);
  ASSERT_TRUE(update) << update.error();
  const runtime::RootedValue object(isolate_, *Entry("make").Invoke({}));
  const std::array argument{object.value()};
  ASSERT_TRUE(Entry("keep").Invoke(argument));
  const std::string fields = "class Box { value: int = 40; extra: int = initial(); } ";
  const std::string reader = "export func read(value: Box): int { return value.extra; } ";
  update = Update(fields + common + reader +
                  "func initial(): int { const value = saved; if (value != null) { return "
                  "value.extra; } return 0; }");
  ASSERT_TRUE(update) << update.error();
  const auto failed = Entry("read").Invoke(argument);
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error(), ExecutionStatus::CyclicInitialization);
  update = Update(fields + common + reader + "func initial(): int { return 8; }");
  ASSERT_TRUE(update) << update.error();
  EXPECT_EQ(Entry("read").Invoke(argument)->AsInteger(), 8);
}

TEST_P(JitStateTest, RequiredNewFieldsRejectAtomicallyWhenAnInstanceIsReachable) {
  const std::string common = "export func make(): Box { return Box(value: 40); } ";
  auto update = Update("class Box { value: int; } " + common);
  ASSERT_TRUE(update) << update.error();
  const runtime::RootedValue object(isolate_, *Entry("make").Invoke({}));
  const auto* layout = runtime::Heap::ClassOf(*object.value().AsObject());
  update = Update(
      "class Box { value: int; required: int; } "
      "export func make(): Box { return Box(value: 40, required: 2); }");
  ASSERT_FALSE(update);
  EXPECT_EQ(isolate_.native_dispatch().revision(), 1U);
  EXPECT_EQ(source_.snapshot()->revision, 1U);
  EXPECT_EQ(runtime::Heap::ClassOf(*object.value().AsObject()), layout);
  EXPECT_EQ(runtime::Heap::GetField(*object.value().AsObject(), 0)->AsInteger(), 40);
}

TEST_P(JitStateTest, OldFactoriesUseStableExplicitArgumentsAndTheCurrentConstructorDefaults) {
  auto update = Update(
      "class Box { first: int = 1; second: int = 2; } "
      "export func make(): Box { return Box(first: 40); }");
  ASSERT_TRUE(update) << update.error();
  const auto old_factory = Entry("make").Capture();
  ASSERT_TRUE(old_factory);
  update = Update(
      "class Box { second: int = 20; third: int = 3; first: int = 10; } "
      "export func make(): Box { return Box(first: 80); } "
      "export func read(value: Box): int { return value.first * 100 + value.second * 10 + "
      "value.third; }");
  ASSERT_TRUE(update) << update.error();
  const runtime::RootedValue previous(isolate_, *old_factory->Invoke({}));
  const runtime::RootedValue current(isolate_, *Entry("make").Invoke({}));
  EXPECT_EQ(Entry("read").Invoke(std::array{previous.value()})->AsInteger(), 4203);
  EXPECT_EQ(Entry("read").Invoke(std::array{current.value()})->AsInteger(), 8203);
}

TEST_P(JitStateTest, DeletedFieldsAreRejectedOnlyWhileAReachableOldClosureDependsOnThem) {
  auto update = Update(
      "class Box { stored: int = 42; } "
      "export func make(): Box { return Box(); } "
      "export func reader(value: Box): () => int { return (): int => value.stored; }");
  ASSERT_TRUE(update) << update.error();
  const runtime::RootedValue object(isolate_, *Entry("make").Invoke({}));
  std::optional<runtime::RootedValue> closure;
  closure.emplace(isolate_, *Entry("reader").Invoke(std::array{object.value()}));
  const auto* original = runtime::Heap::ClassOf(*object.value().AsObject());
  const auto removed_field = original->field_declarations().front();
  const std::string changed =
      "class Box {} "
      "export func make(): Box { return Box(); } "
      "export func reader(value: Box): () => int { return (): int => 0; }";
  update = Update(changed);
  ASSERT_FALSE(update);
  EXPECT_NE(update.error().find("retained function"), std::string::npos);
  EXPECT_NE(update.error().find("deleted field"), std::string::npos);
  EXPECT_EQ(runtime::Heap::ClassOf(*object.value().AsObject()), original);
  closure.reset();
  update = Update(changed);
  ASSERT_TRUE(update) << update.error();
  EXPECT_FALSE(runtime::Heap::ClassOf(*object.value().AsObject())->FieldIndex(removed_field));
  EXPECT_TRUE(runtime::Heap::GetField(*object.value().AsObject(), 0)->IsNull());
}

TEST_P(JitStateTest, NestedOldLambdasAreCheckedBeforeTheOldFactoryCreatesThem) {
  auto update = Update(
      "class Box { stored: int = 42; } const box = Box(); "
      "export func factory(): () => int { return (): int => box.stored; }");
  ASSERT_TRUE(update) << update.error();
  std::optional<runtime::NativeVersionEntry> factory = *Entry("factory").Capture();
  const std::string changed =
      "class Box {} const box = Box(); "
      "export func factory(): () => int { return (): int => 0; }";
  update = Update(changed);
  ASSERT_FALSE(update);
  EXPECT_NE(update.error().find("deleted field"), std::string::npos);
  EXPECT_EQ(isolate_.native_dispatch().revision(), 1U);
  factory.reset();
  update = Update(changed);
  ASSERT_TRUE(update) << update.error();
}

TEST_P(JitStateTest, OldConstructionPlansRejectNewRequiredFieldsWithoutAnyLiveInstances) {
  auto update = Update(
      "class Box { value: int = 1; } "
      "export func make(): Box { return Box(); }");
  ASSERT_TRUE(update) << update.error();
  std::optional<runtime::NativeVersionEntry> factory = *Entry("make").Capture();
  const std::string changed =
      "class Box { value: int = 1; required: int; } "
      "export func make(): Box { return Box(required: 2); }";
  update = Update(changed);
  ASSERT_FALSE(update);
  EXPECT_NE(update.error().find("now required"), std::string::npos);
  factory.reset();
  update = Update(changed);
  ASSERT_TRUE(update) << update.error();
  EXPECT_TRUE(Entry("make").Invoke({}));
}

TEST_P(JitStateTest, SuspendedGenericInterfaceMethodsKeepTheirConcreteVersionAfterAlphaRename) {
  const std::string common =
      "const ready = Completer<void>(); "
      "const worker: Delayed = Worker(); "
      "export func release(): void { ready.complete(); } ";
  auto update = Update(
      "interface Delayed { later<T extends int>(value: T): Future<int>; } "
      "class Worker implements Delayed { "
      "async later<T extends int>(value: T): Future<int> { await ready.future; return value + 1; } "
      "} " +
      common + "export func start(): Future<int> { return worker.later(41); }");
  ASSERT_TRUE(update) << update.error();
  const runtime::RootedValue pending(isolate_, *Entry("start").Invoke({}));
  update = Update(
      "interface Delayed { later<U extends int>(value: U): Future<int>; } "
      "class Worker implements Delayed { "
      "async later<U extends int>(value: U): Future<int> { await ready.future; return value + 2; } "
      "} " +
      common + "export func start(): Future<int> { return Future.value(100); }");
  ASSERT_TRUE(update) << update.error();
  isolate_.heap().Collect();
  const auto incompatible = Update(
      "interface Delayed { later<U extends double>(value: U): Future<int>; } "
      "class Worker implements Delayed { "
      "async later<U extends double>(value: U): Future<int> { await ready.future; return 0; } } " +
      common + "export func start(): Future<int> { return Future.value(100); }");
  ASSERT_FALSE(incompatible);
  EXPECT_EQ(isolate_.native_dispatch().revision(), 2U);
  ASSERT_TRUE(Entry("release").Invoke({}));
  ASSERT_TRUE(test::CompletesWithInteger(isolate_, pending.value(), 42));
  const auto current = Entry("start").Invoke({});
  ASSERT_TRUE(current);
  ASSERT_TRUE(test::CompletesWithInteger(isolate_, *current, 100));
}

TEST_P(JitStateTest, SuspendedOldFramesKeepTheirFieldDependenciesUntilTheyFinish) {
  auto update = Update(
      "class Box { stored: int = 42; } "
      "export func make(): Box { return Box(); } "
      "export async func read(value: Box): Future<int> { await sleep(1); return value.stored; }");
  ASSERT_TRUE(update) << update.error();
  const runtime::RootedValue object(isolate_, *Entry("make").Invoke({}));
  const runtime::RootedValue pending(isolate_, *Entry("read").Invoke(std::array{object.value()}));
  const std::string changed =
      "class Box {} "
      "export func make(): Box { return Box(); } "
      "export async func read(value: Box): Future<int> { await sleep(1); return 0; }";
  update = Update(changed);
  ASSERT_FALSE(update);
  EXPECT_NE(update.error().find("deleted field"), std::string::npos);
  ASSERT_TRUE(test::CompletesWithInteger(isolate_, pending.value(), 42));
  update = Update(changed);
  ASSERT_TRUE(update) << update.error();
}

TEST_P(JitStateTest, SuspendedFramesOnlyKeepFieldAccessesReachableFromTheirAwait) {
  const std::string common =
      "const ready = Completer<void>(); "
      "export func make(): Box { return Box(); } "
      "export func release(): void { ready.complete(); } ";
  ASSERT_TRUE(Update("class Box { stored: int = 42; } " + common +
                     "export async func read(value: Box): Future<int> { "
                     "const result = value.stored; await ready.future; return result; }"));
  const runtime::RootedValue box(isolate_, *Entry("make").Invoke({}));
  const runtime::RootedValue pending(isolate_, *Entry("read").Invoke(std::array{box.value()}));
  auto updated = Update("class Box {} " + common +
                        "export async func read(value: Box): Future<int> { return 0; }");
  ASSERT_TRUE(updated) << updated.error();
  ASSERT_TRUE(Entry("release").Invoke({}));
  ASSERT_TRUE(test::CompletesWithInteger(isolate_, pending.value(), 42));
  isolate_.heap().Collect();
  EXPECT_EQ(compiler_->resource_counts().batches, 1U);
}

TEST_P(JitStateTest, SuspendedDependencyReachabilityIncludesExceptionAndLoopEdges) {
  const std::string common =
      "const ready = Completer<void>(); export func make(): Box { return Box(); } ";
  ASSERT_TRUE(Update("class Box { loop: int = 1; error: int = 42; } " + common +
                     "export async func read(value: Box): Future<int> { "
                     "let result = 0; while (result < 2) { result += value.loop; "
                     "try { await ready.future; } catch (error: Error) { return value.error; } } "
                     "return result; }"));
  const runtime::RootedValue box(isolate_, *Entry("make").Invoke({}));
  const runtime::RootedValue pending(isolate_, *Entry("read").Invoke(std::array{box.value()}));
  for (const auto& fields : {"loop: int = 1;", "error: int = 42;"}) {
    const auto updated = Update("class Box { " + std::string(fields) + " } " + common +
                                "export async func read(value: Box): Future<int> { return 0; }");
    ASSERT_FALSE(updated);
    EXPECT_NE(updated.error().find("deleted field"), std::string::npos);
    EXPECT_EQ(isolate_.native_dispatch().revision(), 1U);
  }
}

TEST_P(JitStateTest, DeletedGlobalsAreCheckedAgainstExplicitVersionHandles) {
  auto update = Update("let stored = 42; export func read(): int { return stored; }");
  ASSERT_TRUE(update) << update.error();
  std::optional<runtime::NativeVersionEntry> reader = *Entry("read").Capture();
  const std::string changed = "export func read(): int { return 0; }";
  update = Update(changed);
  ASSERT_FALSE(update);
  EXPECT_NE(update.error().find("deleted global"), std::string::npos);
  EXPECT_EQ(reader->Invoke({})->AsInteger(), 42);
  reader.reset();
  update = Update(changed);
  ASSERT_TRUE(update) << update.error();
  EXPECT_EQ(Entry("read").Invoke({})->AsInteger(), 0);
}

TEST_P(JitStateTest, AQueuedPublicationMigratesAtAPreciseSafepointAndOldFramesCallNewCode) {
  const std::string common =
      "class Box { stored: int = 40; } "
      "export func make(): Box { return Box(); } ";
  auto update = Update(common +
                       "func helper(value: Box): int { return value.stored; } "
                       "export func caller(value: Box): int { return helper(value); }");
  ASSERT_TRUE(update) << update.error();
  const runtime::RootedValue object(isolate_, *Entry("make").Invoke({}));
  const auto compiled =
      test::CompileUpdate(*compiler_, source_,
                          common +
                              "func helper(value: Box): int { return value.stored + 2; } "
                              "export func caller(value: Box): int { return helper(value) + 100; }",
                          GetParam());
  ASSERT_TRUE(compiled) << compiled.error();
  auto& dispatch = isolate_.native_dispatch();
  auto candidate = dispatch.Prepare(1, compiled->code.image);
  ASSERT_TRUE(candidate);
  bool published = false;
  ASSERT_TRUE(isolate_.native_control().ScheduleSafepoint([&] {
    const auto result = dispatch.Publish(std::move(*candidate));
    ASSERT_TRUE(result) << result.error().message;
    ASSERT_TRUE(source_.Commit(compiled->source));
    published = true;
  }));
  const auto result = Entry("caller").Invoke(std::array{object.value()});
  ASSERT_TRUE(result);
  EXPECT_TRUE(published);
  EXPECT_EQ(result->AsInteger(), 42);
  EXPECT_EQ(Entry("caller").Invoke(std::array{object.value()})->AsInteger(), 142);
  isolate_.heap().Collect();
  EXPECT_EQ(compiler_->resource_counts().batches, 1U);
}

TEST_P(JitStateTest, ConstructionArgumentsRunBeforeTheInitializationBarrierAndBodiesRunInsideIt) {
  ASSERT_TRUE(
      isolate_.RegisterNative(runtime::NativeFunction::Bind<&IsConstructing>("constructing")));
  static_cast<void>(isolate_.FreezeBindings());
  const auto update = Update(
      "external func constructing(): bool; "
      "class Box { private value: int; "
      "constructor(value: int) { this.value = value + (constructing() ? 2 : 0); } "
      "read(): int { return this.value; } } "
      "export func main(): int { return Box(constructing() ? 0 : 40).read(); }");
  ASSERT_TRUE(update) << update.error();
  EXPECT_EQ(Entry("main").Invoke({})->AsInteger(), 42);
  EXPECT_TRUE(isolate_.native_control().CanPublish());
}

TEST_P(JitStateTest, FailedConstructionReleasesItsBarrierBeforeEnteringTheCatchHandler) {
  auto update = Update(
      "class Box { value: int = initial(); } "
      "func initial(): int { const zero = 0; return 1 / zero; } "
      "export func main(): int { try { Box(); } catch (error: Error) { return 42; } return 0; }");
  ASSERT_TRUE(update) << update.error();
  EXPECT_EQ(Entry("main").Invoke({})->AsInteger(), 42);
  EXPECT_TRUE(isolate_.native_control().CanPublish());
  update = Update(
      "class Box { value: int = initial(); } "
      "func initial(): int { return 7; } "
      "export func main(): int { return Box().value; }");
  ASSERT_TRUE(update) << update.error();
  EXPECT_EQ(Entry("main").Invoke({})->AsInteger(), 7);
}

TEST_P(JitStateTest, AwaitingConstructorArgumentsDoesNotSuspendAnInitializationBarrier) {
  const auto update = Update(
      "class Box { value: int; } "
      "export async func main(): Future<int> { "
      "const value = Box(value: await Future.delayed(1, (): int => 42)); return value.value; }");
  ASSERT_TRUE(update) << update.error();
  const runtime::RootedValue result(isolate_, *Entry("main").Invoke({}));
  EXPECT_FALSE(isolate_.native_control().IsInitializing());
  ASSERT_TRUE(test::CompletesWithInteger(isolate_, result.value(), 42));
  EXPECT_FALSE(isolate_.native_control().IsInitializing());
}

TEST_P(JitStateTest, FieldBlockPreparationFailureLeavesEveryObjectAndTheCandidateUnchanged) {
  const std::string common = "export func make(): Box { return Box(); } ";
  auto update = Update("class Box { value: int = 40; } " + common);
  ASSERT_TRUE(update) << update.error();
  const runtime::RootedValue first(isolate_, *Entry("make").Invoke({}));
  const runtime::RootedValue second(isolate_, *Entry("make").Invoke({}));
  const auto compiled = test::CompileUpdate(
      *compiler_, source_, "class Box { value: int = 100; extra: int = 2; } " + common, GetParam());
  ASSERT_TRUE(compiled) << compiled.error();
  auto& dispatch = isolate_.native_dispatch();
  auto candidate = dispatch.Prepare(1, compiled->code.image);
  ASSERT_TRUE(candidate);
  auto& heap = isolate_.heap();
  heap.FinishCollection();
  heap.Collect();
  const auto* original = runtime::Heap::ClassOf(*first.value().AsObject());
  auto* nominal = isolate_.CreateNominalType({});
  auto* probe = isolate_.CreateClassLayout(
      2, {},
      {
          .callable = {},
          .interfaces = {},
          .dispatch_targets = {},
          .nominal = nominal,
          .storage = runtime::ClassStorage::Reloadable,
          .field_declarations = {original->field_declarations().front(), {}},
      });
  const auto bytes = heap.allocated_bytes();
  std::size_t block_bytes = 0;
  {
    const auto migration = heap.PrepareInstanceMigration(*first.value().AsObject(), *probe);
    ASSERT_NE(migration, nullptr);
    block_bytes = heap.allocated_bytes() - bytes;
  }
  heap.SetAllocationLimit(bytes + block_bytes);
  const auto failed = dispatch.Publish(std::move(*candidate));
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error().status, runtime::NativePublicationStatus::InstantiationFailed);
  EXPECT_EQ(dispatch.revision(), 1U);
  EXPECT_EQ(runtime::Heap::ClassOf(*first.value().AsObject()), original);
  EXPECT_EQ(runtime::Heap::ClassOf(*second.value().AsObject()), original);
  EXPECT_EQ(runtime::Heap::GetField(*first.value().AsObject(), 0)->AsInteger(), 40);
  EXPECT_EQ(runtime::Heap::GetField(*second.value().AsObject(), 0)->AsInteger(), 40);
  EXPECT_EQ(heap.allocated_bytes(), bytes);
  heap.SetAllocationLimit(std::numeric_limits<std::size_t>::max());
  ASSERT_TRUE(dispatch.Publish(std::move(*candidate)));
  ASSERT_TRUE(source_.Commit(compiled->source));
  EXPECT_EQ(dispatch.revision(), 2U);
}

TEST_P(JitStateTest, EnumKeysAndDynamicEqualityUseNominalIdentityAcrossLayoutVersions) {
  const std::string common =
      "enum Shade { light, dark } "
      "const saved = Map<dynamic, int>(); "
      "export func save(): void { saved.set(Shade.dark, 40); } "
      "export func read(): int { return saved.get(Shade.dark) ?? 0; } ";
  auto update = Update(common + "export func main(): int { return read(); }");
  ASSERT_TRUE(update) << update.error();
  ASSERT_TRUE(Entry("save").Invoke({}));
  update = Update(common + "export func main(): int { return read() + 2; }");
  ASSERT_TRUE(update) << update.error();
  EXPECT_EQ(Entry("main").Invoke({})->AsInteger(), 42);
}

TEST_P(JitStateTest, FieldPrivacyRejectsRetainedForeignReadersAndAllowsRetainedClassLambdas) {
  auto update = Update(
      "class Box { stored: int = 42; "
      "reader(): () => int { return (): int => this.stored; } "
      "read(): int { return this.stored; } } "
      "export func make(): Box { return Box(); } "
      "export func read(value: Box): int { return value.stored; } "
      "export func callback(value: Box): () => int { return value.reader(); } "
      "export func invoke(value: () => int): int { return value(); }");
  ASSERT_TRUE(update) << update.error();
  const runtime::RootedValue object(isolate_, *Entry("make").Invoke({}));
  const runtime::RootedValue callback(isolate_,
                                      *Entry("callback").Invoke(std::array{object.value()}));
  std::optional<runtime::NativeVersionEntry> reader = *Entry("read").Capture();
  const std::string changed =
      "class Box { private stored: int = 100; "
      "reader(): () => int { return (): int => this.stored; } "
      "read(): int { return this.stored; } } "
      "export func make(): Box { return Box(); } "
      "export func read(value: Box): int { return value.read(); } "
      "export func callback(value: Box): () => int { return value.reader(); } "
      "export func invoke(value: () => int): int { return value(); }";
  update = Update(changed);
  ASSERT_FALSE(update);
  EXPECT_NE(update.error().find("private field"), std::string::npos);
  reader.reset();
  update = Update(changed);
  ASSERT_TRUE(update) << update.error();
  EXPECT_EQ(Entry("invoke").Invoke(std::array{callback.value()})->AsInteger(), 42);
}

TEST_P(JitStateTest, RetainedCallsRejectInstanceToStaticBindingChanges) {
  auto update = Update(
      "class Box { read(): int { return 42; } } "
      "export func call(value: Box): int { return value.read(); }");
  ASSERT_TRUE(update) << update.error();
  std::optional<runtime::NativeVersionEntry> caller = *Entry("call").Capture();
  const std::string changed =
      "class Box { static read(): int { return 42; } } "
      "export func call(value: Box): int { return Box.read(); }";
  update = Update(changed);
  ASSERT_FALSE(update);
  EXPECT_NE(update.error().find("binding kind"), std::string::npos);
  caller.reset();
  update = Update(changed);
  ASSERT_TRUE(update) << update.error();
}

TEST_P(JitStateTest, AddingAnOverrideRejectsRetainedCallsThatWouldChangeTheirStaticTarget) {
  const std::string common = "class Base { read(): int { return 42; } } ";
  auto update = Update(common +
                       "class Child extends Base {} "
                       "export func call(value: Child): int { return value.read(); }");
  ASSERT_TRUE(update) << update.error();
  std::optional<runtime::NativeVersionEntry> caller = *Entry("call").Capture();
  const std::string changed = common +
                              "class Child extends Base { override read(): int { return 100; } } "
                              "export func call(value: Child): int { return value.read(); }";
  update = Update(changed);
  ASSERT_FALSE(update);
  EXPECT_NE(update.error().find("another declaration"), std::string::npos);
  caller.reset();
  update = Update(changed);
  ASSERT_TRUE(update) << update.error();
}

TEST_P(JitStateTest, SuperDependenciesUseTheLexicalParentIncludingItsInheritedMethods) {
  const std::string common = "class Grand { read(): int { return 40; } } ";
  const std::string child =
      "class Child extends Base { override read(): int { return super.read() + 2; } } "
      "export func make(): Child { return Child(); } "
      "export func callback(value: Child): () => int { return value.read; } "
      "export func invoke(value: () => int): int { return value(); }";
  auto update = Update(common + "class Base extends Grand {} " + child);
  ASSERT_TRUE(update) << update.error();
  const runtime::RootedValue object(isolate_, *Entry("make").Invoke({}));
  std::optional<runtime::RootedValue> callback;
  callback.emplace(isolate_, *Entry("callback").Invoke(std::array{object.value()}));
  const std::string changed =
      common + "class Base extends Grand { override read(): int { return 100; } } " + child;
  update = Update(changed);
  ASSERT_FALSE(update);
  EXPECT_NE(update.error().find("another declaration"), std::string::npos);
  callback.reset();
  update = Update(changed);
  ASSERT_TRUE(update) << update.error();
  const runtime::RootedValue current(isolate_,
                                     *Entry("callback").Invoke(std::array{object.value()}));
  EXPECT_EQ(Entry("invoke").Invoke(std::array{current.value()})->AsInteger(), 102);
}

TEST_P(JitStateTest, RetainedOldCallersUseCurrentGenericBodiesWithoutNewSourceReferences) {
  auto update = Update(
      "func value<T>(input: T): int { return 40; } "
      "export func read(): int { return value<int>(0) + 2; }");
  ASSERT_TRUE(update) << update.error();
  const auto reader = Entry("read").Capture();
  ASSERT_TRUE(reader);
  update = Update(
      "func value<T>(input: T): int { return 100; } "
      "export func read(): int { return 0; }");
  ASSERT_TRUE(update) << update.error();
  EXPECT_EQ(reader->Invoke({})->AsInteger(), 102);
}

TEST_P(JitStateTest, LiveGenericObjectsAndEmptyTypedContainersRetainCurrentLayouts) {
  const std::string initial =
      "class Box<T> { value: int = 40; read(): int { return this.value; } } ";
  auto update = Update(initial +
                       "export func make(): Box<int> { return Box<int>(); } "
                       "export func empty(): Box<string>[] { return []; }");
  ASSERT_TRUE(update) << update.error();
  const runtime::RootedValue object(isolate_, *Entry("make").Invoke({}));
  const runtime::RootedValue array(isolate_, *Entry("empty").Invoke({}));
  const auto* nominal = &runtime::Heap::ClassOf(*object.value().AsObject())->nominal_type();
  const auto* element = runtime::Heap::ArrayTypeOf(*array.value().AsObject())->element.nominal;
  ASSERT_NE(element, nullptr);
  update = Update(
      "class Box<T> { value: int = 100; added: int = 2; read(): int { return this.value + "
      "this.added; } } "
      "export func read(): int { return 0; }");
  ASSERT_TRUE(update) << update.error();
  EXPECT_EQ(runtime::Heap::ClassOf(*object.value().AsObject()),
            &isolate_.native_dispatch().CurrentLayout(*nominal));
  EXPECT_EQ(isolate_.native_dispatch().CurrentLayout(*element).field_count(), 2U);
}

TEST_P(JitStateTest, AGenericDependencyCreatedAfterCompilationRequiresRepreparingTheCandidate) {
  auto update = Update(
      "func value<T>(input: T): int { return 40; } "
      "export func old(): () => int { return () => value<int>(0); }");
  ASSERT_TRUE(update) << update.error();
  const auto compiled = test::CompileUpdate(*compiler_, source_,
                                            "func value<T>(input: T): int { return 42; } "
                                            "export func old(): () => int { return () => 0; }",
                                            GetParam());
  ASSERT_TRUE(compiled) << compiled.error();
  auto& dispatch = isolate_.native_dispatch();
  auto candidate = dispatch.Prepare(1, compiled->code.image);
  ASSERT_TRUE(candidate);
  const runtime::RootedValue callback(isolate_, *Entry("old").Invoke({}));
  const auto published = dispatch.Publish(std::move(*candidate));
  ASSERT_FALSE(published);
  EXPECT_EQ(published.error().status, runtime::NativePublicationStatus::NeedsCompilation);
  EXPECT_EQ(dispatch.revision(), 1U);
  EXPECT_EQ(source_.snapshot()->revision, 1U);
  update = Update(*compiled->source.snapshot().sources.at("main.vela"));
  ASSERT_TRUE(update) << update.error();
}

TEST_P(JitStateTest, RepeatedPublicationReclaimsVersionLayoutsAndPreservesEnumAllocationLayouts) {
  const std::string source =
      "enum Mode { first, second } "
      "class Box { value: int = 42; } "
      "export func make(): Box { return Box(); } "
      "export func mode(): Mode { return Mode.second; } "
      "export func callback(): () => int { let value = 42; return () => value++; }";
  auto update = Update(source);
  ASSERT_TRUE(update) << update.error();
  const runtime::RootedValue object(isolate_, *Entry("make").Invoke({}));
  const auto enumeration = Entry("mode").Invoke({});
  ASSERT_TRUE(enumeration);
  const auto* enum_layout = runtime::Heap::ClassOf(*enumeration->AsObject());
  const auto initial = isolate_.layout_counts();
  constexpr std::uint32_t kMetadataReloads = 12;
  for (std::uint32_t iteration = 0; iteration < kMetadataReloads; ++iteration) {
    update = Update(source);
    ASSERT_TRUE(update) << update.error();
    isolate_.heap().Collect();
    EXPECT_EQ(isolate_.layout_counts(), initial);
    EXPECT_EQ(compiler_->resource_counts().batches, 1U);
    const auto current = Entry("mode").Invoke({});
    ASSERT_TRUE(current);
    EXPECT_EQ(runtime::Heap::ClassOf(*current->AsObject()), enum_layout);
  }
}

TEST_P(JitStateTest, AnOldWrapperKeepsItsOwnLayoutWhileCapturingANewerFunctionEnvironment) {
  const std::string source =
      "func answer(): int { return 40; } "
      "export func factory(): () => int { return answer; } "
      "export func invoke(value: () => int): int { return value(); }";
  auto update = Update(source);
  ASSERT_TRUE(update) << update.error();
  std::optional<runtime::NativeVersionEntry> factory = *Entry("factory").Capture();
  update = Update(
      "func answer(): int { return 42; } "
      "export func factory(): () => int { return answer; } "
      "export func invoke(value: () => int): int { return value(); }");
  ASSERT_TRUE(update) << update.error();
  std::optional<runtime::RootedValue> callback;
  callback.emplace(isolate_, *factory->Invoke({}));
  factory.reset();
  isolate_.heap().Collect();
  EXPECT_EQ(compiler_->resource_counts().batches, 2U);
  const auto result = Entry("invoke").Invoke(std::array{callback->value()});
  ASSERT_TRUE(result);
  EXPECT_EQ(result->AsInteger(), 42);
  const auto retained = isolate_.layout_counts();
  callback.reset();
  isolate_.heap().Collect();
  EXPECT_EQ(compiler_->resource_counts().batches, 1U);
  EXPECT_LT(isolate_.layout_counts().classes, retained.classes);
  EXPECT_LT(isolate_.layout_counts().code_identities, retained.code_identities);
}

TEST_P(JitStateTest, FailedConstantPreparationReleasesObjectsBeforeRollingBackTheirLayouts) {
  auto update = Update(
      "class Box { value: int = 42; } "
      "export func answer(): int { return Box().value; }");
  ASSERT_TRUE(update) << update.error();
  const auto compiled = test::CompileUpdate(*compiler_, source_,
                                            "class Box { value: int = 100; } "
                                            "export func answer(): int { return Box().value; } "
                                            "export func text(): string { return '" +
                                                std::string(4096, 'x') + "'; }",
                                            GetParam());
  ASSERT_TRUE(compiled) << compiled.error();
  isolate_.heap().Collect();
  const auto layouts = isolate_.layout_counts();
  const auto bytes = isolate_.heap().allocated_bytes();
  for (const auto allowance : std::array<std::size_t, 4>{0, 64, 128, 256}) {
    isolate_.heap().SetAllocationLimit(bytes + allowance);
    const auto candidate = isolate_.native_dispatch().Prepare(1, compiled->code.image);
    ASSERT_FALSE(candidate);
    EXPECT_EQ(candidate.error().status, runtime::NativePublicationStatus::InstantiationFailed);
    isolate_.heap().SetAllocationLimit(std::numeric_limits<std::size_t>::max());
    isolate_.heap().Collect();
    EXPECT_EQ(isolate_.layout_counts(), layouts);
    EXPECT_EQ(isolate_.heap().allocated_bytes(), bytes);
    EXPECT_EQ(isolate_.native_dispatch().revision(), 1U);
    EXPECT_EQ(Entry("answer").Invoke({})->AsInteger(), 42);
    isolate_.heap().Collect();
  }
}

TEST_P(JitStateTest, StaticFieldPrivacyChecksRetainedAccessesUsingTheirLexicalClass) {
  const std::string methods =
      "static callback(): () => int { return () => Box.value; } } "
      "export func factory(): () => int { return Box.callback(); } "
      "export func invoke(value: () => int): int { return value(); } ";
  auto update = Update("class Box { static value: int = 42; " + methods +
                       "export func read(): int { return Box.value; }");
  ASSERT_TRUE(update) << update.error();
  std::optional<runtime::NativeVersionEntry> external = *Entry("read").Capture();
  EXPECT_EQ(external->Invoke({})->AsInteger(), 42);
  const runtime::RootedValue internal(isolate_, *Entry("factory").Invoke({}));
  const auto changed = "class Box { private static value: int = 100; " + methods +
                       "export func read(): int { return 0; }";
  update = Update(changed);
  ASSERT_FALSE(update);
  EXPECT_NE(update.error().find("private static field"), std::string::npos);
  external.reset();
  update = Update(changed);
  ASSERT_TRUE(update) << update.error();
  const auto value = Entry("invoke").Invoke(std::array{internal.value()});
  ASSERT_TRUE(value);
  EXPECT_EQ(value->AsInteger(), 42);
}

TEST_P(JitStateTest, FieldStorageChangesRequireReloadWithoutRetainedAccessorsOrLiveInstances) {
  auto update = Update("class Box { value: int = 42; }");
  ASSERT_TRUE(update) << update.error();
  update = Update("class Box { static value: int = 42; }");
  ASSERT_FALSE(update);
  EXPECT_NE(update.error().find("instance/static storage"), std::string::npos);
  EXPECT_EQ(isolate_.native_dispatch().revision(), 1U);
}

TEST_P(JitStateTest, DiscardedCandidatesReleaseNewNominalsEnumsAndInternedSchemas) {
  ASSERT_TRUE(Update("export func answer(): int { return 42; }"));
  isolate_.heap().Collect();
  const auto initial = isolate_.layout_counts();
  const auto compiled = test::CompileUpdate(
      *compiler_, source_,
      "interface Readable { read(): int; } "
      "enum Mode { first, second } "
      "class Added implements Readable { "
      "value: int = 42; read(): int { return this.value; } } "
      "export func answer(): int { return 42; } "
      "export func values(): Added[] { return []; } "
      "export func lookup(): Map<string, Added> { return Map<string, Added>(); } "
      "export func future(value: Added): Future<Added> { return Future.value(value); } "
      "export func mode(): Mode { return Mode.second; }",
      GetParam());
  ASSERT_TRUE(compiled) << compiled.error();
  constexpr std::uint32_t kDiscardedCandidates = 6;
  for (std::uint32_t iteration = 0; iteration < kDiscardedCandidates; ++iteration) {
    {
      const auto candidate = isolate_.native_dispatch().Prepare(1, compiled->code.image);
      ASSERT_TRUE(candidate);
      isolate_.heap().Collect();
      EXPECT_GT(isolate_.layout_counts().nominal_types, initial.nominal_types);
    }
    isolate_.heap().Collect();
    EXPECT_EQ(isolate_.layout_counts(), initial);
    EXPECT_EQ(Entry("answer").Invoke({})->AsInteger(), 42);
  }
  // 自动 GC 常在实例化事务内完成；事务退出后也必须兑现延迟的元数据清扫。
  constexpr std::uint32_t kAutomaticGcAttempts = 64;
  for (std::uint32_t iteration = 0; iteration < kAutomaticGcAttempts; ++iteration) {
    const auto candidate = isolate_.native_dispatch().Prepare(1, compiled->code.image);
    ASSERT_TRUE(candidate);
  }
  constexpr std::size_t kTransientNominalLimit = 8;
  EXPECT_LE(isolate_.layout_counts().nominal_types, initial.nominal_types + kTransientNominalLimit);
  isolate_.heap().Collect();
  EXPECT_EQ(isolate_.layout_counts(), initial);
  auto accepted = isolate_.native_dispatch().Prepare(1, compiled->code.image);
  ASSERT_TRUE(accepted);
  ASSERT_TRUE(isolate_.native_dispatch().Publish(std::move(*accepted)));
  const auto enumeration = Entry("mode").Invoke({});
  ASSERT_TRUE(enumeration);
  EXPECT_TRUE(runtime::Heap::ClassOf(*enumeration->AsObject())->IsEnum());
}

TEST_P(JitStateTest, HostRequestedInternedSchemasKeepTheirDependenciesAfterNativeCodeRetires) {
  ASSERT_TRUE(
      Update("class Box { value: int = 42; } "
             "export func make(): Box { return Box(); } "
             "export func values(): Box[] { return []; }"));
  const auto value = Entry("make").Invoke({});
  ASSERT_TRUE(value);
  const auto type = runtime::Heap::TypeOf(*value);
  const auto declaration = type.nominal->declaration();
  const auto* layout = isolate_.CreateArrayLayout({.element = type});
  isolate_.native_dispatch().Close();
  isolate_.heap().Collect();
  EXPECT_EQ(compiler_->resource_counts().batches, 0U);
  EXPECT_EQ(layout->element.nominal->declaration(), declaration);
  const runtime::RootedValue array(
      isolate_, runtime::Value::Object(isolate_.heap().AllocateArray(*layout, 0)));
  isolate_.heap().Collect();
  EXPECT_EQ(runtime::Heap::ArrayTypeOf(*array.value().AsObject()), layout);
}

TEST_P(JitStateTest, EmptyTypedContainersAndFutureSignaturesKeepMetadataWithoutRetainingCode) {
  const auto initial = isolate_.layout_counts();
  ASSERT_TRUE(
      Update("interface Readable { read(): int; } "
             "class Box implements Readable { read(): int { return 42; } } "
             "export func values(): Box[] { return []; } "
             "export func lookup(): Map<string, Box> { return Map<string, Box>(); } "
             "export func future(): Future<Box?> { const value: Box? = null; "
             "return Future.value(value); }"));
  {
    const runtime::RootedValue array(isolate_, *Entry("values").Invoke({}));
    const runtime::RootedValue map(isolate_, *Entry("lookup").Invoke({}));
    const runtime::RootedValue future(isolate_, *Entry("future").Invoke({}));
    const auto* nominal = runtime::Heap::ArrayTypeOf(*array.value().AsObject())->element.nominal;
    const auto declaration = nominal->declaration();
    isolate_.native_dispatch().Close();
    isolate_.heap().Collect();
    EXPECT_EQ(compiler_->resource_counts().batches, 0U);
    EXPECT_EQ(nominal->declaration(), declaration);
    EXPECT_EQ(nominal->interfaces().size(), 1U);
    const auto* map_type = runtime::Heap::CollectionTypeOf(*map.value().AsObject());
    ASSERT_NE(map_type, nullptr);
    const auto& map_values = map_type->values;
    if (!map_values) {
      FAIL() << "Map value type was not retained";
    }
    EXPECT_EQ(map_values->element.nominal, nominal);
    const auto* future_type = runtime::Heap::TypeOf(future.value()).signature;
    ASSERT_NE(future_type, nullptr);
    const auto& future_result = future_type->result;
    if (!future_result) {
      FAIL() << "Future result type was not retained";
    }
    EXPECT_EQ(future_result->nominal, nominal);
  }
  isolate_.heap().Collect();
  EXPECT_EQ(isolate_.layout_counts(), initial);
}

INSTANTIATE_TEST_SUITE_P(Tiers,
                         JitStateTest,
                         testing::Values(NativeCodeTier::Baseline, NativeCodeTier::Optimized));

}  // namespace
}  // namespace vela::compiler::jit
