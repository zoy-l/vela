#include "runtime/array_operations.h"

#include <cstdint>
#include <expected>
#include <span>
#include <utility>

#include "base/builtin.h"
#include "runtime/builtins.h"
#include "runtime/heap.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

auto Error(ArrayAccessError error) -> BuiltinError {
  switch (error) {
    case ArrayAccessError::NotArray:
    case ArrayAccessError::ElementTypeMismatch:
      return BuiltinError::TypeError;
    case ArrayAccessError::OutOfBounds:
      return BuiltinError::IndexOutOfBounds;
    case ArrayAccessError::OutOfMemory:
      return BuiltinError::OutOfMemory;
  }
  std::unreachable();
}

}  // namespace

auto ExecuteArrayBuiltin(Builtin builtin, std::span<const Value> arguments, Heap& heap)
    -> std::expected<Value, BuiltinError> {
  const auto receiver = arguments.front();
  if (!receiver.IsObject() || receiver.AsObject() == nullptr) {
    return std::unexpected(BuiltinError::TypeError);
  }
  auto& array = *receiver.AsObject();
  if (builtin == Builtin::ArrayFill || builtin == Builtin::ArrayReverse) {
    const auto result = builtin == Builtin::ArrayFill
                            ? heap.FillArray(array, arguments.subspan(1).front())
                            : Heap::ReverseArray(array);
    if (!result) {
      return std::unexpected(Error(result.error()));
    }
    return Value::Null();
  }
  const auto length = Heap::ArrayLength(array);
  if (!length) {
    return std::unexpected(Error(length.error()));
  }
  std::uint32_t start = 0;
  auto end = *length;
  if (builtin == Builtin::ArraySlice) {
    const auto first = arguments.subspan(1).front();
    const auto last = arguments.subspan(2).front();
    if (!first.IsInteger() || !last.IsInteger()) {
      return std::unexpected(BuiltinError::TypeError);
    }
    const auto begin_index = first.AsInteger();
    const auto end_index = last.AsInteger();
    if (begin_index < 0 || end_index < begin_index || std::cmp_greater(end_index, *length)) {
      return std::unexpected(BuiltinError::IndexOutOfBounds);
    }
    start = static_cast<std::uint32_t>(begin_index);
    end = static_cast<std::uint32_t>(end_index);
  }
  const auto copied = heap.SliceArray(array, start, end);
  if (!copied) {
    return std::unexpected(Error(copied.error()));
  }
  return Value::Object(*copied);
}

}  // namespace vela::runtime
