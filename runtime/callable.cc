#include "runtime/callable.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include "base/callable.h"
#include "base/execution_status.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/value.h"

namespace vela::runtime {

std::expected<std::uint32_t, ExecutionStatus> ResolveInterfaceTarget(std::uint32_t selector,
                                                                     Value receiver,
                                                                     std::size_t function_count) {
  const auto* object = receiver.IsObject() ? receiver.AsObject() : nullptr;
  const auto* layout = object != nullptr ? Heap::ClassOf(*object) : nullptr;
  if (layout == nullptr || selector >= layout->dispatch_targets().size()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto function = layout->dispatch_targets().subspan(selector).front();
  if (function >= function_count) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  return function;
}

std::expected<ResolvedCallable, ExecutionStatus> ResolveCallable(Value value,
                                                                 const CodeIdentity& identity,
                                                                 std::size_t function_count) {
  const auto* object = value.IsObject() ? value.AsObject() : nullptr;
  const auto* layout = object != nullptr ? Heap::ClassOf(*object) : nullptr;
  if (layout == nullptr || !layout->callable() || layout->code_identity() != &identity) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto target = *layout->callable();
  ResolvedCallable result{.function = target.function};
  switch (target.mode) {
    case CallableMode::Plain:
      break;
    case CallableMode::Closure:
      result.implicit = value;
      break;
    case CallableMode::BoundMethod:
    case CallableMode::InterfaceMethod: {
      const auto* receiver = Heap::GetField(*object, 0);
      if (receiver == nullptr) {
        return std::unexpected(ExecutionStatus::TypeError);
      }
      result.implicit = *receiver;
      if (target.mode == CallableMode::InterfaceMethod) {
        const auto resolved = ResolveInterfaceTarget(target.function, *receiver, function_count);
        if (!resolved) {
          return std::unexpected(resolved.error());
        }
        result.function = *resolved;
      }
      break;
    }
  }
  if (result.function >= function_count) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  return result;
}

}  // namespace vela::runtime
