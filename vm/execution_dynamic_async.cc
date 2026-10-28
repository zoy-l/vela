#include "vm/execution.h"

#include <array>
#include <expected>
#include <span>

#include "base/async_operation.h"
#include "base/builtin.h"
#include "runtime/dynamic_async.h"
#include "runtime/future.h"
#include "runtime/value.h"
#include "vm/bytecode.h"
#include "vm/program.h"

namespace vela::vm::internal {

StepResult Execution::ExecuteDynamicAsync(ActiveFrame& active, Instruction instruction) {
  std::array<runtime::Value, kDynamicOperationArity> values;
  active.frame.registers().subspan(DecodeC(instruction), values.size()).CopyTo(values);
  const auto selector = std::span(values).subspan(3).front();
  const auto diagnostic =
      selector.IsInteger() &&
              selector.AsInteger() == static_cast<int>(AsyncOperation::CompleterError)
          ? ThrowDiagnostic(std::span(values).subspan(1).front())
          : runtime::FutureDiagnostic();
  const auto result = runtime::ExecuteDynamicAsyncOperation(
      isolate_, DescribeFutureCall(AsyncOperation::Count, values),
      active.function->layouts->types(), MakeFutureCallbacks(), diagnostic);
  if (!result) {
    return std::unexpected(result.error());
  }
  active.frame.registers().subspan(DecodeA(instruction)).front() = *result;
  return {};
}

}  // namespace vela::vm::internal
