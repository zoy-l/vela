"""Direct AOT callbacks retain stable snapshots, captures and failure semantics."""

SOURCE = """
external func collect(): void;
enum Choice { first, second }
class Item { key: int; id: int; }
class Box { number: int; next: Box? = null; }
func lookup(values: int[], index: int): int { return values[index]; }
func update(box: Box, next: Box?, divisor: int): int {
  box.next = next;
  return box.number / divisor;
}
func pickEnum(operation: int): Choice? {
  if (operation == 0) { return null; }
  return Choice.second;
}
export func calculate(operation: int): int {
  if (operation == 0) {
    const items = [Item(key: 2, id: 0), Item(key: 1, id: 1),
                   Item(key: 2, id: 2), Item(key: 1, id: 3)];
    const direction = 1;
    const sorted = items.sort((left, right) => direction * (left.key - right.key));
    return sorted[0].id == 1 && sorted[1].id == 3 &&
           sorted[2].id == 0 && sorted[3].id == 2 ? 42 : 0;
  }
  if (operation == 1) {
    const values = [3, 1, 2];
    values.sort((left, right) => { values[0] = 99; values.reserve(200); collect(); return left - right; });
    return values[0] == 1 && values[1] == 2 && values[2] == 3 ? 42 : 0;
  }
  if (operation == 2) {
    const values = [3, 1, 2];
    let calls = 0;
    try {
      values.sort((left, right) => { calls++; values.push(4); values.pop(); return left - right; });
    } catch (error: Error) {
      collect();
      return error.code == 'ConcurrentModification' && calls == 1 && values.length == 3 &&
             values[0] == 3 && values[1] == 1 && values[2] == 2 ? 42 : 0;
    }
    return 0;
  }
  if (operation == 3) {
    const values = [3, 1, 2];
    try { values.sort((left, right) => { values[0] = 99; return (left - right) / (operation - 3); }); }
    catch (error: Error) {
      collect();
      return error.code == 'DivisionByZero' && values[0] == 99 && values[1] == 1 &&
             values[2] == 2 ? 42 : 0;
    }
    return 0;
  }
  if (operation == 4) { return lookup([42], -1); }
  if (operation == 5) {
    const present: dynamic = [1]; const absent: dynamic = null;
    const missing: int? = null; const zero: int? = 0; const text: string? = null;
    const box: Box? = null;
    return present != null && null == absent && missing == null && zero != null &&
           null == text && box == null ? 42 : 0;
  }
  if (operation == 6) {
    let callback = (value: int): int => value + 1;
    if (operation == 6) { callback = (value: int): int => value + 2; }
    return callback(40);
  }
  if (operation == 7) {
    let offset = 1;
    const callback = (value: int): int => value + offset;
    offset = 2;
    return callback(40);
  }
  if (operation == 8) {
    const values = [1, 2, 3]; const offset = 12;
    return values.map(value => value + offset).reduce((sum, value) => sum + value, 0);
  }
  if (operation == 9) {
    const values = [2.0, 1.0, 3.0];
    values.sort(() => Math.pow(-1.0, 0.5));
    return values[0] == 2.0 && values[1] == 1.0 && values[2] == 3.0 ? 42 : 0;
  }
  if (operation == 11) { return pickEnum(operation) == Choice.second ? 42 : 0; }
  const box = Box(number: 42); const child = Box(number: 42);
  try { return update(box, child, operation - 10); }
  catch (error: Error) {
    collect();
    return error.code == 'DivisionByZero' && box.next == child ? child.number : 0;
  }
}

export func savedCallbacks(): ((int) => int)[] {
  let delta = 1;
  const read = (n: int): int => n + delta;
  const write = (n: int): int => { delta = n; return delta; };
  return [read, write];
}
"""

