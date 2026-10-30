#include "runtime/exception.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

#include "base/execution_limits.h"
#include "base/execution_status.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/object.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

std::span<TracePosition> TraceStorage(ExceptionData& exception) {
  return {StorageAfter<TracePosition>(exception), exception.trace_count};
}

}  // namespace

std::expected<Value, ExecutionStatus> CaptureTraceValue(Heap& heap,
                                                        std::span<const TracePosition> positions) {
  auto* object = heap.AllocateByteArray(positions.size_bytes());
  if (object == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  const auto bytes = Heap::ByteArrayBytes(*object);
  std::memcpy(bytes.data(), positions.data(), positions.size_bytes());
  return Value::Object(object);
}

std::expected<std::vector<TracePosition>, ExecutionStatus> ReadTraceValue(Value value) {
  if (!value.IsObject() || !Heap::IsByteArray(*value.AsObject())) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto bytes = Heap::ByteArrayBytes(*value.AsObject());
  if (bytes.size() % sizeof(TracePosition) != 0 ||
      bytes.size() / sizeof(TracePosition) > kMaxCallFrames) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  std::vector<TracePosition> positions(bytes.size() / sizeof(TracePosition));
  std::memcpy(positions.data(), bytes.data(), bytes.size());
  return positions;
}

static_assert(std::is_standard_layout_v<ExceptionData>);
static_assert(std::is_trivially_destructible_v<ExceptionData>);
static_assert(sizeof(HeapObject) % alignof(ExceptionData) == 0);
static_assert(sizeof(ExceptionData) % alignof(TracePosition) == 0);

std::span<const TracePosition> ExceptionData::trace() const noexcept {
  return {StorageAfter<TracePosition>(*this), trace_count};
}

std::string_view ExceptionData::message() const noexcept {
  const void* storage = trace().last(0).data();
  return {static_cast<const char*>(storage), message_length};
}

ExceptionData& ExceptionData::From(HeapObject& object) noexcept {
  return *StorageAfter<ExceptionData>(object);
}

HeapObject* Heap::AllocateException(HeapObject& payload,
                                    ExecutionStatus status,
                                    std::span<const TracePosition> trace,
                                    std::string_view message) noexcept {
  const auto size =
      sizeof(HeapObject) + sizeof(ExceptionData) + trace.size_bytes() + message.size();
  if (size > std::numeric_limits<std::uint32_t>::max()) {
    return nullptr;
  }
  const auto rooted_payload = Value::Object(&payload);
  RootRange roots(*this, &rooted_payload, 1, RootOwner::Runtime);
  return AllocateObject(
      ObjectKind::Exception, size, [this, &payload, status, trace, message](HeapObject& object) {
        auto* exception = std::construct_at(StorageAfter<ExceptionData>(object));
        exception->payload = &payload;
        exception->status = status;
        auto& scheduler = isolate_->scheduler();
        exception->origin = {
            .context = scheduler.context(),
            .identity = scheduler.NewErrorIdentity(),
            .captured = true,
        };
        exception->trace_count = static_cast<std::uint32_t>(trace.size());
        exception->message_length = static_cast<std::uint32_t>(message.size());
        std::ranges::uninitialized_copy(trace, TraceStorage(*exception));
        auto bytes = std::span(StorageAfter<char>(*exception), trace.size_bytes() + message.size());
        std::ranges::copy(message, bytes.subspan(trace.size_bytes()).begin());
      });
}

}  // namespace vela::runtime
