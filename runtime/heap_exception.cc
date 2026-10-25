#include "runtime/exception.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>

#include "base/execution_status.h"
#include "runtime/heap.h"
#include "runtime/object.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

auto TraceStorage(ExceptionData& exception) -> std::span<TracePosition> {
  void* storage = std::span(&exception, 1).last(0).data();
  return {static_cast<TracePosition*>(storage), exception.trace_count};
}

}  // namespace

static_assert(std::is_standard_layout_v<ExceptionData>);
static_assert(std::is_trivially_destructible_v<ExceptionData>);
static_assert(sizeof(HeapObject) % alignof(ExceptionData) == 0);
static_assert(sizeof(ExceptionData) % alignof(TracePosition) == 0);

auto ExceptionData::trace() const noexcept -> std::span<const TracePosition> {
  const void* storage = std::span(this, 1).last(0).data();
  return {static_cast<const TracePosition*>(storage), trace_count};
}

auto ExceptionData::message() const noexcept -> std::string_view {
  const void* storage = trace().last(0).data();
  return {static_cast<const char*>(storage), message_length};
}

auto ExceptionData::From(HeapObject& object) noexcept -> ExceptionData& {
  void* storage = std::span(&object, 1).last(0).data();
  return *static_cast<ExceptionData*>(storage);
}

auto Heap::AllocateException(HeapObject& payload,
                             ExecutionStatus status,
                             std::span<const TracePosition> trace,
                             std::string_view message) noexcept -> HeapObject* {
  const auto size =
      sizeof(HeapObject) + sizeof(ExceptionData) + trace.size_bytes() + message.size();
  if (size > std::numeric_limits<std::uint32_t>::max()) {
    return nullptr;
  }
  const auto rooted_payload = Value::Object(&payload);
  RootRange roots(*this, &rooted_payload, 1);
  MaybeCollect(size);
  const auto allocation = AllocateRaw(size);
  if (allocation.memory == nullptr) {
    return nullptr;
  }
  auto* object = std::construct_at(static_cast<HeapObject*>(allocation.memory));
  object->kind = ObjectKind::Exception;
  void* storage = std::span(object, 1).last(0).data();
  auto* exception = std::construct_at(static_cast<ExceptionData*>(storage));
  exception->payload = &payload;
  exception->status = status;
  exception->trace_count = static_cast<std::uint32_t>(trace.size());
  exception->message_length = static_cast<std::uint32_t>(message.size());
  std::ranges::uninitialized_copy(trace, TraceStorage(*exception));
  auto bytes = std::span(static_cast<std::byte*>(allocation.memory), allocation.size);
  auto text = bytes.subspan(sizeof(HeapObject) + sizeof(ExceptionData) + trace.size_bytes(),
                            message.size());
  std::ranges::copy(std::as_bytes(std::span(message.data(), message.size())), text.begin());
  Publish(*object, allocation.size);
  return object;
}

}  // namespace vela::runtime
