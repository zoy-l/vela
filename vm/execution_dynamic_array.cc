#include "vm/execution.h"

#include <array>
#include <expected>
#include <span>

#include "base/builtin.h"
#include "runtime/dynamic_array.h"
#include "runtime/heap.h"
#include "runtime/value.h"
#include "vm/bytecode.h"
#include "vm/execution_status.h"
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
    return EnterDynamicCall(active, {.value = request->callback}, *request->arguments,
                            *request->names, DecodeA(instruction));
  }
  std::expected<runtime::Value, ExecutionStatus> value;
  if (builtin == Builtin::DynamicArrayPrepare) {
    const auto callback = input.subspan(1).front();
    const auto callable = ResolveCallable(callback);
    if (!callable) {
      return std::unexpected(callable.error());
    }
    const auto* signature = runtime::Heap::ClassOf(*callback.AsObject())->callable_type();
    if (signature == nullptr) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    value = runtime::PrepareDynamicArray(isolate_, input, active.function->layouts->classes(),
                                         *signature);
  } else {
    value = runtime::ReadDynamicArrayOperation(builtin, input);
  }
  return StoreResult(registers.subspan(DecodeA(instruction)).front(), value);
}

}  // namespace vela::vm::internal
