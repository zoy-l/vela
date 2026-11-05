#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "base/execution_limits.h"
#include "base/execution_status.h"
#include "base/native_program_mode.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/jit/compiler.h"
#include "compiler/jit/source_compiler.h"
#include "compiler/jit/test_support.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_call.h"
#include "runtime/native/native_dispatch.h"
#include "runtime/native/native_execution.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_instance.h"
#include "runtime/value/value.h"

namespace vela::compiler::jit {
namespace {

class RecursiveCallTest
    : public testing::TestWithParam<std::tuple<NativeProgramMode, NativeCodeTier>> {
 protected:
  void Load(const std::string& source) {
    auto compiler = Compiler::Create();
    ASSERT_TRUE(compiler);
    const auto [mode, tier] = GetParam();
    if (mode == NativeProgramMode::Closed) {
      const auto compiled =
          CompileSource(**compiler, source, {.diagnostics = {}, .compilation = {.tier = tier}});
      ASSERT_TRUE(compiled.Succeeded()) << frontend::DiagnosticsText(compiled.diagnostics);
      auto instance = runtime::NativeInstance::Instantiate(isolate_, compiled.code.image);
      ASSERT_TRUE(instance);
      closed_instance_ = std::move(*instance);
    } else {
      const auto compiled = test::CompileText(**compiler, source, tier);
      ASSERT_TRUE(compiled) << frontend::DiagnosticsText(compiled.error());
      ASSERT_TRUE(test::PublishImage(isolate_, compiled->image));
    }
  }

  runtime::NativeInstance BorrowInstance() {
    return runtime::NativeInstance::Borrow(closed_instance_ != nullptr
                                               ? *closed_instance_->state().front().AsObject()
                                               : isolate_.native_dispatch().current_state());
  }
  runtime::Isolate& isolate() { return isolate_; }

 private:
  runtime::Isolate isolate_{1};
  std::unique_ptr<runtime::NativeInstance> closed_instance_;
};

TEST_P(RecursiveCallTest, EarlyReturnsPreserveRecursiveFailuresAndInstanceReuse) {
  ASSERT_NO_FATAL_FAILURE(Load(R"(
export func fibonacci(value: int): int {
  if (value < 2) { return value; }
  return fibonacci(value - 1) + fibonacci(value - 2);
}
func divide(depth: int, divisor: int): int {
  if (depth == 0) { return 84 / divisor; }
  return divide(depth - 1, divisor);
}
export func fail(): int { return divide(3, 0); }
export func caught(): int {
  try { return divide(3, 0); }
  catch (error: Error) { return error.code == 'DivisionByZero' ? fibonacci(10) : 0; }
}
export func overflow(depth: int): int {
  if (depth == 0) { return 9223372036854775807; }
  return overflow(depth - 1) + 1;
}
)"));
  auto instance = BorrowInstance();
  const auto fail = instance.FindFunction("fail");
  const auto caught = instance.FindFunction("caught");
  const auto fibonacci = instance.FindFunction("fibonacci");
  const auto overflow = instance.FindFunction("overflow");
  if (!fail || !caught || !fibonacci || !overflow) {
    FAIL() << "missing recursion entry";
  }
  const auto failed = runtime::ExecuteNativeFunction(instance, *fail);
  EXPECT_EQ(failed.status, ExecutionStatus::DivisionByZero);
  ASSERT_EQ(failed.stack_trace.size(), 5U);
  for (std::size_t index = 0; index < 4; ++index) {
    EXPECT_EQ(failed.stack_trace.at(index).function, "divide");
  }
  EXPECT_EQ(failed.stack_trace.back().function, "fail");
  const auto handled = runtime::ExecuteNativeFunction(instance, *caught);
  ASSERT_TRUE(handled.Succeeded());
  EXPECT_EQ(handled.value.AsInteger(), 55);
  runtime::NativeExecution execution(instance);
  const std::array depth{runtime::Value::Integer(3)};
  const auto exceeded = runtime::InvokeNativeFunction(execution, *overflow, depth);
  ASSERT_FALSE(exceeded);
  EXPECT_EQ(exceeded.error(), ExecutionStatus::IntegerOverflow);
  const auto overflow_trace = runtime::ExecuteNativeFunction(instance, *overflow, depth);
  EXPECT_EQ(overflow_trace.status, ExecutionStatus::IntegerOverflow);
  ASSERT_EQ(overflow_trace.stack_trace.size(), 3U);
  for (const auto& frame : overflow_trace.stack_trace) {
    EXPECT_EQ(frame.function, "overflow");
  }
  for (const auto [input, expected] : {std::pair{-1, -1}, {0, 0}, {1, 1}, {2, 1}, {10, 55}}) {
    const std::array arguments{runtime::Value::Integer(input)};
    const auto result = runtime::InvokeNativeFunction(execution, *fibonacci, arguments);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->AsInteger(), expected);
    EXPECT_EQ(execution.current_frame(), nullptr);
  }
}

TEST_P(RecursiveCallTest, EarlyReturnsAndInlinedCallsKeepTheExactDepthLimit) {
  ASSERT_NO_FATAL_FAILURE(Load(R"(
func leaf(value: int): int { return value + 1; }
export func descend(depth: int): int {
  if (depth == 0) { return 42; }
  return descend(depth - 1);
}
export func throughLeaf(): int { return leaf(41); }
)"));
  auto instance = BorrowInstance();
  const auto descend = instance.FindFunction("descend");
  const auto through_leaf = instance.FindFunction("throughLeaf");
  if (!descend || !through_leaf) {
    FAIL() << "missing stack depth entry";
  }
  runtime::NativeExecution execution(instance);
  std::vector<runtime::NativeTraceFrame> outer(kMaxCallFrames - 1);
  for (auto& frame : outer) {
    execution.MaterializeFrame(frame, *descend);
  }
  const std::array zero{runtime::Value::Integer(0)};
  const auto returned = runtime::InvokeNativeFunction(execution, *descend, zero);
  ASSERT_TRUE(returned);
  EXPECT_EQ(returned->AsInteger(), 42);
  for (const std::int64_t depth : {1, 2, 3, 4}) {
    const std::array arguments{runtime::Value::Integer(depth)};
    const auto exceeded = runtime::InvokeNativeFunction(execution, *descend, arguments);
    ASSERT_FALSE(exceeded);
    EXPECT_EQ(exceeded.error(), ExecutionStatus::StackOverflow);
    EXPECT_EQ(execution.current_frame(), &outer.back());
  }
  const auto leaf_exceeded = runtime::InvokeNativeFunction(execution, *through_leaf);
  ASSERT_FALSE(leaf_exceeded);
  EXPECT_EQ(leaf_exceeded.error(), ExecutionStatus::StackOverflow);
  EXPECT_EQ(execution.current_frame(), &outer.back());
  while (execution.current_frame() != nullptr) {
    execution.Exit();
  }
  const auto reused = runtime::InvokeNativeFunction(execution, *descend, zero);
  ASSERT_TRUE(reused);
  EXPECT_EQ(reused->AsInteger(), 42);
  EXPECT_EQ(execution.current_frame(), nullptr);
}

TEST_P(RecursiveCallTest, RawEntryValidatesExecutionAndPreservesNestedExceptions) {
  ASSERT_NO_FATAL_FAILURE(Load("export func identity(value: int): int { return value; }"));
  auto instance = BorrowInstance();
  const auto identity = instance.FindFunction("identity");
  if (!identity) {
    FAIL() << "missing identity entry";
  }
  const auto entry = instance.image().functions.values().subspan(*identity).front().entry;
  const std::array arguments{runtime::EncodeNativeValue(runtime::Value::Integer(42))};
  runtime::NativeValue result;
  EXPECT_EQ(entry(nullptr, arguments.data(), arguments.size(), &result),
            static_cast<std::uint8_t>(ExecutionStatus::InvalidIsolate));
  runtime::NativeExecution execution(instance);
  EXPECT_EQ(entry(&execution, arguments.data(), 0, &result),
            static_cast<std::uint8_t>(ExecutionStatus::InvalidEntry));
  EXPECT_EQ(entry(&execution, arguments.data(), arguments.size(), nullptr),
            static_cast<std::uint8_t>(ExecutionStatus::InvalidEntry));
  runtime::NativeTraceFrame outer;
  execution.MaterializeFrame(outer, *identity);
  ASSERT_EQ(execution.HandleFailure(ExecutionStatus::DivisionByZero),
            ExecutionStatus::DivisionByZero);
  const auto exception = execution.TakeException();
  ASSERT_FALSE(exception.IsNull());
  EXPECT_EQ(execution.Rethrow(exception), ExecutionStatus::DivisionByZero);
  ASSERT_EQ(entry(&execution, arguments.data(), arguments.size(), &result),
            static_cast<std::uint8_t>(ExecutionStatus::Returned));
  EXPECT_EQ(runtime::DecodeNativeValue(result).AsInteger(), 42);
  const auto preserved = execution.TakeException();
  EXPECT_EQ(preserved.AsObject(), exception.AsObject());
  EXPECT_EQ(execution.Rethrow(preserved), ExecutionStatus::DivisionByZero);
  EXPECT_EQ(execution.current_frame(), &outer);
  execution.Exit();
  ASSERT_EQ(entry(&execution, arguments.data(), arguments.size(), &result),
            static_cast<std::uint8_t>(ExecutionStatus::Returned));
  EXPECT_FALSE(execution.HasException());
  EXPECT_EQ(execution.current_frame(), nullptr);
}

TEST_P(RecursiveCallTest, ReferenceEarlyReturnsAndAllocatingCallersKeepRootsUntilCleanup) {
  ASSERT_NO_FATAL_FAILURE(Load(R"(
class Link { text: string; next: Link? = null; }
func retain(value: Link, depth: int): Link {
  if (depth == 0) { return value; }
  const next = retain(value, depth - 1);
  return Link(text: 'branch', next: next);
}
export func walk(depth: int): int {
  let node = retain(Link(text: 42.toString()), depth);
  while (true) {
    const next = node.next;
    if (next == null) { return node.text.length; }
    node = next;
  }
}
export func fail(depth: int): int { return 8 / (walk(depth) - 2); }
)"));
  auto instance = BorrowInstance();
  const auto walk = instance.FindFunction("walk");
  const auto fail = instance.FindFunction("fail");
  if (!walk || !fail) {
    FAIL() << "missing walk entry";
  }
  auto& heap = isolate().heap();
  heap.FinishCollection();
  heap.Collect();
  const auto baseline = heap.object_count();
  runtime::NativeExecution execution(instance);
  for (const std::int64_t depth : {0, 1, 32, 0}) {
    const std::array arguments{runtime::Value::Integer(depth)};
    const auto value = runtime::InvokeNativeFunction(execution, *walk, arguments);
    ASSERT_TRUE(value);
    EXPECT_EQ(value->AsInteger(), 2);
    EXPECT_EQ(execution.current_frame(), nullptr);
    EXPECT_EQ(execution.current_frame(), nullptr);
    // 先完成调用期间已标记旧根的增量周期，再检查下一周期确实回收已退出帧的对象。
    heap.FinishCollection();
    heap.Collect();
    EXPECT_EQ(heap.object_count(), baseline);
  }
  const std::array arguments{runtime::Value::Integer(32)};
  const auto failed = runtime::InvokeNativeFunction(execution, *fail, arguments);
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error(), ExecutionStatus::DivisionByZero);
  const auto reused = runtime::InvokeNativeFunction(execution, *walk, arguments);
  ASSERT_TRUE(reused);
  EXPECT_EQ(reused->AsInteger(), 2);
  heap.FinishCollection();
  heap.Collect();
  EXPECT_EQ(heap.object_count(), baseline);
}

INSTANTIATE_TEST_SUITE_P(
    NativeModes,
    RecursiveCallTest,
    testing::Values(std::tuple{NativeProgramMode::Closed, NativeCodeTier::Baseline},
                    std::tuple{NativeProgramMode::Reloadable, NativeCodeTier::Baseline},
                    std::tuple{NativeProgramMode::Reloadable, NativeCodeTier::Optimized}));

}  // namespace
}  // namespace vela::compiler::jit
