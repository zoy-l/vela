#include "vm/execution.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <utility>

#include "base/builtin.h"
#include "runtime/exception.h"
#include "runtime/future.h"
#include "runtime/heap.h"
#include "runtime/string.h"
#include "runtime/test_bridge.h"
#include "runtime/value.h"
#include "vm/bytecode.h"
#include "vm/execution_status.h"
#include "vm/program.h"

namespace vela::vm::internal {

StepResult Execution::ValidateTestFinalizer(runtime::Value value) {
  const auto resolved = ResolveCallable(value);
  if (!resolved) {
    return std::unexpected(resolved.error());
  }
  const auto& function = functions_.subspan(resolved->function).front();
  const auto& signature = function.signature;
  if (!signature) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto skipped = (resolved->implicit ? 1U : 0U) - signature->hidden_parameters;
  if (function.is_async || signature->result || signature->parameters.size() != skipped) {
    error_message_ =
        "test scope finalizer must be synchronous, accept no arguments and return void";
    return std::unexpected(ExecutionStatus::TypeError);
  }
  return {};
}

StepResult Execution::ExecuteTestRegistration(ActiveFrame& active, Instruction instruction) {
  constexpr auto kArity = BuiltinArity(Builtin::TestRegister);
  std::array<runtime::Value, kArity> values;
  active.frame.registers().subspan(DecodeC(instruction), kArity).CopyTo(values);
  const auto opcode = values.front();
  if (!opcode.IsInteger() || opcode.AsInteger() < 0 ||
      opcode.AsInteger() > static_cast<std::int64_t>(runtime::TestOperation::AddTearDown)) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto operation = static_cast<runtime::TestOperation>(opcode.AsInteger());
  const auto description = values.at(1);
  const auto timeout_mode = values.at(5);
  const auto milliseconds = values.at(6);
  const auto multiplier = values.at(7);
  if (!description.IsString() || !timeout_mode.IsInteger() || !milliseconds.IsInteger() ||
      !multiplier.IsDouble()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  if (operation != runtime::TestOperation::ExitGroup) {
    const auto resolved = ResolveCallable(values.at(2));
    if (!resolved) {
      error_message_ = "test callback must be a non-null callable from this program";
      return std::unexpected(resolved.error());
    }
    const auto& function = functions_.subspan(resolved->function).front();
    const auto& signature = function.signature;
    if (!signature) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    const auto skipped = (resolved->implicit ? 1U : 0U) - signature->hidden_parameters;
    const auto parameters = std::span(signature->parameters).subspan(skipped);
    if (std::ranges::any_of(parameters, [](const auto& parameter) -> bool {
          return !parameter.optional && !parameter.rest;
        })) {
      error_message_ = "test callback must accept zero arguments";
      return std::unexpected(ExecutionStatus::TypeError);
    }
    if (operation == runtime::TestOperation::EnterGroup &&
        (function.is_async || signature->result)) {
      error_message_ = "group callback must be synchronous and return void";
      return std::unexpected(ExecutionStatus::TypeError);
    }
  }
  auto* bridge = isolate_.test_bridge();
  if (bridge == nullptr) {
    error_message_ = "sdk:test registration requires a test execution session";
    return std::unexpected(ExecutionStatus::TypeError);
  }
  auto diagnostic = Diagnostic({.stack_trace = CaptureStack(), .message = {}, .origin = {}});
  const auto registered = bridge->Register({
      .operation = operation,
      .description = description.AsString()->value(),
      .callback = values.at(2),
      .skip = values.at(3),
      .tags = values.at(4),
      .timeout_mode = timeout_mode.AsInteger(),
      .milliseconds = milliseconds.AsInteger(),
      .multiplier = multiplier.AsDouble(),
      .trace = std::move(diagnostic.stack_trace),
  });
  if (!registered) {
    error_message_ = registered.error();
    return std::unexpected(ExecutionStatus::TypeError);
  }
  active.frame.registers().subspan(DecodeA(instruction)).front().SetNull();
  return {};
}

StepResult Execution::ExecuteTestControl(ActiveFrame& active, Instruction instruction) {
  constexpr auto kArity = BuiltinArity(Builtin::TestControl);
  std::array<runtime::Value, kArity> values;
  active.frame.registers().subspan(DecodeC(instruction), kArity).CopyTo(values);
  const auto opcode = values.front();
  if (!opcode.IsInteger() || opcode.AsInteger() < 0 ||
      opcode.AsInteger() > static_cast<std::int64_t>(runtime::TestControl::TrackCallWait)) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto operation = static_cast<runtime::TestControl>(opcode.AsInteger());
  if (operation == runtime::TestControl::RegisterFinalizer) {
    const auto valid = ValidateTestFinalizer(values.at(1));
    if (!valid) {
      return valid;
    }
  }
  if (operation == runtime::TestControl::RegisterCallback) {
    const auto resolved = ResolveCallable(values.at(1));
    const auto arity = values.at(4);
    if (!resolved || !arity.IsInteger() || (arity.AsInteger() != 0 && arity.AsInteger() != 1)) {
      error_message_ = "expectAsync requires a callable with the requested arity";
      return std::unexpected(ExecutionStatus::TypeError);
    }
    const auto& function = functions_.subspan(resolved->function).front();
    const auto& signature = function.signature;
    if (!signature) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    const auto skipped = (resolved->implicit ? 1U : 0U) - signature->hidden_parameters;
    const auto parameters = std::span(signature->parameters).subspan(skipped);
    const auto count = static_cast<std::size_t>(arity.AsInteger());
    if (parameters.size() < count ||
        std::ranges::any_of(parameters.subspan(count), [](const auto& parameter) -> bool {
          return !parameter.optional && !parameter.rest;
        })) {
      error_message_ = "expectAsync callback does not accept the requested arguments";
      return std::unexpected(ExecutionStatus::TypeError);
    }
  }
  auto* bridge = isolate_.test_bridge();
  if (bridge == nullptr && operation == runtime::TestControl::CurrentTestScope) {
    active.frame.registers().subspan(DecodeA(instruction)).front().SetInteger(0);
    return {};
  }
  if (bridge == nullptr) {
    error_message_ = "asynchronous expectations require a test execution session";
    return std::unexpected(ExecutionStatus::TypeError);
  }
  auto diagnostic = Diagnostic({.stack_trace = CaptureStack(), .message = {}, .origin = {}});
  const auto controlled =
      bridge->Control(operation, std::span(values).subspan(1), std::move(diagnostic.stack_trace));
  if (!controlled) {
    error_message_ = controlled.error();
    return std::unexpected(ExecutionStatus::TypeError);
  }
  active.frame.registers().subspan(DecodeA(instruction)).front() = *controlled;
  return {};
}

}  // namespace vela::vm::internal
