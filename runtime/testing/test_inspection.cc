#include "runtime/testing/test_inspection.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <sstream>
#include <string>
#include <string_view>

#include "base/execution_status.h"
#include "base/utf8.h"
#include "runtime/heap/heap.h"
#include "runtime/strings/string.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"
#include "runtime/value/value_io.h"

namespace vela::runtime {
namespace {

enum class TestValueKind : std::uint8_t { Scalar, Array, Map, Set, Future };
constexpr std::size_t kDisplayBudget = 256;

TestValueKind KindOf(Value value) {
  if (!value.IsObject()) {
    return TestValueKind::Scalar;
  }
  const auto& object = *value.AsObject();
  switch (Heap::KindOf(object)) {
    case ObjectKind::Array:
      return TestValueKind::Array;
    case ObjectKind::Collection:
      return Heap::CollectionTypeOf(object)->values ? TestValueKind::Map : TestValueKind::Set;
    case ObjectKind::Future:
      return TestValueKind::Future;
    default:
      return TestValueKind::Scalar;
  }
}

std::string Display(Value value) {
  if (value.IsString()) {
    const auto text = value.AsString()->value();
    auto length = std::min(text.size(), kDisplayBudget);
    while (length < text.size() &&
           (static_cast<unsigned char>(text.at(length)) & kContinuationMask) == kContinuationTag) {
      --length;
    }
    return "'" + std::string(text.substr(0, length)) + (text.size() > kDisplayBudget ? "…'" : "'");
  }
  if (value.IsObject()) {
    const auto kind = KindOf(value);
    if (kind == TestValueKind::Array) {
      return "Array(length=" + std::to_string(*Heap::ArrayLength(*value.AsObject())) + ")";
    }
    if (kind == TestValueKind::Map || kind == TestValueKind::Set) {
      return std::string(kind == TestValueKind::Map ? "Map" : "Set") +
             "(size=" + std::to_string(*Heap::CollectionLength(*value.AsObject())) + ")";
    }
  }
  std::ostringstream output;
  WriteValue(output, value);
  return output.str();
}

}  // namespace

std::expected<Value, ExecutionStatus> InspectTestValue(std::span<const Value> arguments,
                                                       Heap& heap) {
  const auto operation = arguments.front();
  if (!operation.IsInteger()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto value = arguments.subspan(1).front();
  if (operation.AsInteger() == 0) {
    return Value::Integer(static_cast<std::int64_t>(KindOf(value)));
  }
  if (operation.AsInteger() != 1) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto text = Display(value);
  auto* string = heap.AllocateString(text);
  if (string == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  return Value::String(string);
}

}  // namespace vela::runtime