HOST_TESTS = r'''
#include <vela/host.h>
#include "runtime/native/native_call.h"
extern "C" const NativeImage* CALLBACK_IMAGE() noexcept;
using NativeCallbackTest = CollectingImageTest<CALLBACK_IMAGE>;

TEST_F(NativeCallbackTest, StableSnapshotsAndMutatingCallbacksKeepTheirContractsUnderGc) {
  NativeExecution execution(*instance);
  ExpectIntegerOperations(CALLBACK_ENTRY, execution, {0, 1, 2, 3, 9, 10});
}

TEST_F(NativeCallbackTest, SavedScalarCallbackReadsLiveCaptureAndPreservesFailure) {
  using Callback = vela::Function<std::int64_t(std::int64_t)>;
  const auto factory = vela::Entry<vela::Array<Callback>()>::Bind(*instance, "savedCallbacks");
  ASSERT_TRUE(factory);
  vela::Persistent<Callback> saved;
  {
    vela::HandleScope registration(isolate);
    const auto pair = factory->Call(registration);
    ASSERT_TRUE(pair);
    const auto reader = pair->Get(registration, 0);
    const auto writer = pair->Get(registration, 1);
    ASSERT_TRUE(reader);
    ASSERT_TRUE(writer);
    auto saved_reader = reader->Persist();
    ASSERT_TRUE(saved_reader);
    EXPECT_EQ(saved_reader->Call(std::int64_t{41}), 42);
    EXPECT_EQ(writer->Call(registration, std::int64_t{7}), 7);
    saved = std::move(*saved_reader);
  }
  isolate.heap().Collect();
  EXPECT_EQ(saved.Call(std::int64_t{35}), 42);
  const auto failure = saved.Call(std::numeric_limits<std::int64_t>::max());
  ASSERT_FALSE(failure);
  EXPECT_EQ(failure.error().status, ExecutionStatus::IntegerOverflow);
  EXPECT_FALSE(failure.error().stack_trace.empty());
}

TEST_F(NativeCallbackTest, NullPresenceReassignmentAndMutableCapturesUseTheirCurrentValues) {
  NativeExecution execution(*instance);
  ExpectIntegerOperations(CALLBACK_ENTRY, execution, {5, 6, 7, 8, 11});
}

TEST_F(NativeCallbackTest, LocalizedCallbacksSurviveIndependentPersistentRelease) {
  using Callback = vela::Function<std::int64_t(std::int64_t)>;
  const auto factory = vela::Entry<vela::Array<Callback>()>::Bind(*instance, "savedCallbacks");
  ASSERT_TRUE(factory);
  vela::Persistent<Callback> reader;
  vela::Persistent<Callback> writer;
  {
    vela::HandleScope registration(isolate);
    const auto pair = factory->Call(registration);
    ASSERT_TRUE(pair);
    const auto read = pair->Get(registration, 0);
    const auto write = pair->Get(registration, 1);
    ASSERT_TRUE(read);
    ASSERT_TRUE(write);
    auto saved_read = read->Persist();
    auto saved_write = write->Persist();
    ASSERT_TRUE(saved_read);
    ASSERT_TRUE(saved_write);
    reader = std::move(*saved_read);
    writer = std::move(*saved_write);
  }
  vela::HandleScope event(isolate);
  const auto local_read = reader.ToLocal(event);
  const auto local_write = writer.ToLocal(event);
  ASSERT_TRUE(local_read);
  ASSERT_TRUE(local_write);
  reader.Reset();
  writer.Reset();
  isolate.heap().Collect();
  EXPECT_EQ(local_write->Call(event, std::int64_t{7}), 7);
  EXPECT_EQ(local_read->Call(event, std::int64_t{35}), 42);
  const auto failure = local_read->Call(event, std::numeric_limits<std::int64_t>::max());
  ASSERT_FALSE(failure);
  EXPECT_EQ(failure.error().status, ExecutionStatus::IntegerOverflow);
  EXPECT_FALSE(failure.error().stack_trace.empty());
  std::int64_t observations = 0;
  ASSERT_TRUE(isolate.native_control().endpoint().Post([&] {
    ++observations;
    isolate.heap().Collect();
  }));
  EXPECT_EQ(local_read->Call(event, std::int64_t{35}), 42);
  EXPECT_EQ(observations, 1);
  const auto saved = local_read->Persist();
  ASSERT_TRUE(saved);
  isolate.native_control().RequestStop();
  const auto stopped = local_read->Call(event, std::int64_t{35});
  ASSERT_FALSE(stopped);
  EXPECT_EQ(stopped.error().status, ExecutionStatus::ExecutionStopped);
  const auto saved_stopped = saved->Call(std::int64_t{35});
  ASSERT_FALSE(saved_stopped);
  EXPECT_EQ(saved_stopped.error().status, ExecutionStatus::ExecutionStopped);
}

TEST_F(NativeCallbackTest, ColdReferenceLeafFailuresKeepTheirOriginalSourceAndCallerTrace) {
  NativeExecution execution(*instance);
  const std::array arguments{Integer(4)};
  NativeValue result;
  ASSERT_EQ(Execute(CALLBACK_ENTRY, execution, arguments, result), ExecutionStatus::IndexOutOfBounds);
  EXPECT_EQ(execution.current_frame(), nullptr);
  ASSERT_TRUE(execution.HasException());
  isolate.heap().Collect();
  const auto diagnostic = execution.ExceptionDiagnostic();
  const auto& trace = diagnostic.stack_trace;
  ASSERT_EQ(trace.size(), 2U);
  EXPECT_EQ(trace.front().function, "lookup");
  EXPECT_EQ(trace.front().line, 6U);
  EXPECT_EQ(trace.back().function, "calculate");
}

TEST_F(NativeCallbackTest, EnumResultMetadataBoxesScalarPayloadsAndPreservesNullableResults) {
  NativeExecution execution(*instance);
  const auto functions = image->functions.values();
  const auto found = std::ranges::find(functions, "pickEnum", [](const auto& function) {
    return function.name.text();
  });
  ASSERT_NE(found, functions.end());
  EXPECT_NE(found->flags & kNativeEnumResult, 0U);
  const auto index = static_cast<std::uint32_t>(found - functions.begin());
  const std::array absent{Integer(0)};
  const auto null_result = InvokeNativeTarget(execution, index, absent);
  ASSERT_TRUE(null_result.has_value());
  EXPECT_TRUE(null_result->IsNull());
  const std::array present{Integer(1)};
  auto result = InvokeNativeTarget(execution, index, present);
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(result->IsObject());
  RootRange root(isolate.heap(), &*result, 1);
  isolate.heap().Collect();
  const auto* object = result->AsObject();
  ASSERT_NE(Heap::ClassOf(*object), nullptr);
  EXPECT_TRUE(Heap::ClassOf(*object)->IsEnum());
  EXPECT_EQ(Heap::GetField(*object, 0)->AsInteger(), 1);
  EXPECT_EQ(execution.current_frame(), nullptr);
}
'''
