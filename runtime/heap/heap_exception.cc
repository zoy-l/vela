#include "runtime/exceptions/exception.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <string_view>

#include "base/execution_status.h"
#include "runtime/heap/heap.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {

static_assert(sizeof(HeapObject) % alignof(ExceptionData) == 0);

HeapObject* Heap::AllocateException(HeapObject& payload,
                                    ExecutionStatus status,
                                    std::string_view message) noexcept {
  const auto size = sizeof(HeapObject) + sizeof(ExceptionData) + message.size();
  if (size > std::numeric_limits<std::uint32_t>::max()) {
    return nullptr;
  }
  const auto rooted_payload = Value::Object(&payload);
  RootRange roots(*this, &rooted_payload, 1, RootOwner::Runtime);
  return AllocateObject(ObjectKind::Exception, size,
                        [&payload, status, message](HeapObject& object) {
                          auto* exception = std::construct_at(StorageAfter<ExceptionData>(object));
                          exception->payload = &payload;
                          exception->status = status;
                          exception->message_length = static_cast<std::uint32_t>(message.size());
                          std::ranges::copy(message, StorageAfter<char>(*exception));
                        });
}

}  // namespace vela::runtime
