#include "runtime/native/native_test.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <utility>

#include "runtime/isolate/isolate.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_call.h"
#include "runtime/strings/string.h"
#include "runtime/testing/test_bridge.h"

namespace vela::runtime {
namespace {

std::unexpected<ExecutionStatus> Failure(NativeExecution& execution, std::string_view message) {
  return std::unexpected(execution.HandleFailure(ExecutionStatus::TypeError, message));
}

bool HasRequiredParameters(std::span<const CallSignature::Parameter> parameters) {
  return std::ranges::any_of(parameters, [](const auto& parameter) {
    return !parameter.optional && !parameter.rest;
  });
}

std::expected<Value, ExecutionStatus> Register(NativeExecution& execution,
                                              std::span<const Value> values) {
  const auto opcode = values.front();
  if (!opcode.IsInteger() || opcode.AsInteger() < 0 ||
      opcode.AsInteger() > static_cast<std::int64_t>(TestOperation::AddTearDown)) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto operation = static_cast<TestOperation>(opcode.AsInteger());
  const auto description = values.subspan(1).front();
  const auto timeout_mode = values.subspan(5).front();
  const auto milliseconds = values.subspan(6).front();
  const auto multiplier = values.subspan(7).front();
  if (!description.IsString() || !timeout_mode.IsInteger() || !milliseconds.IsInteger() ||
      !multiplier.IsDouble()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto callback = values.subspan(2).front();
  if (operation != TestOperation::ExitGroup) {
    const auto contract = InspectNativeCallable(execution.instance(), callback);
    if (!contract) {
      return Failure(execution, "test callback must be a non-null callable from this program");
    }
    if (HasRequiredParameters(contract->parameters)) {
      return Failure(execution, "test callback must accept zero arguments");
    }
    if (operation == TestOperation::EnterGroup &&
        (contract->is_async || contract->signature->result)) {
      return Failure(execution, "group callback must be synchronous and return void");
    }
  }
  auto* bridge = execution.isolate().test_bridge();
  if (bridge == nullptr) {
    return Failure(execution, "sdk:test registration requires a test execution session");
  }
  auto diagnostic = execution.Diagnostic(ExecutionStatus::Returned, {});
  const auto registered = bridge->Register({
      .operation = operation,
      .description = description.AsString()->value(),
      .callback = callback,
      .skip = values.subspan(3).front(),
      .tags = values.subspan(4).front(),
      .timeout_mode = timeout_mode.AsInteger(),
      .milliseconds = milliseconds.AsInteger(),
      .multiplier = multiplier.AsDouble(),
      .trace = std::move(diagnostic.stack_trace),
  });
  if (!registered) {
    return Failure(execution, registered.error());
  }
  return Value::Null();
}

std::expected<void, ExecutionStatus> ValidateCallback(NativeExecution& execution,
                                                       TestControl operation,
                                                       std::span<const Value> values) {
  const auto contract = InspectNativeCallable(execution.instance(), values.subspan(1).front());
  if (operation == TestControl::RegisterFinalizer) {
    if (!contract || contract->is_async || contract->signature->result ||
        !contract->parameters.empty()) {
      return Failure(execution,
                     "test scope finalizer must be synchronous, accept no arguments and return void");
    }
    return {};
  }
  const auto arity = values.subspan(4).front();
  if (!contract || !arity.IsInteger() || (arity.AsInteger() != 0 && arity.AsInteger() != 1)) {
    return Failure(execution, "expectAsync requires a callable with the requested arity");
  }
  const auto parameters = contract->parameters;
  const auto count = static_cast<std::size_t>(arity.AsInteger());
  if (parameters.size() < count || HasRequiredParameters(parameters.subspan(count))) {
    return Failure(execution, "expectAsync callback does not accept the requested arguments");
  }
  return {};
}

std::expected<Value, ExecutionStatus> Control(NativeExecution& execution,
                                             std::span<const Value> values) {
  const auto opcode = values.front();
  if (!opcode.IsInteger() || opcode.AsInteger() < 0 ||
      opcode.AsInteger() > static_cast<std::int64_t>(TestControl::TrackCallWait)) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto operation = static_cast<TestControl>(opcode.AsInteger());
  if (operation == TestControl::RegisterFinalizer || operation == TestControl::RegisterCallback) {
    if (const auto valid = ValidateCallback(execution, operation, values); !valid) {
      return std::unexpected(valid.error());
    }
  }
  auto* bridge = execution.isolate().test_bridge();
  if (bridge == nullptr) {
    if (operation == TestControl::CurrentTestScope) {
      return Value::Integer(0);
    }
    return Failure(execution, "asynchronous expectations require a test execution session");
  }
  auto diagnostic = execution.Diagnostic(ExecutionStatus::Returned, {});
  const auto controlled =
      bridge->Control(operation, values.subspan(1), std::move(diagnostic.stack_trace));
  if (!controlled) {
    return Failure(execution, controlled.error());
  }
  return *controlled;
}

}  // namespace

std::expected<Value, ExecutionStatus> ExecuteNativeTest(NativeExecution& execution,
                                                        Builtin builtin,
                                                        std::span<const Value> arguments) {
  return builtin == Builtin::TestRegister ? Register(execution, arguments)
                                           : Control(execution, arguments);
}

}  // namespace vela::runtime
