#include "vm/execution.h"

#include <array>
#include <span>

#include "base/async_operation.h"
#include "base/builtin.h"
#include "runtime/async/future.h"
#include "runtime/dynamic/dynamic_async.h"
#include "runtime/value/value.h"
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
  return StoreResult(active.frame.registers().subspan(DecodeA(instruction)).front(), result);
}

}  // namespace vela::vm::internal
