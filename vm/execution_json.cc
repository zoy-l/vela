#include "base/member.h"
#include "vm/execution.h"

#include <expected>
#include <optional>
#include <utility>

#include "base/builtin.h"
#include "runtime/exceptions/error.h"
#include "runtime/json/json.h"
#include "runtime/strings/string.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"
#include "vm/bytecode.h"
#include "vm/execution_status.h"

namespace vela::vm::internal {

StepResult Execution::JsonError(const runtime::JsonRuntimeError& error) {
  const auto payload = runtime::CreateJsonError(isolate_, error);
  if (!payload) {
    return std::unexpected(payload.error());
  }
  const auto message = runtime::Heap::GetField(*payload->AsObject(), runtime::kErrorMessageField)
                           ->AsString()
                           ->value();
  if (auto created = CreateException(*payload, ExecutionStatus::Thrown, message); !created) {
    return created;
  }
  return std::unexpected(ExecutionStatus::Thrown);
}

StepResult Execution::ExecuteJson(ActiveFrame& active, Instruction instruction) {
  const auto values = active.frame.registers();
  const auto input = values.subspan(DecodeC(instruction)).front().Read();
  if (static_cast<Builtin>(DecodeB(instruction)) == Builtin::JsonParse) {
    if (!input.IsString()) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    const auto parsed = runtime::ParseJson(input.AsString()->value(), isolate_);
    if (!parsed) {
      return JsonError(parsed.error());
    }
    values.subspan(DecodeA(instruction)).front() = *parsed;
    return {};
  }
  active.json_encoder.emplace(isolate_, input);
  active.json_destination = DecodeA(instruction);
  return ContinueJson(active);
}

StepResult Execution::ContinueJson(ActiveFrame& active) {
  if (!active.json_encoder) {
    std::unreachable();
  }
  auto& encoder = *active.json_encoder;
  const auto step = encoder.Advance();
  if (!step) {
    return JsonError(step.error());
  }
  if (step->IsString()) {
    active.frame.registers().subspan(active.json_destination).front() = *step;
    active.json_encoder.reset();
    return {};
  }
  const auto* method = active.function->layouts->FindMember(*step, "toJson");
  if (method == nullptr || method->kind != MemberKind::Method) {
    return JsonError(encoder.ConversionError(
        "JSON object requires a public zero-argument toJson() returning a value"));
  }
  const auto* const callable =
      active.function->layouts->classes().subspan(method->target).front()->callable();
  if (callable == nullptr) {
    std::unreachable();
  }
  const auto target = callable->function;
  const auto& function = functions_.subspan(target).front();
  const auto& signature = function.signature;
  if (!signature || !signature->parameters.empty() || !signature->result) {
    return JsonError(encoder.ConversionError(
        "JSON object requires a public zero-argument toJson() returning a value"));
  }
  // 参数和返回值经普通帧传递；JSON builtin 不重入解释器，不改写用户异常与 trace。
  return EnterCall(target, active, EncodeABC(OpCode::Call, active.json_destination, 0, 0), *step);
}

}  // namespace vela::vm::internal
