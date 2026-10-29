#include "vm/execution.h"

#include <array>
#include <expected>
#include <span>

#include "base/builtin.h"
#include "runtime/dynamic_array.h"
#include "runtime/heap.h"
#include "runtime/value.h"
#include "vm/bytecode.h"
#include "vm/program.h"

namespace vela::vm::internal {

StepResult Execution::ExecuteDynamicArray(ActiveFrame& active, Instruction instruction) {
  const auto builtin = static_cast<Builtin>(DecodeB(instruction));
  const auto count = BuiltinArity(builtin);
  std::array<runtime::Value, kDynamicOperationArity> storage;
  const auto input = std::span(storage).first(count);
  const auto registers = active.frame.registers();
  registers.subspan(DecodeC(instruction), count).CopyTo(input);
  if (builtin == Builtin::DynamicArrayInvoke) {
    const auto request = runtime::PrepareDynamicArrayInvocation(isolate_.heap(), input);
    if (!request) {
      return std::unexpected(request.error());
    }
    const auto callable = ResolveCallable(request->callback);
    if (!callable) {
      return std::unexpected(callable.error());
    }
    return EnterDynamicCall(active, *callable, *request->arguments, *request->names,
                            DecodeA(instruction), DynamicCallStyle::Function);
  }
  std::expected<runtime::Value, ExecutionStatus> value;
  if (builtin == Builtin::DynamicArrayPrepare) {
    const auto callable = ResolveCallable(input[1]);
    if (!callable) {
      return std::unexpected(callable.error());
    }
    const auto* signature = runtime::Heap::ClassOf(*input[1].AsObject())->callable_type();
    if (signature == nullptr) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    value = runtime::PrepareDynamicArray(isolate_, input, active.function->layouts->classes(),
                                         *signature);
  } else {
    value = runtime::ReadDynamicArrayOperation(builtin, input);
  }
  if (!value) {
    return std::unexpected(value.error());
  }
  registers.subspan(DecodeA(instruction)).front() = *value;
  return {};
}

}  // namespace vela::vm::internal
