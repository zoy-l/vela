#include "runtime/async/execution_context.h"

#include <optional>

#include "runtime/async/future.h"

namespace vela::runtime {

ExecutionContextScope::ExecutionContextScope(FutureScheduler& scheduler,
                                             std::optional<ExecutionContext> context)
    : scheduler_(&scheduler), previous_(scheduler.context()) {
  scheduler.SetExecutionContext(context);
}

ExecutionContextScope::~ExecutionContextScope() {
  scheduler_->SetExecutionContext(previous_);
}

}  // namespace vela::runtime
