#include "runtime/native/native_json.h"

#include <expected>
#include <span>
#include <string_view>

#include "base/builtin.h"
#include "base/execution_status.h"
#include "base/member.h"
#include "runtime/dynamic/dynamic_call.h"
#include "runtime/isolate/isolate.h"
#include "runtime/json/json.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_call.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_instance.h"
#include "runtime/strings/string.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {
namespace {

std::unexpected<ExecutionStatus> Failure(NativeExecution& execution,
                                         const JsonRuntimeError& error) {
  const auto payload = CreateJsonError(execution.isolate(), error);
  return std::unexpected(payload ? execution.Throw(*payload) : payload.error());
}

std::expected<Value, ExecutionStatus> Convert(NativeExecution& execution,
                                              JsonEncoder& encoder,
                                              Value receiver) {
  const auto& instance = execution.instance();
  const auto invalid = [&] {
    return Failure(execution,
                   encoder.ConversionError(
                       "JSON object requires a public zero-argument toJson() returning a value"));
  };
  const auto* member = instance.members().Find(receiver, "toJson");
  if (member == nullptr || member->kind != MemberKind::Method) {
    return invalid();
  }
  const auto* const callable = instance.layouts().classes.at(member->target)->callable();
  if (callable == nullptr) {
    return invalid();
  }
  const auto selected = ResolveNativeMethod(instance, *callable, receiver);
  if (!selected) {
    return std::unexpected(selected.error());
  }
  auto origin = NativeInstance::Borrow(*selected->state);
  const NativeInstanceScope scope(execution, origin);
  const auto& function = *selected->function;
  if ((function.flags & kNativeTypedFunction) == 0 || function.result_type == kNoNativeIndex) {
    return invalid();
  }
  const auto& signature = origin.call_signature(selected->target.function);
  const auto bound = BindDynamicArguments(signature, std::span<const std::string_view>{},
                                          DynamicCallStyle::Method);
  if (!bound) {
    return invalid();
  }
  return InvokeBoundNativeTarget(execution, selected->target.function, signature, *bound,
                                 std::span<const Value>{}, receiver,
                                 NativeArgumentPresence::Absent);
}

}  // namespace

std::expected<Value, ExecutionStatus> ExecuteNativeJson(NativeExecution& execution,
                                                        Builtin builtin,
                                                        Value input) {
  auto& isolate = execution.isolate();
  if (builtin == Builtin::JsonParse) {
    const auto parsed = ParseJson(input.AsString()->value(), isolate);
    if (!parsed) {
      return Failure(execution, parsed.error());
    }
    return *parsed;
  }
  JsonEncoder encoder(isolate, input);
  for (;;) {
    const auto step = encoder.Advance();
    if (!step) {
      return Failure(execution, step.error());
    }
    if (step->IsString()) {
      return *step;
    }
    const auto converted = Convert(execution, encoder, *step);
    if (!converted) {
      return std::unexpected(converted.error());
    }
    // ResumeConversion 在下一次分配前接管转换结果的根。
    encoder.ResumeConversion(*converted);
  }
}

}  // namespace vela::runtime
