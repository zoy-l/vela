#include <vela/handles.h>
#include <expected>
#include <span>

#include "base/execution_status.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/value/value.h"

namespace vela {

namespace internal {

runtime::Heap& HandleScopeHeap(runtime::Isolate& isolate) noexcept {
  return isolate.heap();
}

bool IsHandleScopeOwnerThread(runtime::Isolate& isolate) noexcept {
  return isolate.scheduler().IsOwnerThread();
}

}  // namespace internal

std::expected<runtime::Value*, ExecutionStatus> HandleScope::Store(runtime::Value value) noexcept {
  auto& heap = isolate_->heap();
  if (heap.IsCollecting() && value.IsReference()) {
    heap.WriteRootBarrier(std::span(&value, 1));
  }
  return StoreRooted(value);
}

}  // namespace vela
