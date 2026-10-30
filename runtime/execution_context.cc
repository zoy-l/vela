#include "runtime/execution_context.h"

#include <optional>

#include "runtime/future.h"

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
