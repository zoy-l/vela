#include <array>
#include <cstdint>
#include <expected>
#include <string_view>
#include <tuple>
#include <vector>

#include <gtest/gtest.h>

#include "base/execution_limits.h"
#include "base/execution_status.h"
#include "base/native_program_mode.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/jit/compiler.h"
#include "compiler/jit/source_compiler.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_call.h"
#include "runtime/native/native_context.h"
#include "runtime/native/native_function.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_instance.h"
#include "runtime/value/value.h"
#include "vela/error.h"

namespace vela::compiler::jit {
namespace {

class ScalarHostCallTest : public testing::TestWithParam<std::tuple<NativeCodeTier, bool>> {
 protected:
  void TearDown() override { execution_ = nullptr; }

  static void Observe(std::string_view name) {
    const auto* frame = execution_->current_frame();
    ASSERT_NE(frame, nullptr);
    ASSERT_NE(frame->previous, nullptr);
    ASSERT_EQ(frame->image, &execution_->instance().image());
    EXPECT_EQ(frame->image->functions.values()[frame->position.function].name.text(), name);
    EXPECT_EQ(frame->position.instruction, 0U);
    EXPECT_EQ(frame->checkpoint, nullptr);
    EXPECT_EQ(frame->debug_values, nullptr);
    EXPECT_EQ(frame->debug_count, 0U);
  }

  static std::int64_t Step(std::int64_t value) noexcept {
    Observe("step");
    const auto* frame = execution_->current_frame();
    const auto nested = execution_->instance().FindFunction("nested");
    EXPECT_TRUE(nested);
    if (nested) {
      const auto result = runtime::InvokeNativeFunction(*execution_, *nested);
      EXPECT_TRUE(result);
      EXPECT_EQ(execution_->current_frame(), frame);
    }
    execution_->isolate().heap().Collect();
    // 当前指令是运行时允许修改的字段；后续调用必须重新从指令零开始。
    constexpr std::uint32_t kHostModifiedInstruction = 17;
    *execution_->current_instruction() = kHostModifiedInstruction;
    return value + 1;
  }

  static std::int64_t StepWithContext(runtime::NativeContext& context,
                                      std::int64_t value) noexcept {
    EXPECT_EQ(&context, &execution_->context());
    return Step(value);
  }

  static bool Flip(bool value) noexcept {
    Observe("flip");
    return !value;
  }

  static double Half(double value) noexcept {
    Observe("half");
    return value / 2;
  }

  static void Touch() noexcept {
    Observe("touch");
    const auto trace = execution_->CaptureTrace();
    ASSERT_GE(trace.size(), 2U);
    EXPECT_EQ(trace.front().function, "touch");
    if (trace.at(1).function == "nested") {
      ASSERT_EQ(trace.size(), 4U);
      EXPECT_EQ(trace.at(2).function, "step");
      EXPECT_EQ(trace.at(3).function, "run");
    }
  }

  static std::expected<std::int64_t, vela::Error> Reject() noexcept {
    Observe("reject");
    return std::unexpected(vela::Error{.message = "rejected", .stack_trace = {}});
  }

