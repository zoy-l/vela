#include "vm/vm.h"

#include <absl/cleanup/cleanup.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include "base/builtin.h"
#include "base/byte_access.h"
#include "base/callable.h"
#include "base/execution_limits.h"
#include "base/regexp.h"
#include "runtime/byte_operations.h"
#include "runtime/exception.h"
#include "vm/debugger.h"
#include "vm/execution.h"
#include "vm/execution_status.h"
#include "vm/program.h"
#include "vm/register_file.h"
#include "vm/suspended_frame.h"

#include "runtime/builtins.h"
#include "runtime/callable.h"
#include "runtime/dynamic_value.h"
#include "runtime/error.h"
#include "runtime/future.h"
#include "runtime/future_operations.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/native_function.h"
#include "runtime/numeric_operations_internal.h"
#include "runtime/object.h"
#include "runtime/register_storage.h"
#include "runtime/string.h"
#include "runtime/value.h"
#include "runtime/value_io.h"
#include "runtime/value_operations.h"
#include "vm/bytecode.h"
#include "vm/frame.h"

namespace vela::vm {
namespace internal {
namespace {

inline constexpr std::size_t kInlineInterpolationParts = 8;

struct BinaryOperands final {
  runtime::RegisterReference left;
  runtime::RegisterReference right;
};

template <class T>
void ConsumeFutureCompletion(std::expected<void, T> completion) {
  if (!completion) {
    std::unreachable();
  }
}

[[clang::always_inline]] StepResult IntegerOperation(OpCode opcode,
                                                     runtime::RegisterReference destination,
                                                     BinaryOperands operands) noexcept {
  const auto result = runtime::internal::IntegerOperation(
      opcode, {.left = operands.left.Read(), .right = operands.right.Read()});
  if (!result) {
    return std::unexpected(result.error());
  }
  destination = *result;
  return {};
}

[[clang::always_inline]] StepResult DoubleOperation(OpCode opcode,
                                                    runtime::RegisterReference destination,
                                                    BinaryOperands operands) noexcept {
  const auto result = runtime::internal::DoubleOperation(
      opcode, {.left = operands.left.Read(), .right = operands.right.Read()});
  if (!result) {
    return std::unexpected(result.error());
  }
  destination = *result;
  return {};
}

[[clang::always_inline]] StepResult BooleanOperation(OpCode opcode,
                                                     runtime::RegisterReference destination,
                                                     BinaryOperands operands) noexcept {
  const auto& [left_value, right_value] = operands;
  if (!left_value.IsBoolean() || !right_value.IsBoolean()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const bool left = left_value.AsBoolean();
  const bool right = right_value.AsBoolean();
  bool result = false;
  switch (opcode) {
    case OpCode::EqualBool:
      result = left == right;
      break;
    case OpCode::NotEqualBool:
      result = left != right;
      break;
    case OpCode::AndBool:
      result = left && right;
      break;
    case OpCode::OrBool:
      result = left || right;
      break;
    default:
      std::unreachable();
  }
  destination.SetBoolean(result);
  return {};
}

std::expected<runtime::Value, ExecutionStatus> ExecuteValueBuiltin(Builtin builtin,
                                                                   runtime::RegisterView arguments,
                                                                   runtime::Heap& heap) {
  constexpr std::size_t kMaxBuiltinArguments = 4;
  std::array<runtime::Value, kMaxBuiltinArguments> materialized;
  const auto input = std::span(materialized).first(arguments.size());
  arguments.CopyTo(input);
  return runtime::ExecuteBuiltin(builtin, input, heap);
}

}  // namespace

template <bool Debugging>
ExecutionResult Execution::RunImpl(std::uint32_t entry_function, Debugger* debugger) noexcept {
  auto& entry = frames_.front();
  entry.function = &functions_.subspan(entry_function).front();
  auto entry_values = registers_.front().Acquire(entry.function->register_count);
  if (entry_values.IsEmpty()) {
    return {
        .value = runtime::Value::Null(),
        .status = ExecutionStatus::OutOfMemory,
        .stack_trace = {},
        .message = {},
    };
  }
  entry.frame.Attach(isolate_.heap(), entry_values);
  frame_count_ = 1;
  return Continue<Debugging>(debugger);
}

template <bool Debugging>
ExecutionResult Execution::Continue(Debugger* debugger) noexcept {
  const auto prepared = frame_count_ != 0 ? CompleteCallSetup() : StepResult{};
  if (!prepared) {
    if (prepared.error() == ExecutionStatus::DebuggerStopped) {
      AbortInitializers();
      return {.status = prepared.error(), .stack_trace = {}, .message = {}};
    }
    if (auto failure = HandleError(prepared.error())) {
      return std::move(*failure);
    }
  }
  while (frame_count_ != 0) {
    if constexpr (Debugging) {
      const DebugContext context(frames_.data(), frame_count_);
      if (debugger->BeforeInstruction(context) == DebugAction::Stop) {
        AbortInitializers();
        return {
            .value = runtime::Value::Null(),
            .status = ExecutionStatus::DebuggerStopped,
            .stack_trace = {},
            .message = {},
        };
      }
    }
    auto& active = std::span(frames_).subspan(frame_count_ - 1).front();
    const auto instruction =
        std::span(active.function->code).subspan(active.program_counter++).front();
    const auto step = ExecuteInstruction(active, instruction);
    if (!step) {
      if (step.error() == ExecutionStatus::DebuggerStopped) {
        AbortInitializers();
        return {.status = step.error(), .stack_trace = {}, .message = {}};
      }
      if (auto failure = HandleError(step.error())) {
        return std::move(*failure);
      }
    }
  }
  return {.value = result_, .status = ExecutionStatus::Returned, .stack_trace = {}, .message = {}};
}

ExecutionResult Execution::Run(std::uint32_t entry_function) noexcept {
  return RunImpl<false>(entry_function, nullptr);
}

ExecutionResult Execution::DebugRun(std::uint32_t entry_function, Debugger& debugger) noexcept {
  return RunImpl<true>(entry_function, &debugger);
}

runtime::FutureDiagnostic Execution::Diagnostic(const ExecutionResult& result) {
  runtime::FutureDiagnostic diagnostic{
      .message = result.message,
      .stack_trace = {},
      .status = result.status,
  };
  for (const auto& frame : result.stack_trace) {
    diagnostic.stack_trace.push_back({
        .function = frame.function,
        .source_path = frame.source_path,
        .line = frame.location.line,
        .column = frame.location.column,
        .instruction = frame.instruction,
    });
  }
  return diagnostic;
}

runtime::FutureDiagnostic Execution::ThrowDiagnostic(runtime::Value payload) const {
  return Diagnostic({
      .value = payload,
      .status = ExecutionStatus::Thrown,
      .stack_trace = CaptureStack(),
      .message = std::string(runtime::ErrorMessage(payload, isolate_.error_layout())),
  });
}

void Execution::CompleteFuture(runtime::Isolate& isolate,
                               runtime::Future& future,
                               const ExecutionResult& result) {
  if (result.Succeeded()) {
    if (result.value.IsObject()) {
      if (const auto* nested = runtime::Heap::GetFuture(*result.value.AsObject())) {
        runtime::CompleteFuture(isolate, future, *nested);
        return;
      }
    }
    ConsumeFutureCompletion(future.Complete(result.value));
  } else {
    runtime::CompleteFutureError(isolate, future, result.value, Execution::Diagnostic(result));
  }
}

runtime::Future Execution::InvokeFuture(runtime::Isolate& isolate,
                                        const Program& program,
                                        runtime::Value callable,
                                        std::span<const runtime::Value> arguments,
                                        std::shared_ptr<DebugSession> debug_session) {
  std::array<RegisterFile, kMaxCallFrames> registers;
  Execution execution(isolate, program, registers, std::move(debug_session));
  const auto invoked = execution.Invoke(callable, arguments);
  if (invoked.Succeeded() && invoked.value.IsObject()) {
    if (const auto* source = runtime::Heap::GetFuture(*invoked.value.AsObject())) {
      return source->Rooted();
    }
  }
  runtime::Future result(&isolate.scheduler());
  result.AttachHeap(isolate.heap());
  CompleteFuture(isolate, result, invoked);
  return result;
}

ExecutionStatus Execution::RestoreFutureError(const runtime::FutureResult& completion) {
  const auto& diagnostic = completion.diagnostic;
  if (!completion.value.IsObject() || completion.value.AsObject() == nullptr) {
    error_message_ = diagnostic.message;
    return diagnostic.status == ExecutionStatus::Thrown ? ExecutionStatus::TypeError
                                                        : diagnostic.status;
  }
  const auto created = CreateException(completion.value, diagnostic.status, diagnostic.message);
  if (!created) {
    return created.error();
  }
  auto& exception = runtime::ExceptionData::From(*pending_exception_.AsObject());
  exception.async_diagnostic = std::make_unique<runtime::FutureDiagnostic>(diagnostic).release();
  const auto boundary = CaptureStack();
  for (const auto& frame : boundary) {
    exception.async_diagnostic->stack_trace.push_back({
        .function = frame.function,
        .source_path = frame.source_path,
        .line = frame.location.line,
        .column = frame.location.column,
        .instruction = frame.instruction,
    });
  }
  return diagnostic.status;
}

std::optional<ExecutionResult> Execution::ResumeFrame(
    SuspendedFrame& suspended,
    const runtime::FutureResult& completion) noexcept {
  resumed_frame_ = &suspended;
  const auto point = suspended.point();
  const auto indices = suspended.indices();
  const auto saved = suspended.saved();
  auto& active = frames_.front();
  active.function = &functions_.subspan(point.function).front();
  auto values = registers_.front().Acquire(active.function->register_count);
  if (values.IsEmpty()) {
    return ExecutionResult{
        .status = ExecutionStatus::OutOfMemory, .stack_trace = {}, .message = {}};
  }
  active.frame.Attach(isolate_.heap(), values);
  for (std::size_t index = 0; index < saved.size(); ++index) {
    values.subspan(indices.subspan(index).front()).front() = saved.subspan(index).front();
  }
  suspended.ReleaseSaved();
  active.program_counter = point.program_counter;
  frame_count_ = 1;
  if (completion.state == runtime::FutureState::Failed) {
    const auto& diagnostic = completion.diagnostic;
    if (diagnostic.status == ExecutionStatus::DebuggerStopped) {
      return ExecutionResult{
          .value = runtime::Value::Null(),
          .status = diagnostic.status,
          .stack_trace = {},
          .message = diagnostic.message,
      };
    }
    const auto status = RestoreFutureError(completion);
    if (auto failure = HandleError(status)) {
      return std::move(*failure);
    }
  } else {
    values.subspan(point.destination).front() = completion.value;
  }
  auto outcome = debug_session_ != nullptr && debug_session_->debugger != nullptr
                     ? Continue<true>(debug_session_->debugger)
                     : Continue<false>(nullptr);
  return resuspended_ ? std::nullopt : std::optional(std::move(outcome));
}

ExecutionResult Execution::Invoke(runtime::Value callable,
                                  std::span<const runtime::Value> arguments) noexcept {
  runtime::RootRange argument_roots(isolate_.heap(), arguments.data(), arguments.size());
  runtime::RootRange callable_root(isolate_.heap(), &callable, 1);
  const auto resolved = ResolveCallable(callable);
  if (!resolved) {
    return {.status = resolved.error(), .stack_trace = {}, .message = {}};
  }
  const auto& [target, implicit] = *resolved;
  const auto& function = functions_.subspan(target).front();
  const auto hidden = implicit.has_value() ? 1U : 0U;
  if (function.parameter_count != arguments.size() + hidden) {
    return {.status = ExecutionStatus::TypeError, .stack_trace = {}, .message = {}};
  }
  if (function.signature) {
    const auto& signature = *function.signature;
    const auto skipped = hidden - signature.hidden_parameters;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
      const auto parameter = signature.parameters.at(index + skipped).type;
      const auto& type = function.layouts->types().subspan(parameter).front();
      if (!runtime::Heap::IsValueOfType(arguments.subspan(index).front(), type)) {
        return {.status = ExecutionStatus::InvalidCast, .stack_trace = {}, .message = {}};
      }
    }
  }
  auto values = registers_.front().Acquire(function.register_count);
  if (values.IsEmpty()) {
    return {.status = ExecutionStatus::OutOfMemory, .stack_trace = {}, .message = {}};
  }
  auto& active = frames_.front();
  active.function = &function;
  active.frame.Attach(isolate_.heap(), values);
  if (implicit) {
    values.front() = *implicit;
  }
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    auto value = arguments.subspan(index).front();
    if (function.signature) {
      const auto& signature = *function.signature;
      const auto parameter =
          signature.parameters.at(index + hidden - signature.hidden_parameters).type;
      const auto& type = function.layouts->types().subspan(parameter).front();
      if (!value.IsNull() && type.instance != nullptr && type.instance->IsEnum()) {
        value = *runtime::Heap::GetField(*value.AsObject(), 0);
      }
    }
    values.subspan(index + hidden).front() = value;
  }
  frame_count_ = 1;
  return debug_session_ != nullptr && debug_session_->debugger != nullptr
             ? Continue<true>(debug_session_->debugger)
             : Continue<false>(nullptr);
}

StepResult Execution::ExecuteInstruction(ActiveFrame& active, Instruction instruction) {
  auto values = active.frame.registers();
  const auto opcode = DecodeOp(instruction);
  const auto operand_a = DecodeA(instruction);
  const auto operand_b = DecodeB(instruction);
  const auto operand_c = DecodeC(instruction);
  switch (opcode) {
    case OpCode::LoadNull:
      values.subspan(operand_a).front().SetNull();
      return {};
    case OpCode::LoadBool:
      values.subspan(operand_a).front().SetBoolean(operand_b != 0);
      return {};
    case OpCode::LoadInt:
      values.subspan(operand_a).front().SetInteger(DecodeImm16(instruction));
      return {};
    case OpCode::LoadIntConstant:
      values.subspan(operand_a).front().SetInteger(
          std::span(active.function->integers).subspan(DecodeUnsigned16(instruction)).front());
      return {};
    case OpCode::LoadDouble:
      values.subspan(operand_a).front().SetDouble(
          std::span(active.function->doubles).subspan(DecodeUnsigned16(instruction)).front());
      return {};
    case OpCode::AddDouble:
    case OpCode::SubDouble:
    case OpCode::MulDouble:
    case OpCode::DivDouble:
    case OpCode::EqualDouble:
    case OpCode::NotEqualDouble:
    case OpCode::LessDouble:
    case OpCode::LessEqualDouble:
    case OpCode::GreaterDouble:
    case OpCode::GreaterEqualDouble:
      return DoubleOperation(
          opcode, values.subspan(operand_a).front(),
          {.left = values.subspan(operand_b).front(), .right = values.subspan(operand_c).front()});
    case OpCode::NegDouble: {
      const auto operand = values.subspan(operand_b).front();
      if (!operand.IsDouble()) {
        return std::unexpected(ExecutionStatus::TypeError);
      }
      values.subspan(operand_a).front().SetDouble(-operand.AsDouble());
      return {};
    }
    case OpCode::Move:
      values.subspan(operand_a).front() = values.subspan(operand_b).front();
      return {};
    case OpCode::AddInt:
    case OpCode::SubInt:
    case OpCode::MulInt:
    case OpCode::DivInt:
    case OpCode::ModInt:
    case OpCode::BitAndInt:
    case OpCode::BitOrInt:
    case OpCode::BitXorInt:
    case OpCode::ShiftLeftInt:
    case OpCode::ShiftRightInt:
    case OpCode::ShiftUnsignedRightInt:
    case OpCode::EqualInt:
    case OpCode::NotEqualInt:
    case OpCode::LessInt:
    case OpCode::LessEqualInt:
    case OpCode::GreaterInt:
    case OpCode::GreaterEqualInt:
      return IntegerOperation(
          opcode, values.subspan(operand_a).front(),
          {.left = values.subspan(operand_b).front(), .right = values.subspan(operand_c).front()});
    case OpCode::NegInt:
    case OpCode::BitNotInt: {
      const auto operand = values.subspan(operand_b).front();
      if (!operand.IsInteger()) {
        return std::unexpected(ExecutionStatus::TypeError);
      }
      const auto value = operand.AsInteger();
      if (opcode == OpCode::BitNotInt) {
        values.subspan(operand_a).front().SetInteger(~value);
        return {};
      }
      if (value == std::numeric_limits<std::int64_t>::min()) {
        return std::unexpected(ExecutionStatus::IntegerOverflow);
      }
      values.subspan(operand_a).front().SetInteger(-value);
      return {};
    }
    case OpCode::NotBool:
      if (!values.subspan(operand_b).front().IsBoolean()) {
        return std::unexpected(ExecutionStatus::TypeError);
      }
      values.subspan(operand_a).front().SetBoolean(!values.subspan(operand_b).front().AsBoolean());
      return {};
    case OpCode::EqualBool:
    case OpCode::NotEqualBool:
    case OpCode::AndBool:
    case OpCode::OrBool:
      return BooleanOperation(
          opcode, values.subspan(operand_a).front(),
          {.left = values.subspan(operand_b).front(), .right = values.subspan(operand_c).front()});
    default:
      return ExecuteObject(active, instruction, opcode, operand_a, operand_b, operand_c);
  }
}

StepResult Execution::ExecuteObject(ActiveFrame& active,
                                    Instruction instruction,
                                    OpCode opcode,
                                    std::uint8_t operand_a,
                                    std::uint8_t operand_b,
                                    std::uint8_t operand_c) {
  auto values = active.frame.registers();
  auto& heap = isolate_.heap();
  switch (opcode) {
    case OpCode::NewOk:
    case OpCode::NewErr:
    case OpCode::ExpectedHasValue:
    case OpCode::ExpectedValue:
      return ExecuteExpected(active, instruction);
    case OpCode::LoadString:
      values.subspan(operand_a).front().SetString(std::span(active.function->strings->values)
                                                      .subspan(DecodeUnsigned16(instruction))
                                                      .front());
      return {};
    case OpCode::EqualValue:
    case OpCode::NotEqualValue: {
      const auto equal = runtime::IsEqualValue(values.subspan(operand_b).front(),
                                               values.subspan(operand_c).front());
      values.subspan(operand_a).front().SetBoolean(opcode == OpCode::EqualValue ? equal : !equal);
      return {};
    }
    case OpCode::BuiltinCall:
      return ExecuteBuiltin(active, instruction);
    case OpCode::StringInterpolate:
      return Interpolate(active, instruction);
    case OpCode::StringConcat:
    case OpCode::EqualString:
    case OpCode::NotEqualString:
      return StringOperation(opcode, values.subspan(operand_a).front(),
                             values.subspan(operand_b).front(), values.subspan(operand_c).front());
    case OpCode::NewArray:
    case OpCode::NewDynamicArray:
    case OpCode::NewInstance:
    case OpCode::NewCollection:
    case OpCode::NewFuture:
    case OpCode::SleepFuture:
      return AllocateObject(active, opcode, operand_a, operand_b, operand_c);
    case OpCode::Await:
      return Await(active, {.destination = operand_a, .source = operand_b});
    case OpCode::GetIndex:
    case OpCode::SetIndex:
      return ArrayOperation(opcode, values.subspan(operand_a).front(),
                            values.subspan(operand_b).front(), values.subspan(operand_c).front());
    case OpCode::SetField: {
      const auto object = values.subspan(operand_a).front();
      if (!object.IsObject() || object.AsObject() == nullptr) {
        return std::unexpected(ExecutionStatus::TypeError);
      }
      if (!heap.SetField(*object.AsObject(), operand_b, values.subspan(operand_c).front())) {
        return std::unexpected(ExecutionStatus::InvalidField);
      }
      return {};
    }
    case OpCode::GetField: {
      const auto object = values.subspan(operand_b).front();
      if (!object.IsObject() || object.AsObject() == nullptr) {
        return std::unexpected(ExecutionStatus::TypeError);
      }
      const auto* field = runtime::Heap::GetField(*object.AsObject(), operand_c);
      if (field == nullptr) {
        return std::unexpected(ExecutionStatus::InvalidField);
      }
      values.subspan(operand_a).front() = *field;
      return {};
    }
    case OpCode::IsType:
    case OpCode::CheckedCast: {
      const auto value = values.subspan(operand_b).front();
      const auto& type = active.function->layouts->types().subspan(operand_c).front();
      const bool matches = runtime::Heap::IsValueOfType(value, type);
      if (opcode == OpCode::CheckedCast && !matches) {
        return std::unexpected(ExecutionStatus::InvalidCast);
      }
      if (opcode == OpCode::IsType) {
        values.subspan(operand_a).front().SetBoolean(matches);
      } else {
        values.subspan(operand_a).front() = value;
      }
      return {};
    }
    case OpCode::Jump:
      Jump(active, DecodeImm16(instruction));
      return {};
    case OpCode::JumpIfFalse: {
      const auto condition = values.subspan(operand_a).front();
      if (!condition.IsBoolean()) {
        return std::unexpected(ExecutionStatus::TypeError);
      }
      if (!condition.AsBoolean()) {
        Jump(active, DecodeImm16(instruction));
      }
      return {};
    }
    case OpCode::JumpIfNull:
      if (values.subspan(operand_a).front().IsNull()) {
        Jump(active, DecodeImm16(instruction));
      }
      return {};
    case OpCode::Print:
      runtime::WriteValue(isolate_.output(), values.subspan(operand_b).front());
      isolate_.output() << '\n';
      return {};
    case OpCode::AsyncCall:
      return ExecuteAsync(active, instruction);
    case OpCode::Catch:
    case OpCode::ExceptionValue:
    case OpCode::Throw:
    case OpCode::Rethrow:
      return ExecuteException(active, instruction);
    case OpCode::Call:
      return Call(active, instruction);
    case OpCode::CallIndirect:
      return CallIndirect(active, instruction);
    case OpCode::CallInterface:
      return CallInterface(active, instruction);
    case OpCode::GetGlobal:
    case OpCode::SetGlobal:
      return AccessGlobal(active, instruction);
    case OpCode::Return:
      return Return(active, values.subspan(operand_a).front());
    default:
      std::unreachable();
  }
}

StepResult Execution::Await(ActiveFrame& active, FutureRegisters registers) {
  const auto source = active.frame.registers().subspan(registers.source).front().Read();
  const SuspensionPoint point{
      .function = static_cast<std::uint32_t>(active.function - functions_.data()),
      .program_counter = active.program_counter,
      .destination = registers.destination,
  };
  const auto values = active.frame.registers();
  if (IsResumedFrame()) {
    resumed_frame_->Suspend(point, source, values);
    resuspended_ = true;
    active.frame.Detach();
    frame_count_ = 0;
    return {};
  }
  runtime::Future result(&isolate_.scheduler());
  auto* object = isolate_.heap().AllocateFuture(result);
  if (object == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  const auto output = runtime::Value::Object(object);
  const runtime::RootRange root(isolate_.heap(), &output, 1);
  auto suspended = std::make_shared<SuspendedFrame>(isolate_, program_.Internal(), output,
                                                    values.size(), debug_session_);
  suspended->Suspend(point, source, values);
  return Return(active, output);
}

StepResult Execution::AllocateObject(ActiveFrame& active,
                                     OpCode opcode,
                                     std::uint8_t operand_a,
                                     std::uint8_t operand_b,
                                     std::uint8_t operand_c) {
  switch (opcode) {
    case OpCode::NewArray:
    case OpCode::NewDynamicArray:
    case OpCode::NewInstance: {
      return AllocateArrayOrInstance(
          active, opcode, {.destination = operand_a, .source = operand_c, .layout = operand_b});
    }
    case OpCode::NewFuture: {
      return AllocateFuture(active, {.destination = operand_a, .source = operand_b}, operand_c);
    }
    case OpCode::SleepFuture:
      return AllocateSleep(active, {.destination = operand_a, .source = operand_b});
    case OpCode::NewCollection:
      return AllocateCollection(active, {.destination = operand_a, .layout = operand_b});
    default:
      std::unreachable();
  }
}

StepResult Execution::AllocateArrayOrInstance(ActiveFrame& active,
                                              OpCode opcode,
                                              FutureRegisters registers) {
  const auto values = active.frame.registers();
  std::size_t length = registers.source;
  if (opcode == OpCode::NewDynamicArray) {
    const auto size = values.subspan(registers.source).front();
    if (!size.IsInteger() || size.AsInteger() < 0) {
      return std::unexpected(ExecutionStatus::IndexOutOfBounds);
    }
    length = static_cast<std::size_t>(size.AsInteger());
  }
  auto* object =
      opcode == OpCode::NewInstance
          ? isolate_.heap().AllocateInstance(
                *active.function->layouts->classes().subspan(registers.layout).front())
          : isolate_.heap().AllocateArray(
                *active.function->layouts->arrays().subspan(registers.layout).front(), length);
  if (object == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  values.subspan(registers.destination).front().SetObject(object);
  return {};
}

StepResult Execution::AllocateFuture(ActiveFrame& active,
                                     FutureRegisters registers,
                                     std::uint8_t error_flag) {
  const auto payload = active.frame.registers().subspan(registers.source).front().Read();
  const auto value = error_flag != 0
                         ? runtime::AllocateFutureError(isolate_, payload, ThrowDiagnostic(payload))
                         : runtime::AllocateFutureValue(isolate_, payload);
  if (!value) {
    return std::unexpected(value.error());
  }
  active.frame.registers().subspan(registers.destination).front() = *value;
  return {};
}

StepResult Execution::AllocateSleep(ActiveFrame& active, FutureRegisters registers) {
  const auto delay = active.frame.registers().subspan(registers.source).front();
  if (!delay.IsInteger()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto value = runtime::AllocateSleepFuture(isolate_, delay.AsInteger());
  if (!value) {
    return std::unexpected(value.error());
  }
  active.frame.registers().subspan(registers.destination).front() = *value;
  return {};
}

StepResult Execution::AllocateCollection(ActiveFrame& active, FutureRegisters registers) {
  const auto& layout = *active.function->layouts->collections().subspan(registers.layout).front();
  auto* collection = isolate_.heap().AllocateCollection(layout);
  if (collection == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  active.frame.registers().subspan(registers.destination).front().SetObject(collection);
  return {};
}

StepResult Execution::ExecuteBuiltin(ActiveFrame& active, Instruction instruction) {
  const auto builtin = static_cast<Builtin>(DecodeB(instruction));
  if (builtin == Builtin::DynamicAsync) {
    return ExecuteDynamicAsync(active, instruction);
  }
  if (builtin >= Builtin::DynamicArrayPrepare && builtin <= Builtin::DynamicArrayValidate) {
    return ExecuteDynamicArray(active, instruction);
  }
  if (builtin == Builtin::DynamicCall) {
    return ExecuteDynamicCall(active, instruction);
  }
  if (builtin == Builtin::DynamicMethodCall) {
    return ExecuteDynamicMethodCall(active, instruction);
  }
  if (builtin == Builtin::DynamicMemberGet || builtin == Builtin::DynamicMemberSet ||
      builtin == Builtin::DynamicResolveCall) {
    return ExecuteDynamicMember(active, instruction);
  }
  if (builtin == Builtin::BindRuntimeType || builtin == Builtin::CheckFuturePayload) {
    return ExecuteTypeBinding(active, instruction);
  }
  if (builtin >= Builtin::DynamicBinary && builtin <= Builtin::DynamicIndexWrite) {
    return ExecuteDynamic(active, instruction);
  }
  if (builtin == Builtin::JsonParse || builtin == Builtin::JsonStringify) {
    return ExecuteJson(active, instruction);
  }
  if (IsRegExpBuiltin(builtin)) {
    return ExecuteRegExp(active, instruction);
  }
  if (IsByteBuiltin(builtin)) {
    auto values = active.frame.registers();
    const auto count = BuiltinArity(builtin);
    std::array<runtime::Value, 4> arguments;
    const auto input = std::span(arguments).first(count);
    values.subspan(DecodeC(instruction), count).CopyTo(input);
    const auto result = runtime::ExecuteByteBuiltin(builtin, input, isolate_.heap());
    if (!result) {
      return std::unexpected(result.error());
    }
    values.subspan(DecodeA(instruction)).front() = *result;
    return {};
  }
  if (builtin == Builtin::DateParse || builtin == Builtin::DateToISOString ||
      builtin == Builtin::DateFromUTC || builtin == Builtin::DateToUTC) {
    return ExecuteCalendar(active, instruction);
  }
  auto values = active.frame.registers();
  const auto arguments = values.subspan(DecodeC(instruction), BuiltinArity(builtin));
  const auto result = ExecuteValueBuiltin(builtin, arguments, isolate_.heap());
  if (!result) {
    return std::unexpected(result.error());
  }
  values.subspan(DecodeA(instruction)).front() = *result;
  return {};
}

StepResult Execution::ExecuteDynamic(ActiveFrame& active, Instruction instruction) {
  const auto builtin = static_cast<Builtin>(DecodeB(instruction));
  const auto count = BuiltinArity(builtin);
  std::array<runtime::Value, 3> storage;
  const auto arguments = std::span(storage).first(count);
  const auto values = active.frame.registers();
  values.subspan(DecodeC(instruction), count).CopyTo(arguments);
  const auto result = runtime::ExecuteDynamicValue(builtin, arguments, isolate_.heap());
  if (!result) {
    return std::unexpected(result.error());
  }
  values.subspan(DecodeA(instruction)).front() = *result;
  return {};
}

StepResult Execution::CompleteCallSetup() {
  auto& active = std::span(frames_).subspan(frame_count_ - 1).front();
  return active.function->native != nullptr ? ExecuteNative(active) : StepResult{};
}

StepResult Execution::ExecuteNative(ActiveFrame& active) {
  if (debug_session_ != nullptr && debug_session_->debugger != nullptr &&
      debug_session_->debugger->BeforeInstruction(DebugContext(frames_.data(), frame_count_)) ==
          DebugAction::Stop) {
    return std::unexpected(ExecutionStatus::DebuggerStopped);
  }
  active.program_counter = 1;
  const auto& function = *active.function;
  auto result = function.native->Invoke(
      isolate_, active.frame.registers().subspan(0, function.parameter_count));
  if (!result) {
    error_message_ = result.error().message;
    return std::unexpected(result.error().kind == runtime::NativeErrorKind::OutOfMemory
                               ? ExecutionStatus::OutOfMemory
                               : ExecutionStatus::NativeFailure);
  }
  auto value = *result;
  runtime::RootRange result_root;
  if (value.IsReference()) {
    // 返回引用尚未写入调用者；后续 Future 验证或返回类型转换可能触发 GC。
    result_root.Attach(isolate_.heap(), &value, 1);
  }
  if (function.signature && function.signature->result) {
    const auto& type = function.layouts->types().subspan(*function.signature->result).front();
    if (type.object_kind == runtime::ObjectKind::Future) {
      auto checked = ValidateNativeFuture(value, type.signature->result);
      if (!checked) {
        return std::unexpected(checked.error());
      }
      value = *checked;
    }
  }
  return Return(active, value);
}

StepResult Execution::Interpolate(ActiveFrame& active, Instruction instruction) {
  auto values = active.frame.registers();
  const auto arguments = values.subspan(DecodeB(instruction), DecodeC(instruction));
  std::array<runtime::StringObject*, kInlineInterpolationParts> inline_parts{};
  std::vector<runtime::StringObject*> allocated_parts;
  std::span<runtime::StringObject*> parts;
  if (arguments.size() <= inline_parts.size()) {
    parts = std::span(inline_parts).first(arguments.size());
  } else {
    allocated_parts.resize(arguments.size());
    parts = allocated_parts;
  }
  std::size_t index = 0;
  for (; index < arguments.size(); ++index) {
    const auto value = arguments.subspan(index).front();
    if (!value.IsString()) {
      break;
    }
    parts.subspan(index).front() = value.AsString();
  }
  auto& heap = isolate_.heap();
  const auto string =
      index == arguments.size()
          ? std::expected<runtime::StringObject*, runtime::StringJoinError>(heap.JoinStrings(parts))
          : heap.JoinValues(arguments);
  if (!string) {
    return std::unexpected(string.error() == runtime::StringJoinError::TypeMismatch
                               ? ExecutionStatus::TypeError
                               : ExecutionStatus::OutOfMemory);
  }
  if (*string == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  values.subspan(DecodeA(instruction)).front().SetString(*string);
  return {};
}

StepResult Execution::StringOperation(OpCode opcode,
                                      runtime::RegisterReference destination,
                                      runtime::Value left,
                                      runtime::Value right) {
  if (opcode == OpCode::StringConcat) {
    const auto result = runtime::ConcatenateValues(isolate_.heap(), left, right);
    if (!result) {
      return std::unexpected(result.error());
    }
    destination = *result;
  } else {
    const auto equal = runtime::CompareStrings(left, right);
    if (!equal) {
      return std::unexpected(equal.error());
    }
    destination.SetBoolean(opcode == OpCode::EqualString ? *equal : !*equal);
  }
  return {};
}

StepResult Execution::ArrayOperation(OpCode opcode,
                                     runtime::RegisterReference destination,
                                     runtime::Value left,
                                     runtime::Value right) {
  const auto object = opcode == OpCode::GetIndex ? left : destination.Read();
  const auto index = opcode == OpCode::GetIndex ? right : left;
  if (!object.IsObject() || !index.IsInteger()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  if (opcode == OpCode::SetIndex) {
    return runtime::WriteIndex(isolate_.heap(), object.AsObject(), index.AsInteger(), right);
  }
  const auto result = runtime::ReadIndex(object.AsObject(), index.AsInteger());
  if (!result) {
    return std::unexpected(result.error());
  }
  destination = *result;
  return {};
}

void Execution::Jump(ActiveFrame& active, std::int16_t offset) noexcept {
  const auto target = static_cast<std::int64_t>(active.program_counter) + offset;
  active.program_counter = static_cast<std::size_t>(target);
}

StepResult Execution::ExecuteExpected(ActiveFrame& active, Instruction instruction) {
  const auto values = active.frame.registers();
  const auto opcode = DecodeOp(instruction);
  auto destination = values.subspan(DecodeA(instruction)).front();
  if (opcode == OpCode::NewOk || opcode == OpCode::NewErr) {
    const auto payload = values.subspan(DecodeC(instruction)).front();
    auto& layout = *active.function->layouts->classes().subspan(DecodeB(instruction)).front();
    auto* object = isolate_.heap().AllocateExpected(layout, payload,
                                                    opcode == OpCode::NewOk
                                                        ? runtime::ExpectedAlternative::Ok
                                                        : runtime::ExpectedAlternative::Err);
    if (object == nullptr) {
      return std::unexpected(ExecutionStatus::OutOfMemory);
    }
    destination = runtime::Value::Object(object);
    return {};
  }
  const auto receiver = values.subspan(DecodeB(instruction)).front();
  if (!receiver.IsObject() || receiver.AsObject() == nullptr) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto payload =
      opcode == OpCode::ExpectedHasValue
          ? runtime::ExpectedHasValue(receiver.AsObject())
          : runtime::ExpectedValue(receiver.AsObject(),
                                   static_cast<runtime::ExpectedAlternative>(DecodeC(instruction)));
  if (!payload) {
    return std::unexpected(payload.error());
  }
  destination = *payload;
  return {};
}

StepResult Execution::Call(ActiveFrame& active, Instruction instruction) {
  return EnterCall(DecodeB(instruction), active, instruction, std::nullopt);
}

StepResult Execution::AccessGlobal(ActiveFrame& active, Instruction instruction) {
  const auto index = DecodeB(instruction);
  auto& state = std::span(globals_->states).subspan(index).front();
  auto& value = std::span(globals_->values).subspan(index).front();
  auto slot = active.frame.registers().subspan(DecodeA(instruction)).front();
  if (DecodeOp(instruction) == OpCode::SetGlobal) {
    globals_->Store(index, slot);
    state = GlobalState::Ready;
    return {};
  }
  switch (state) {
    case GlobalState::Ready:
      slot = value;
      return {};
    case GlobalState::Initializing:
      return std::unexpected(ExecutionStatus::CyclicInitialization);
    case GlobalState::Uninitialized:
      break;
  }
  state = GlobalState::Initializing;
  return EnterCall(std::span(globals_->initializers).subspan(index).front(), active, instruction,
                   std::nullopt, index);
}

std::expected<std::uint32_t, ExecutionStatus> Execution::InterfaceTarget(
    runtime::Value receiver,
    std::uint32_t selector) const {
  return runtime::ResolveInterfaceTarget(selector, receiver, functions_.size());
}

StepResult Execution::CallInterface(ActiveFrame& active, Instruction instruction) {
  const auto base = DecodeC(instruction);
  const auto values = active.frame.registers();
  const auto target = InterfaceTarget(values.subspan(base).front(), DecodeB(instruction));
  if (!target) {
    return std::unexpected(target.error());
  }
  if (static_cast<std::size_t>(base) + functions_.subspan(*target).front().parameter_count >
      values.size()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  return EnterCall(*target, active, instruction, std::nullopt);
}

StepResult Execution::CallIndirect(ActiveFrame& active, Instruction instruction) {
  const auto value = active.frame.registers().subspan(DecodeB(instruction)).front();
  if (!value.IsObject() || value.AsObject() == nullptr) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto resolved = ResolveCallable(value.Read());
  if (!resolved) {
    return std::unexpected(resolved.error());
  }
  const auto& [target, implicit] = *resolved;
  const auto count = functions_.subspan(target).front().parameter_count;
  const auto hidden = implicit.has_value() ? 1U : 0U;
  if (count < hidden || static_cast<std::size_t>(DecodeC(instruction)) + count - hidden >
                            active.frame.registers().size()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  return EnterCall(target, active, instruction, implicit);
}

StepResult Execution::EnterCall(std::uint32_t target,
                                ActiveFrame& active,
                                Instruction instruction,
                                std::optional<runtime::Value> implicit,
                                std::optional<std::size_t> initialized_global) {
  const auto& callee = functions_.subspan(target).front();
  if (callee.native != nullptr && !implicit) {
    if (frame_count_ == kMaxCallFrames) {
      return std::unexpected(ExecutionStatus::StackOverflow);
    }
    auto& next = std::span(frames_).subspan(frame_count_).front();
    next.function = &callee;
    next.program_counter = 0;
    next.return_register = DecodeA(instruction);
    next.initialized_global = initialized_global;
    next.frame.Borrow(
        active.frame.registers().subspan(DecodeC(instruction), callee.parameter_count));
    ++frame_count_;
    return ExecuteNative(next);
  }
  const auto entered = EnterFrame({.function = target, .destination = DecodeA(instruction)});
  if (!entered) {
    return std::unexpected(entered.error());
  }
  const auto callee_values = *entered;
  std::span(frames_).subspan(frame_count_ - 1).front().initialized_global = initialized_global;
  const std::size_t hidden = implicit.has_value() ? 1 : 0;
  if (implicit) {
    callee_values.front() = *implicit;
  }
  callee_values.subspan(hidden, callee.parameter_count - hidden)
      .CopyFrom(
          active.frame.registers().subspan(DecodeC(instruction), callee.parameter_count - hidden));
  return CompleteCallSetup();
}

std::expected<runtime::RegisterView, ExecutionStatus> Execution::EnterFrame(FrameEntry entry) {
  if (frame_count_ == kMaxCallFrames) {
    return std::unexpected(ExecutionStatus::StackOverflow);
  }
  const auto& callee = functions_.subspan(entry.function).front();
  auto callee_values = registers_.subspan(frame_count_).front().Acquire(callee.register_count);
  if (callee_values.IsEmpty()) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  auto& next = std::span(frames_).subspan(frame_count_).front();
  next.function = &callee;
  next.program_counter = 0;
  next.return_register = entry.destination;
  next.initialized_global.reset();
  next.frame.Attach(isolate_.heap(), callee_values);
  ++frame_count_;
  return callee_values;
}

bool Execution::IsErasedReturn() const {
  if (frame_count_ == 1) {
    return true;
  }
  const auto& caller = std::span(frames_).subspan(frame_count_ - 2).front();
  return caller.json_encoder || caller.dynamic_return;
}

StepResult Execution::Return(ActiveFrame& active, runtime::Value value) {
  const auto resumed = IsResumedFrame();
  if (!resumed && active.function->is_async &&
      (!value.IsObject() || runtime::Heap::GetFuture(*value.AsObject()) == nullptr)) {
    auto future = runtime::Future::Value(value, &isolate_.scheduler());
    auto* object = isolate_.heap().AllocateFuture(std::move(future));
    if (object == nullptr) {
      return std::unexpected(ExecutionStatus::OutOfMemory);
    }
    value = runtime::Value::Object(object);
  }
  if (active.initialized_global) {
    const auto index = *active.initialized_global;
    globals_->Store(index, value);
    std::span(globals_->states).subspan(index).front() = GlobalState::Ready;
  }
  if (value.IsObject() && (!resumed || runtime::Heap::GetFuture(*value.AsObject()) != nullptr) &&
      active.function->signature && active.function->signature->result) {
    const auto& type =
        active.function->layouts->types().subspan(*active.function->signature->result).front();
    if (type.object_kind == runtime::ObjectKind::Future ||
        type.object_kind == runtime::ObjectKind::Completer) {
      if (auto bound = runtime::Heap::BindRuntimeType(*value.AsObject(), type); !bound) {
        return bound;
      }
    }
  }
  const auto return_register = active.return_register;
  const auto erased_return = !resumed && IsErasedReturn();
  if (erased_return && active.function->signature && active.function->signature->result) {
    const auto& type =
        active.function->layouts->types().subspan(*active.function->signature->result).front();
    const auto boxed = BoxDynamicValue(value, type);
    if (!boxed) {
      return std::unexpected(boxed.error());
    }
    value = *boxed;
  }
  active.frame.Detach();
  --frame_count_;
  if (frame_count_ == 0) {
    result_ = value;
    return {};
  }
  auto& caller = std::span(frames_).subspan(frame_count_ - 1).front();
  caller.dynamic_return = false;
  if (caller.json_encoder) {
    caller.json_encoder->ResumeConversion(value);
    return ContinueJson(caller);
  }
  caller.frame.registers().subspan(return_register).front() = value;
  if (caller.dynamic_assignment) {
    caller.frame.registers().subspan(return_register).front() =
        caller.frame.registers().subspan(*caller.dynamic_assignment).front();
    caller.dynamic_assignment.reset();
  }
  if (caller.dynamic_call_plan) {
    caller.dynamic_call_plan = false;
    const auto plan = CreateDynamicCallPlan(value);
    if (!plan) {
      return std::unexpected(plan.error());
    }
    caller.frame.registers().subspan(return_register).front() = *plan;
  }
  return {};
}

}  // namespace

ExecutionResult VirtualMachine::Execute(const FunctionHandle& function) noexcept {
  return Execute(function.program(), function.index());
}

std::size_t VirtualMachine::register_bytes() const noexcept {
  std::size_t bytes = 0;
  for (const auto& registers : registers_) {
    bytes += registers.capacity() * runtime::kRegisterSlotBytes;
  }
  return bytes;
}

ExecutionResult VirtualMachine::Execute(const Program& program,
                                        std::uint32_t entry_function) noexcept {
  return ExecuteImpl(program, entry_function, nullptr);
}

runtime::Future VirtualMachine::ExecuteFuture(const Program& program,
                                              std::uint32_t entry_function) noexcept {
  const auto executed = Execute(program, entry_function);
  if (executed.Succeeded() && executed.value.IsObject()) {
    if (const auto* source = runtime::Heap::GetFuture(*executed.value.AsObject())) {
      return source->Rooted();
    }
  }
  runtime::Future result(&isolate_.scheduler());
  result.AttachHeap(isolate_.heap());
  internal::Execution::CompleteFuture(isolate_, result, executed);
  return result;
}

ExecutionResult VirtualMachine::Debug(const FunctionHandle& function, Debugger& debugger) noexcept {
  return ExecuteImpl(function.program(), function.index(), &debugger);
}

ExecutionResult VirtualMachine::ExecuteImpl(const Program& program,
                                            std::uint32_t entry_function,
                                            Debugger* debugger) noexcept {
  const auto functions = program.functions();
  if (entry_function >= functions.size() ||
      functions.subspan(entry_function).front().parameter_count != 0) {
    return {
        .value = runtime::Value::Null(),
        .status = ExecutionStatus::InvalidEntry,
        .stack_trace = {},
        .message = {},
    };
  }
  if (!program.IsForIsolate(isolate_)) {
    return {
        .value = runtime::Value::Null(),
        .status = ExecutionStatus::InvalidIsolate,
        .stack_trace = {},
        .message = {},
    };
  }
  if (executing_) {
    // 同一 VM 的重入调用不能清空或扩容外层仍在借用的寄存器窗口。
    VirtualMachine nested(isolate_);
    return nested.ExecuteImpl(program, entry_function, debugger);
  }
  executing_ = true;
  const absl::Cleanup execution_finished = [this] { executing_ = false; };
  if (debugger == nullptr) {
    internal::Execution execution(isolate_, program, registers_);
    return execution.Run(entry_function);
  }
  auto session = std::make_shared<internal::DebugSession>(internal::DebugSession{debugger});
  internal::Execution execution(isolate_, program, registers_, session);
  auto result = execution.DebugRun(entry_function, *debugger);
  runtime::RootRange result_root(isolate_.heap(), &result.value, 1);
  if (result.Succeeded() && result.value.IsObject()) {
    if (auto* source = runtime::Heap::GetFuture(*result.value.AsObject())) {
      auto root = source->Rooted();
      root.Observe([](runtime::FutureState, runtime::Value) {});
      if (!isolate_.scheduler().PumpUntil(root, runtime::FuturePumpMode::WaitForHost)) {
        result = {
            .value = runtime::Value::Null(),
            .status = ExecutionStatus::AsyncIdle,
            .stack_trace = {},
            .message = {},
        };
      } else if (root.state() == runtime::FutureState::Failed) {
        const auto diagnostic = root.error_diagnostic().value_or(runtime::FutureDiagnostic{});
        result = {
            .value = root.error().value_or(runtime::Value::Null()),
            .status = diagnostic.status,
            .stack_trace = {},
            .message = diagnostic.message,
        };
        for (const auto& frame : diagnostic.stack_trace) {
          result.stack_trace.push_back({
              .function = frame.function,
              .source_path = frame.source_path,
              .location = {.line = frame.line, .column = frame.column},
              .instruction = frame.instruction,
          });
        }
      } else {
        result.value = root.value().value_or(runtime::Value::Null());
      }
    }
  }
  if (result.status != ExecutionStatus::DebuggerStopped &&
      result.status != ExecutionStatus::AsyncIdle) {
    while (isolate_.scheduler().PumpOne(runtime::FuturePumpMode::WaitForHost)) {
    }
  }
  session->debugger = nullptr;
  return result;
}

}  // namespace vela::vm
