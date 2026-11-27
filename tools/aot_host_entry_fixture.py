"""Bounded host entries preserve complete invocation and observation contracts."""

SOURCE = """
external func reenter(): int;
class Counter {
  value: int = -1;
  add(value: int): int { this.value = value; return value; }
}
const counter = Counter();
let initial = 0;
let divisor = 0;
const lazy = 84 / divisor;
export func calculate(values: int[]): int {
  let sum = 0;
  for (let index = 0; index < values.length; index++) {
    counter.value = values[index];
    sum += counter.value;
  }
  return sum;
}
export func through(values: int[]): int {
  let sum = initial;
  for (let index = 0; index < values.length; index++) {
    sum += counter.add(values[index]);
  }
  return sum;
}
func inner(value: int): int { counter.value = value; return value / divisor; }
func middle(value: int): int { return inner(value); }
export func failing(values: int[]): int { return middle(values[0]); }
export func lazyRead(values: int[]): int { return lazy + values[0]; }
export func nested(values: int[]): int {
  let sum = 0;
  for (let i = 0; i < values.length; i++) {
    for (let j = 0; j < values.length; j++) { sum += values[j]; }
  }
  return sum;
}
export func allocate(values: int[]): int { return Counter(value: values[0]).value; }
export func mutate(values: int[]): int { values[0] = 42; return values[0]; }
export func recursive(values: int[]): int {
  return values[0] == 0 ? 0 : recursive(values);
}
export func enter(values: int[]): int { return reenter() + values[0]; }
export func reset(value: int, denominator: int): void {
  initial = value; divisor = denominator; counter.value = -1;
}
export func read(): int { return counter.value; }
func ordinarySum(values: int[]): int {
  let total = 0;
  for (let index = 0; index < values.length; index++) { total += values[index]; }
  return total;
}
export func ordinary(values: int[], initial: int): int {
  return ordinarySum(values) + initial;
}
export func ordinaryMutate(values: int[]): int {
  let total = 0;
  for (let index = 0; index < values.length; index++) {
    const value = values[index] + 1;
    values[index] = value;
    total += value;
  }
  return total;
}
"""

EXTRA_EXPORTS = ["through", "failing", "lazyRead", "nested", "allocate", "mutate",
                 "recursive", "enter", "reset", "read", "ordinary", "ordinaryMutate"]