  static thread_local runtime::NativeExecution* execution_;
};

thread_local runtime::NativeExecution* ScalarHostCallTest::execution_ = nullptr;

TEST_P(ScalarHostCallTest, PreservesFramesReentryRootsCheckedFailuresAndDepthLimits) {
  auto compiler = Compiler::Create();
  ASSERT_TRUE(compiler.has_value());
  const auto [tier, with_context] = GetParam();
  const auto compiled = CompileSource(**compiler, R"(
external func step(value: int): int;
external func flip(value: bool): bool;
external func half(value: double): double;
external func touch(): void;
external func reject(): int;
export func nested(): int { touch(); return 1; }
export func run(count: int): int {
  const saved = ['retained', 'text'].join('');
  let value = 0;
  for (let index = 0; index < count; index++) {
    value = step(value);
    if (!flip(false) || half(4.0) != 2.0) { return -1; }
  }
  touch();
  return value + saved.length;
}
export func failing(): int { return reject(); }
export func caught(): int {
  try { return reject(); }
  catch (error: Error) { touch(); return error.message.length; }
}
export func boundary(): void { touch(); }
)",
                                      {.diagnostics = {}, .compilation = {.tier = tier}});
  ASSERT_TRUE(compiled.Succeeded()) << frontend::DiagnosticsText(compiled.diagnostics);
  runtime::Isolate isolate(1);
  ASSERT_TRUE(isolate.RegisterNative(with_context
                                         ? runtime::NativeFunction::Bind<StepWithContext>("step")
                                         : runtime::NativeFunction::Bind<Step>("step")));
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<Flip>("flip")));
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<Half>("half")));
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<Touch>("touch")));
  ASSERT_TRUE(isolate.RegisterNative(runtime::NativeFunction::Bind<Reject>("reject")));
  static_cast<void>(isolate.FreezeBindings());
  auto instance = runtime::NativeInstance::Instantiate(isolate, compiled.code.image);
  ASSERT_TRUE(instance.has_value());
  const auto run = (*instance)->FindFunction("run");
  const auto failing = (*instance)->FindFunction("failing");
  const auto caught = (*instance)->FindFunction("caught");
  const auto boundary = (*instance)->FindFunction("boundary");
  if (!run || !failing || !caught || !boundary) {
    FAIL() << "expected exported functions run, failing, caught and boundary";
  }
  runtime::NativeExecution execution(**instance);
  execution_ = &execution;
  for (const std::int64_t count : {0, 1, 4, 1}) {
    const std::array arguments{runtime::Value::Integer(count)};
    const auto result = runtime::InvokeNativeFunction(execution, *run, arguments);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->AsInteger(), count + 12);
    EXPECT_EQ(execution.current_frame(), nullptr);
  }
  const auto rejected = runtime::InvokeNativeFunction(execution, *failing);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error(), ExecutionStatus::NativeFailure);
  const auto diagnostic = execution.ExceptionDiagnostic();
  ASSERT_EQ(diagnostic.stack_trace.size(), 2U);
  EXPECT_EQ(diagnostic.stack_trace.front().function, "reject");
  EXPECT_EQ(diagnostic.stack_trace.back().function, "failing");
  EXPECT_EQ(execution.current_frame(), nullptr);
  const auto handled = runtime::InvokeNativeFunction(execution, *caught);
  ASSERT_TRUE(handled.has_value());
  EXPECT_EQ(handled->AsInteger(), 8);
  EXPECT_EQ(execution.current_frame(), nullptr);

  std::vector<runtime::NativeTraceFrame> outer(kMaxCallFrames - 2);
  for (auto& frame : outer) {
    execution.MaterializeFrame(frame, *boundary);
  }
  const auto allowed = runtime::InvokeNativeFunction(execution, *boundary);
  ASSERT_TRUE(allowed.has_value());
  runtime::NativeTraceFrame last;
  execution.MaterializeFrame(last, *boundary);
  const auto exceeded = runtime::InvokeNativeFunction(execution, *boundary);
  ASSERT_FALSE(exceeded);
  EXPECT_EQ(exceeded.error(), ExecutionStatus::StackOverflow);
  EXPECT_EQ(execution.current_frame(), &last);
  while (execution.current_frame() != nullptr) {
    execution.Exit();
  }
  EXPECT_TRUE(runtime::InvokeNativeFunction(execution, *boundary));
  EXPECT_EQ(execution.current_frame(), nullptr);
}

INSTANTIATE_TEST_SUITE_P(ClosedModes,
                         ScalarHostCallTest,
                         testing::Combine(testing::Values(NativeCodeTier::Baseline,
                                                          NativeCodeTier::Optimized),
                                          testing::Bool()));

}  // namespace
}  // namespace vela::compiler::jit
