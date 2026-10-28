#include "runtime/error.h"

#include <array>
#include <expected>
#include <string_view>

#include "base/execution_status.h"
#include "runtime/heap.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace vela::runtime {

std::string_view ErrorMessage(Value value, const ClassLayout& layout) noexcept {
  if (!Heap::IsValueOfType(value, {.kind = ValueKind::Object, .instance = &layout})) {
    return {};
  }
  const auto* field = Heap::GetField(*value.AsObject(), kErrorMessageField);
  return field != nullptr && field->IsString() ? field->AsString()->value() : std::string_view();
}

std::expected<Value, ExecutionStatus> CreateError(Heap& heap,
                                                  const ClassLayout& layout,
                                                  ErrorFields fields) {
  std::array<Value, kErrorFieldCount> values;
  RootRange roots(heap, values.data(), values.size());
  auto* text = heap.AllocateString(fields.message);
  if (text == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  values.at(kErrorMessageField) = Value::String(text);
  auto* identifier = heap.AllocateString(fields.code);
  if (identifier == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  values.at(kErrorCodeField) = Value::String(identifier);
  auto* object = heap.AllocateInstance(layout, values);
  return object != nullptr ? std::expected<Value, ExecutionStatus>(Value::Object(object))
                           : std::unexpected(ExecutionStatus::OutOfMemory);
}

}  // namespace vela::runtime