HOST_TESTS = r'''
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
#include <vela/host.h>
#include "base/execution_limits.h"
#include "runtime/native/handle_access.h"
#include "runtime/native/host_value.h"
#include "runtime/native/native_call.h"
#include "runtime/native/native_control.h"
#include "runtime/native/native_debugger.h"
#include "runtime/native/native_execution.h"
#include "runtime/native/native_function.h"
#include "runtime/native/native_host_target.h"
#include "runtime/native/native_program_state.h"
#include "runtime/native/native_stack.h"

using namespace vela::runtime;
using vela::ExecutionStatus;
using IntegerArray = vela::Local<vela::Array<std::int64_t>>;
using ArrayEntry = vela::Entry<std::int64_t(vela::Array<std::int64_t>)>;
extern "C" const NativeImage* HOST_IMAGE() noexcept;

std::span<const NativeHostTarget> AvailableTargets() {
  static constexpr std::array targets{NativeHostTarget::Generic, NativeHostTarget::X86_64V4};
  return std::span(targets).first(CurrentNativeHostTarget() == NativeHostTarget::X86_64V4 ? 2 : 1);
}

void ExpectSameStackTrace(std::span<const FutureTraceFrame> actual,
                         std::span<const FutureTraceFrame> expected) {
  ASSERT_EQ(actual.size(), expected.size());
  for (std::size_t index = 0; index < actual.size(); ++index) {
    EXPECT_EQ(actual[index].function, expected[index].function);
    EXPECT_EQ(actual[index].line, expected[index].line);
  }
}

std::int64_t ReenterHostEntry(NativeContext& context) noexcept {
  auto* parent = context.isolate().native_control().state().host_execution;
  if (parent == nullptr) { return -100; }
  const auto* frame = parent->current_frame();
  const auto entry = ArrayEntry::Bind(parent->instance(), "calculate");
  const auto array = vela::CreateArray<std::int64_t>(context.handles());
  if (!entry || !array || !array->Push(context.handles(), std::int64_t{7})) { return -101; }
  const auto result = entry->Call(context.handles(), *array);
  return result && *result == 7 && parent->current_frame() == frame ? 7 : -102;
}

class NativeHostEntryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(isolate.RegisterNative(NativeFunction::Bind<ReenterHostEntry>("reenter")));
    auto created = NativeInstance::Instantiate(isolate, *HOST_IMAGE());
    ASSERT_TRUE(created) << created.error().message;
    instance = std::move(*created);
  }
  const NativeFunctionDescription& Function(std::string_view name) const {
    return instance->image().function(instance->FindFunction(name).value());
  }
  struct Kernel final {
    const NativeFunctionDescription& function;
    NativeHostEntry entry;
    const ArrayLayout* layout;
  };
  std::optional<Kernel> SelectKernel(std::string_view name, NativeHostTarget target) const {
    const auto& function = Function(name);
    if (function.host_entry_selector == nullptr) { return std::nullopt; }
    const auto entry = function.host_entry_selector(target);
    const auto* layout = instance->layouts().types[function.parameters.values().front().type].array;
    if (entry == nullptr || layout == nullptr) { return std::nullopt; }
    return Kernel{.function = function, .entry = entry, .layout = layout};
  }
  vela::Result<IntegerArray> Array(std::span<const std::int64_t> values) {
    auto result = vela::CreateArray<std::int64_t>(scope);
    if (!result) { return result; }
    for (const auto value : values) {
      if (auto added = result->Push(scope, value); !added) {
        return std::unexpected(std::move(added.error()));
      }
    }
    return result;
  }
  std::expected<Value, ExecutionStatus> Invoke(std::string_view name, Value argument) {
    const auto bound = NativeVersionEntry::Bind(*instance, instance->FindFunction(name).value());
    if (!bound) { return std::unexpected(bound.error()); }
    const std::array values{argument};
    return bound->Invoke(values);
  }
  void Reset(std::int64_t initial = 0, std::int64_t divisor = 0) {
    const std::array arguments{Value::Integer(initial), Value::Integer(divisor)};
    ASSERT_TRUE(ExecuteNativeFunction(*instance, instance->FindFunction("reset").value(),
                                      arguments).Succeeded());
  }
  std::int64_t Read() {
    return ExecuteNativeFunction(*instance, instance->FindFunction("read").value()).value.AsInteger();
  }
  HeapObject* StateForTarget(NativeHostTarget target) {
    const auto& original = instance->program();
    const auto function_count = original.image->functions.size;
    auto state = std::make_unique<NativeProgramState>(target);
    state->isolate = &isolate;
    state->image = original.image;
    state->layouts = original.layouts;
    state->call_signatures.resize(function_count);
    state->code_identity = original.code_identity;
    state->import_owners = original.import_owners;
    auto& heap = isolate.heap();
    state->imports = static_cast<const NativeFunction::AotEntry**>(
        heap.AllocateNative(function_count * sizeof(const NativeFunction::AotEntry*)));
    if (state->imports == nullptr) { return nullptr; }
    std::ranges::copy_n(original.imports, function_count, state->imports);
    const auto global_count = original.image->global_initializers.size;
    state->global_states = static_cast<NativeProgramState::GlobalState*>(
        heap.AllocateNative(global_count * sizeof(NativeProgramState::GlobalState)));
    if (state->global_states == nullptr) { return nullptr; }
    std::ranges::copy_n(original.global_states, global_count, state->global_states);
    // 原实例在整个测试期间保根；只共享 managed 图，独占的原生表必须分别释放。
    state->values = original.values;
    return isolate.heap().AllocateNativeProgram(std::move(state));
  }
  void ExpectOrdinaryStop(NativeHostTarget target) {
    Reset();
    auto* state = StateForTarget(target);
    ASSERT_NE(state, nullptr);
    auto selected = NativeInstance::Borrow(*state);
    const std::array<std::int64_t, 3> terms{1, 2, 3};
    auto array = Array(terms);
    ASSERT_TRUE(array);
    const std::array arguments{vela::internal::HandleAccess::Read(*array)};
    const auto bound = NativeVersionEntry::Bind(selected, selected.FindFunction("calculate").value());
    ASSERT_TRUE(bound);
    isolate.native_control().RequestStop();
    const auto result = bound->Invoke(arguments);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), ExecutionStatus::ExecutionStopped);
    EXPECT_EQ(Read(), -1);
    for (std::size_t index = 0; index < terms.size(); ++index) {
      const auto value = isolate.heap().GetElement(*arguments.front().AsObject(), index);
      ASSERT_TRUE(value);
      EXPECT_EQ(value->AsInteger(), terms[index]);
    }
  }
  Isolate isolate{1};
  vela::HandleScope scope{isolate};
  std::unique_ptr<NativeInstance> instance;
};

TEST_F(NativeHostEntryTest, OrdinaryTargetsKeepVectorTailsMultipleArgumentsAndGcRoots) {
  NativeValue output;
  const auto& ordinary = Function("ordinary");
  EXPECT_EQ(static_cast<ExecutionStatus>(
                ordinary.entry(nullptr, nullptr, 0, &output)),
            ExecutionStatus::InvalidEntry);
  const std::array<NativeValue, 2> arguments{};
  EXPECT_EQ(static_cast<ExecutionStatus>(
                ordinary.entry(nullptr, arguments.data(), arguments.size(), &output)),
            ExecutionStatus::InvalidIsolate);
  EXPECT_EQ(static_cast<ExecutionStatus>(
                ordinary.entry(nullptr, arguments.data(), arguments.size(), nullptr)),
            ExecutionStatus::InvalidEntry);
  for (const auto target : AvailableTargets()) {
    auto* state = StateForTarget(target);
    ASSERT_NE(state, nullptr);
    auto selected = NativeInstance::Borrow(*state);
    const auto function = selected.FindFunction("ordinary");
    ASSERT_TRUE(function);
    for (const std::size_t size : {0U, 1U, 7U, 8U, 9U, 31U, 32U, 33U, 127U, 128U, 129U,
                                   511U, 512U, 513U, 1024U, 1025U, 4097U}) {
      const std::vector<std::int64_t> terms(size, 1);
      auto array = Array(terms);
      ASSERT_TRUE(array);
      const auto input = vela::internal::HandleAccess::Read(*array);
      const std::array arguments{input, Value::Integer(7)};
      isolate.heap().StartCollection();
      const auto result = ExecuteNativeFunction(selected, *function,
                                                 arguments);
      ASSERT_TRUE(result.Succeeded()) << size;
      EXPECT_EQ(result.value.AsInteger(), static_cast<std::int64_t>(size) + 7);
      isolate.heap().FinishCollection();
      const std::array explicit_arguments{input, Value::Integer(11)};
      const auto explicit_result = ExecuteNativeFunction(
          selected, *function, explicit_arguments);
      ASSERT_TRUE(explicit_result.Succeeded());
      EXPECT_EQ(explicit_result.value.AsInteger(), static_cast<std::int64_t>(size) + 11);
    }
    const std::array invalid{Value::Integer(1), Value::Integer(7)};
    const auto rejected = ExecuteNativeFunction(selected, *function,
                                                   invalid);
    EXPECT_EQ(rejected.status, ExecutionStatus::InvalidCast);
  }
}

TEST_F(NativeHostEntryTest, OrdinaryTargetsPreserveOverflowTracesAndCompletedWrites) {
  for (const auto target : AvailableTargets()) {
    auto* state = StateForTarget(target);
    ASSERT_NE(state, nullptr);
    auto selected = NativeInstance::Borrow(*state);
    const std::array terms{std::int64_t{1}, std::numeric_limits<std::int64_t>::max(), std::int64_t{2}};
    auto array = Array(terms);
    ASSERT_TRUE(array);
    const auto input = vela::internal::HandleAccess::Read(*array);
    const std::array arguments{input};
    const auto result = ExecuteNativeFunction(selected,
        selected.FindFunction("ordinaryMutate").value(), arguments);
    EXPECT_EQ(result.status, ExecutionStatus::IntegerOverflow);
    ASSERT_FALSE(result.stack_trace.empty());
    EXPECT_EQ(result.stack_trace.front().function, "ordinaryMutate");
    auto& heap = isolate.heap();
    EXPECT_EQ(heap.GetElement(*input.AsObject(), 0)->AsInteger(), 2);
    EXPECT_EQ(heap.GetElement(*input.AsObject(), 1)->AsInteger(), terms[1]);
    EXPECT_EQ(heap.GetElement(*input.AsObject(), 2)->AsInteger(), terms[2]);
    const std::array crossing{std::numeric_limits<std::int64_t>::max(), std::int64_t{1},
                               std::int64_t{-1}};
    auto cancelled = Array(crossing);
    ASSERT_TRUE(cancelled);
    const std::array crossing_arguments{vela::internal::HandleAccess::Read(*cancelled),
                                          Value::Integer(7)};
    const auto overflow = ExecuteNativeFunction(selected, selected.FindFunction("ordinary").value(),
                                                    crossing_arguments);
    EXPECT_EQ(overflow.status, ExecutionStatus::IntegerOverflow);
    EXPECT_TRUE(std::ranges::any_of(overflow.stack_trace, [](const auto& frame) {
      return frame.function == "ordinarySum";
    }));
  }
}

TEST_F(NativeHostEntryTest, OrdinaryTargetsProcessScheduledObservationsOnce) {
  for (const auto target : AvailableTargets()) {
    Reset();
    auto* state = StateForTarget(target);
    ASSERT_NE(state, nullptr);
    auto selected = NativeInstance::Borrow(*state);
    const std::array<std::int64_t, 2> terms{1, 2};
    auto array = Array(terms);
    ASSERT_TRUE(array);
    const auto bound = NativeVersionEntry::Bind(selected, selected.FindFunction("calculate").value());
    ASSERT_TRUE(bound);
    std::size_t observed = 0;
    ASSERT_TRUE(isolate.native_control().ScheduleSafepoint([&] {
      ++observed;
      EXPECT_EQ(Read(), -1);
      isolate.heap().Collect();
    }));
    const std::array arguments{vela::internal::HandleAccess::Read(*array)};
    const auto result = bound->Invoke(arguments);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->AsInteger(), 3);
    EXPECT_EQ(observed, 1U);
    EXPECT_EQ(Read(), 2);
    EXPECT_EQ(isolate.heap().GetElement(*arguments.front().AsObject(), 0)->AsInteger(), 1);
    EXPECT_EQ(isolate.heap().GetElement(*arguments.front().AsObject(), 1)->AsInteger(), 2);
  }
}

TEST_F(NativeHostEntryTest, OrdinaryPortableEntryStopsBeforeAnyWrite) {
  ExpectOrdinaryStop(NativeHostTarget::Generic);
}

TEST_F(NativeHostEntryTest, OrdinaryWideEntryStopsBeforeAnyWrite) {
  if (CurrentNativeHostTarget() != NativeHostTarget::X86_64V4) { GTEST_SKIP(); }
  ExpectOrdinaryStop(NativeHostTarget::X86_64V4);
}

TEST_F(NativeHostEntryTest, ColdGlobalsAndArrayBudgetsKeepCompleteResults) {
  const auto& function = Function("calculate");
  ASSERT_NE(function.host_entry_selector, nullptr);
  const auto host = function.host_entry_selector(CurrentNativeHostTarget());
  const auto limit = CurrentNativeStackLimit();
  ASSERT_TRUE(limit);
  const std::array<std::int64_t, 3> values{1, 2, 3};
  auto array = Array(values);
  ASSERT_TRUE(array);
  const auto input = vela::internal::HandleAccess::Read(*array);
  NativeHostContext cold{.program = &instance->program(),
                         .control = &isolate.native_control().state(), .depth = 0,
                         .stack_limit = *limit};
  NativeValue output;
  EXPECT_EQ(host(&cold, input.AsObject(), &output), kNativeHostMiss);
  auto first = Invoke("calculate", input);
  ASSERT_TRUE(first);
  EXPECT_EQ(first->AsInteger(), 6);
  auto warm = cold;
  EXPECT_EQ(host(&warm, input.AsObject(), &output),
            ToNativeStatus(ExecutionStatus::Returned));
  EXPECT_EQ(DecodeNativeValue(output).AsInteger(), 6);
  for (const std::size_t size : {0U, 1U, 128U, 512U, 1024U, 1025U, 4097U}) {
    std::vector<std::int64_t> terms(size, 1);
    auto sample = Array(terms);
    ASSERT_TRUE(sample);
    auto result = Invoke("calculate", vela::internal::HandleAccess::Read(*sample));
    ASSERT_TRUE(result) << size;
    EXPECT_EQ(result->AsInteger(), static_cast<std::int64_t>(size));
  }
}

TEST_F(NativeHostEntryTest, UnsupportedGraphsDoNotPublishHostEntries) {
  for (const auto name : {"nested", "allocate", "mutate", "recursive", "enter"}) {
    EXPECT_EQ(Function(name).host_entry_selector, nullptr) << name;
  }
}

TEST_F(NativeHostEntryTest, SelectedKernelsKeepVectorTailsAndBudgetMisses) {
  for (const auto target : AvailableTargets()) {
    for (const auto name : {"calculate", "through"}) {
      const auto kernel = SelectKernel(name, target);
      ASSERT_TRUE(kernel);
      const auto& [function, host, layout] = *kernel;
      for (const std::size_t size : {0U, 1U, 2U, 3U, 7U, 8U, 9U, 15U, 16U, 17U, 31U, 32U,
                                      33U, 63U, 64U, 65U, 127U, 128U, 129U, 511U, 512U, 513U,
                                      1023U, 1024U, 1025U}) {
        SCOPED_TRACE(::testing::Message() << name << '/' << static_cast<int>(target) << '/' << size);
        std::vector<std::int64_t> terms(size);
        std::int64_t expected = 0;
        for (std::size_t index = 0; index < size; ++index) {
          terms[index] = static_cast<std::int64_t>(index & 31U);
          expected += terms[index];
        }
        auto array = Array(terms);
        ASSERT_TRUE(array);
        const auto input = vela::internal::HandleAccess::Read(*array);
        Reset();
        isolate.heap().FinishCollection();
        const auto limit = CurrentNativeStackLimit();
        ASSERT_TRUE(limit);
        NativeHostContext context{.program = &instance->program(),
                                  .control = &isolate.native_control().state(), .depth = 0,
                                  .stack_limit = *limit};
        NativeValue output;
        const auto status = host(&context, input.AsObject(), &output);
        if (status == kNativeHostMiss) {
          EXPECT_EQ(Read(), -1);
        } else {
          ASSERT_EQ(status, ToNativeStatus(ExecutionStatus::Returned));
          EXPECT_EQ(DecodeNativeValue(output).AsInteger(), expected);
        }
        Reset();
        isolate.heap().FinishCollection();
        const std::array arguments{input};
        const NativeArrayEntry entry(*instance, function, *layout, host);
        const auto result = entry.Invoke(arguments);
        ASSERT_TRUE(result);
        EXPECT_EQ(result->AsInteger(), expected);
        EXPECT_EQ(Read(), size == 0 ? -1 : terms.back());
      }
    }
  }
}

TEST_F(NativeHostEntryTest, KernelFailuresCaptureOwnedErrorsBeforeExecutionRelease) {
  struct Capture final {
    std::size_t calls = 0;
    vela::Result<void> result;
  };
  for (const auto target : AvailableTargets()) {
    const auto kernel = SelectKernel("calculate", target);
    ASSERT_TRUE(kernel);
    const auto& [function, host, layout] = *kernel;
    for (const auto sign : {std::int64_t{1}, std::int64_t{-1}}) {
      for (const std::size_t prefix : {0U, 1U, 7U, 8U, 15U, 16U, 31U, 32U, 63U, 64U, 127U}) {
        SCOPED_TRACE(::testing::Message() << static_cast<int>(target) << '/' << sign << '/' << prefix);
        std::vector<std::int64_t> terms(prefix + 3, 0);
        terms[prefix] = sign == 1 ? std::numeric_limits<std::int64_t>::max()
                                  : std::numeric_limits<std::int64_t>::min();
        terms[prefix + 1] = sign;
        terms.back() = 42;
        auto array = Array(terms);
        ASSERT_TRUE(array);
        const std::array arguments{vela::internal::HandleAccess::Read(*array)};
        Reset();
        const auto ordinary = ExecuteNativeFunction(
            *instance, instance->FindFunction("calculate").value(), arguments);
        ASSERT_EQ(ordinary.status, ExecutionStatus::IntegerOverflow);
        Reset();
        isolate.heap().FinishCollection();
        Capture capture;
        const NativeFailureObserver failure{
            .context = &capture,
            .capture = [](void* context, NativeExecution& execution, ExecutionStatus status) {
              auto& captured = *static_cast<Capture*>(context);
              ++captured.calls;
              EXPECT_EQ(execution.current_frame(), nullptr);
              EXPECT_EQ(execution.frame_count(), 0U);
              execution.isolate().heap().Collect();
              captured.result = std::unexpected(vela::internal::CaptureHostError(execution, status));
            },
        };
        const NativeArrayEntry entry(*instance, function, *layout, host);
        const auto result = entry.Invoke(arguments, nullptr, &failure);
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error(), ExecutionStatus::IntegerOverflow);
        ASSERT_EQ(capture.calls, 1U);
        ASSERT_FALSE(capture.result);
        const auto& error = capture.result.error();
        EXPECT_EQ(error.status, ordinary.status);
        ASSERT_NO_FATAL_FAILURE(ExpectSameStackTrace(error.stack_trace, ordinary.stack_trace));
        EXPECT_EQ(Read(), sign);
        isolate.heap().Collect();
        EXPECT_FALSE(error.payload.IsEmpty());
      }
    }
  }
}

TEST_F(NativeHostEntryTest, PublicErrorsMatchOrdinaryTracesAndRetainWrittenPrefixes) {
  const std::array<std::int64_t, 3> values{1, 2, 3};
  auto array = Array(values);
  ASSERT_TRUE(array);
  const auto input = vela::internal::HandleAccess::Read(*array);
  Reset();
  const std::array arguments{input};
  auto ordinary = ExecuteNativeFunction(*instance, instance->FindFunction("failing").value(),
                                        arguments);
  ASSERT_EQ(ordinary.status, ExecutionStatus::DivisionByZero);
  auto entry = ArrayEntry::Bind(*instance, "failing");
  ASSERT_TRUE(entry);
  auto failed = entry->Call(scope, *array);
  ASSERT_FALSE(failed);
  const auto& error = failed.error();
  EXPECT_EQ(error.status, ordinary.status);
  ASSERT_NO_FATAL_FAILURE(ExpectSameStackTrace(error.stack_trace, ordinary.stack_trace));
  EXPECT_EQ(Read(), 1);
  Reset(std::numeric_limits<std::int64_t>::max());
  auto through = ArrayEntry::Bind(*instance, "through");
  ASSERT_TRUE(through);
  auto overflow = through->Call(scope, *array);
  ASSERT_FALSE(overflow);
  EXPECT_EQ(overflow.error().status, ExecutionStatus::IntegerOverflow);
  EXPECT_EQ(Read(), 1);
  isolate.heap().Collect();
  EXPECT_FALSE(error.payload.IsEmpty());
}

TEST_F(NativeHostEntryTest, LazyGlobalFailureCanBeRepairedWithoutSkippingInitialization) {
  const std::array<std::int64_t, 1> values{1};
  auto array = Array(values);
  ASSERT_TRUE(array);
  const auto input = vela::internal::HandleAccess::Read(*array);
  auto failed = Invoke("lazyRead", input);
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error(), ExecutionStatus::DivisionByZero);
  Reset(0, 2);
  auto retried = Invoke("lazyRead", input);
  ASSERT_TRUE(retried);
  EXPECT_EQ(retried->AsInteger(), 43);
  auto warmed = Invoke("lazyRead", input);
  ASSERT_TRUE(warmed);
  EXPECT_EQ(warmed->AsInteger(), 43);
}

TEST_F(NativeHostEntryTest, CanonicalTypesGcAndPublicReentryKeepTheirBoundaries) {
  const std::array<std::int64_t, 1> values{1};
  auto array = Array(values);
  ASSERT_TRUE(array);
  const auto input = vela::internal::HandleAccess::Read(*array);
  Reset();
  for (const auto invalid : {Value::Null(), Value::Integer(1)}) {
    auto result = Invoke("calculate", invalid);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), ExecutionStatus::InvalidCast);
  }
  auto* head = isolate.heap().native_stack_roots_head();
  auto* previous = *head;
  isolate.heap().StartCollection();
  ASSERT_TRUE(isolate.heap().IsCollecting());
  auto result = Invoke("calculate", input);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->AsInteger(), 1);
  EXPECT_EQ(*head, previous);
  isolate.heap().FinishCollection();
  result = Invoke("enter", input);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->AsInteger(), 8);
  EXPECT_EQ(isolate.native_control().state().host_execution, nullptr);
}

TEST_F(NativeHostEntryTest, LogicalAndPhysicalStackLimitsFailBeforeCalleeEffects) {
  const std::array<std::int64_t, 1> values{7};
  auto array = Array(values);
  ASSERT_TRUE(array);
  const auto input = vela::internal::HandleAccess::Read(*array);
  Reset();
  const auto limit = CurrentNativeStackLimit();
  ASSERT_TRUE(limit);
  const auto kernel = SelectKernel("through", CurrentNativeHostTarget());
  ASSERT_TRUE(kernel);
  const auto host = kernel->entry;
  for (const auto depth : {std::uint64_t{vela::kMaxCallFrames - 1},
                           std::uint64_t{vela::kMaxCallFrames}}) {
    NativeHostContext context{.program = &instance->program(),
                              .control = &isolate.native_control().state(), .depth = depth,
                              .stack_limit = *limit};
    NativeValue output;
    EXPECT_EQ(host(&context, input.AsObject(), &output),
              ToNativeStatus(ExecutionStatus::StackOverflow));
    EXPECT_EQ(Read(), -1);
  }
  NativeHostContext context{.program = &instance->program(),
                            .control = &isolate.native_control().state(), .depth = 0,
                            .stack_limit = std::numeric_limits<std::uintptr_t>::max()};
  NativeValue output;
  EXPECT_EQ(host(&context, input.AsObject(), &output),
            ToNativeStatus(ExecutionStatus::StackOverflow));
  EXPECT_EQ(Read(), -1);
}

class HostEntryDebugger final : public NativeDebugger {
 public:
  void AtSafepoint(const NativeDebugContext& context) noexcept override {
    names.emplace_back(context.top_frame().name());
  }
  std::vector<std::string> names;
};

TEST_F(NativeHostEntryTest, PendingObservationAndStopsUseOrdinarySafepoints) {
  const std::array<std::int64_t, 1> values{7};
  auto array = Array(values);
  ASSERT_TRUE(array);
  const auto input = vela::internal::HandleAccess::Read(*array);
  Reset();
  HostEntryDebugger debugger;
  isolate.native_control().SetDebugger(&debugger);
  auto result = Invoke("calculate", input);
  isolate.native_control().SetDebugger(nullptr);
  ASSERT_TRUE(result);
  EXPECT_FALSE(debugger.names.empty());
  isolate.native_control().RequestStop();
  result = Invoke("calculate", input);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), ExecutionStatus::ExecutionStopped);
}

std::uint8_t ObserveHostReturn(NativeHostContext* context, HeapObject* array,
                               NativeValue* output) noexcept {
  const auto functions = context->program->image->functions.values();
  const auto found = std::ranges::find(functions, std::string_view("calculate"),
                                      [](const auto& value) { return value.name.text(); });
  const auto host = found->host_entry_selector(CurrentNativeHostTarget());
  const auto status = host(context, array, output);
  if (status != ToNativeStatus(ExecutionStatus::Returned)) { return status; }
  auto& isolate = *context->program->isolate;
  const auto scheduled = isolate.native_control().ScheduleSafepoint([&isolate, array] {
    isolate.heap().Collect();
    EXPECT_TRUE(isolate.heap().SetElement(*array, 0, Value::Integer(900)));
  });
  EXPECT_TRUE(scheduled);
  context->trace_count = 1;
  return kNativeHostObserve;
}

TEST_F(NativeHostEntryTest, CompletedEffectsAreNotReplayedAtReturnObservation) {
  const std::array<std::int64_t, 3> values{1, 2, 3};
  auto array = Array(values);
  ASSERT_TRUE(array);
  const auto input = vela::internal::HandleAccess::Read(*array);
  Reset();
  const auto kernel = SelectKernel("calculate", CurrentNativeHostTarget());
  ASSERT_TRUE(kernel);
  const auto& function = kernel->function;
  const auto* layout = kernel->layout;
  isolate.heap().FinishCollection();
  const std::array arguments{input};
  NativeExecution parent(*instance);
  NativeTraceFrame frame;
  parent.MaterializeFrame(frame, instance->FindFunction("read").value());
  auto* head = isolate.heap().native_stack_roots_head();
  const auto* previous = *head;
  const NativeArrayEntry entry(*instance, function, *layout, ObserveHostReturn);
  const auto result = entry.Invoke(arguments, &parent);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->AsInteger(), 6);
  EXPECT_EQ(isolate.heap().GetElement(*input.AsObject(), 0)->AsInteger(), 900);
  EXPECT_EQ(Read(), 3);
  EXPECT_EQ(*head, previous);
  EXPECT_EQ(parent.current_frame(), &frame);
  EXPECT_EQ(parent.frame_count(), 1U);
  parent.Exit();
}
'''
