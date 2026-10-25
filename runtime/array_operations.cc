#include "runtime/array_operations.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <utility>

#include <utf8proc.h>

#include "base/builtin.h"
#include "runtime/builtins.h"
#include "runtime/heap.h"
#include "runtime/string.h"
#include "runtime/value.h"
#include "runtime/value_io.h"

namespace vela::runtime {
namespace {

constexpr std::uint32_t kSupplementaryPlaneStart = 0x10000;
constexpr std::uint32_t kHighSurrogateBase = 0xd800;
constexpr std::uint32_t kLowSurrogateBase = 0xdc00;
constexpr std::uint32_t kSurrogatePayloadBits = 10;
constexpr std::uint32_t kSurrogatePayloadMask = (1U << kSurrogatePayloadBits) - 1;

BuiltinError Error(ArrayAccessError error) {
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

template <class T, class Convert>
std::expected<Value, BuiltinError> Result(std::expected<T, ArrayAccessError> result,
                                          const Convert& convert) {
  if (!result) {
    return std::unexpected(Error(result.error()));
  }
  return convert(*result);
}

std::expected<Value, BuiltinError> MutationResult(std::expected<void, ArrayAccessError> result) {
  if (!result) {
    return std::unexpected(Error(result.error()));
  }
  return Value::Null();
}

std::uint32_t RelativeIndex(Value value, std::uint32_t length) {
  const auto index = value.AsInteger();
  const auto size = static_cast<std::int64_t>(length);
  return static_cast<std::uint32_t>(index < 0 ? std::max(size + index, std::int64_t{0})
                                              : std::min(index, size));
}

std::expected<Value, BuiltinError> CopyArray(HeapObject& array,
                                             std::span<const Value> arguments,
                                             Heap& heap) {
  const auto length = Heap::ArrayLength(array);
  if (!length) {
    return std::unexpected(Error(length.error()));
  }
  const auto first = arguments.subspan(1).front();
  const auto last = arguments.subspan(2).front();
  if (!first.IsInteger() || !last.IsInteger()) {
    return std::unexpected(BuiltinError::TypeError);
  }
  const auto start = RelativeIndex(first, *length);
  const auto end = std::max(start, RelativeIndex(last, *length));
  return Result(heap.SliceArray(array, start, end),
                [](HeapObject* copy) -> Value { return Value::Object(copy); });
}

std::expected<Value, BuiltinError> ReplaceRange(Builtin builtin,
                                                HeapObject& array,
                                                std::span<const Value> arguments,
                                                Heap& heap) {
  const auto length = Heap::ArrayLength(array);
  if (!length) {
    return std::unexpected(Error(length.error()));
  }
  const bool splice = builtin == Builtin::ArraySplice;
  const auto input = arguments.subspan(splice ? 3 : 1).front();
  if (!input.IsObject() || input.AsObject() == nullptr) {
    return std::unexpected(BuiltinError::TypeError);
  }
  std::uint32_t start = builtin == Builtin::ArrayPushMany ? *length : 0;
  std::uint32_t count = 0;
  if (splice) {
    const auto position = arguments.subspan(1).front();
    const auto deleted = arguments.subspan(2).front();
    if (!position.IsInteger() || !deleted.IsInteger()) {
      return std::unexpected(BuiltinError::TypeError);
    }
    start = RelativeIndex(position, *length);
    count = static_cast<std::uint32_t>(
        std::clamp<std::int64_t>(deleted.AsInteger(), 0, *length - start));
  }
  auto* items = input.AsObject();
  auto roots = std::to_array({Value::Null(), Value::Null()});
  RootRange root_range(heap, roots.data(), roots.size());
  // 外部字节码可能复用 receiver；先快照，避免后缀移动覆盖待插入值。
  if (items == &array) {
    const auto copy = heap.SliceArray(array, 0, *length);
    if (!copy) {
      return std::unexpected(Error(copy.error()));
    }
    items = *copy;
    roots.front() = Value::Object(items);
  }
  if (splice) {
    const auto removed = heap.SliceArray(array, start, start + count);
    if (!removed) {
      return std::unexpected(Error(removed.error()));
    }
    roots.back() = Value::Object(*removed);
  }
  const auto changed = heap.ReplaceArrayRange(array, start, count, *items);
  if (!changed) {
    return std::unexpected(Error(changed.error()));
  }
  return splice ? roots.back() : Value::Integer(*Heap::ArrayLength(array));
}

std::string_view SortText(Value value, std::span<char, kScalarTextCapacity> buffer) {
  if (value.IsString()) {
    return value.AsString()->value();
  }
  if (value.IsNull()) {
    return "null";
  }
  return FormatArrayScalar(value, buffer);
}

// 默认排序按 UTF-16 code unit 比较；Vela 字符串内部仍保持 UTF-8。
std::uint32_t NextUnit(std::string_view& text, std::uint32_t& pending) {
  if (pending != 0) {
    return std::exchange(pending, 0);
  }
  utf8proc_int32_t point = 0;
  const void* data = text.data();
  const auto consumed = utf8proc_iterate(static_cast<const utf8proc_uint8_t*>(data),
                                         static_cast<utf8proc_ssize_t>(text.size()), &point);
  text.remove_prefix(static_cast<std::size_t>(consumed));
  const auto scalar = static_cast<std::uint32_t>(point);
  if (scalar < kSupplementaryPlaneStart) {
    return scalar;
  }
  const auto payload = scalar - kSupplementaryPlaneStart;
  pending = kLowSurrogateBase + (payload & kSurrogatePayloadMask);
  return kHighSurrogateBase + (payload >> kSurrogatePayloadBits);
}

std::expected<Value, BuiltinError> DefaultCompare(std::span<const Value> arguments) {
  const auto left = arguments.front();
  const auto right = arguments.subspan(1).front();
  if (left.IsObject() || right.IsObject()) {
    return std::unexpected(BuiltinError::TypeError);
  }
  std::array<char, kScalarTextCapacity> left_buffer{};
  std::array<char, kScalarTextCapacity> right_buffer{};
  auto first = SortText(left, left_buffer);
  auto second = SortText(right, right_buffer);
  std::uint32_t first_pending = 0;
  std::uint32_t second_pending = 0;
  while ((!first.empty() || first_pending != 0) && (!second.empty() || second_pending != 0)) {
    const auto left_unit = NextUnit(first, first_pending);
    const auto right_unit = NextUnit(second, second_pending);
    if (left_unit != right_unit) {
      return Value::Integer(left_unit < right_unit ? -1 : 1);
    }
  }
  const bool first_done = first.empty() && first_pending == 0;
  const bool second_done = second.empty() && second_pending == 0;
  if (first_done == second_done) {
    return Value::Integer(0);
  }
  return Value::Integer(first_done ? -1 : 1);
}

}  // namespace

std::expected<Value, BuiltinError> ExecuteArrayBuiltin(Builtin builtin,
                                                       std::span<const Value> arguments,
                                                       Heap& heap) {
  if (builtin == Builtin::ArrayDefaultCompare) {
    return DefaultCompare(arguments);
  }
  const auto receiver = arguments.front();
  if (!receiver.IsObject() || receiver.AsObject() == nullptr) {
    return std::unexpected(BuiltinError::TypeError);
  }
  auto& array = *receiver.AsObject();
  const auto integer = [](auto value) -> Value {
    return Value::Integer(static_cast<std::int64_t>(value));
  };
  const auto identity = [](Value value) -> Value { return value; };
  switch (builtin) {
    case Builtin::ArraySlice:
      return CopyArray(array, arguments, heap);
    case Builtin::ArrayFill: {
      const auto length = Heap::ArrayLength(array);
      const auto first = arguments.subspan(2).front();
      const auto last = arguments.subspan(3).front();
      if (!length || !first.IsInteger() || !last.IsInteger()) {
        return std::unexpected(BuiltinError::TypeError);
      }
      const auto start = RelativeIndex(first, *length);
      const auto end = std::max(start, RelativeIndex(last, *length));
      const auto filled = heap.FillArray(array, arguments.subspan(1).front(), start, end);
      if (!filled) {
        return std::unexpected(Error(filled.error()));
      }
      return receiver;
    }
    case Builtin::ArrayReverse: {
      const auto reversed = Heap::ReverseArray(array);
      if (!reversed) {
        return std::unexpected(Error(reversed.error()));
      }
      return receiver;
    }
    case Builtin::ArrayCapacity:
      return Result(Heap::ArrayCapacity(array), integer);
    case Builtin::ArrayPush:
      return Result(heap.PushArray(array, arguments.subspan(1).front()), integer);
    case Builtin::ArrayPop:
      return Result(Heap::PopArray(array), identity);
    case Builtin::ArrayShift: {
      const auto length = Heap::ArrayLength(array);
      if (!length) {
        return std::unexpected(Error(length.error()));
      }
      if (*length == 0) {
        return Value::Null();
      }
      return Result(Heap::RemoveArrayElement(array, 0), identity);
    }
    case Builtin::ArrayClear:
      return MutationResult(Heap::ClearArray(array));
    case Builtin::ArrayReserve: {
      const auto capacity = arguments.subspan(1).front();
      if (!capacity.IsInteger()) {
        return std::unexpected(BuiltinError::TypeError);
      }
      if (capacity.AsInteger() < 0) {
        return std::unexpected(BuiltinError::IndexOutOfBounds);
      }
      return MutationResult(
          heap.ReserveArray(array, static_cast<std::size_t>(capacity.AsInteger())));
    }
    case Builtin::ArrayPushMany:
    case Builtin::ArrayUnshift:
    case Builtin::ArraySplice:
      return ReplaceRange(builtin, array, arguments, heap);
    case Builtin::ArrayMutation:
      return Result(Heap::ArrayMutation(array), [](std::uint64_t mutation) -> Value {
        return Value::Integer(std::bit_cast<std::int64_t>(mutation));
      });
    case Builtin::ArrayCheckMutation: {
      const auto mutation = Heap::ArrayMutation(array);
      const auto expected = arguments.subspan(1).front();
      if (!mutation || !expected.IsInteger()) {
        return std::unexpected(BuiltinError::TypeError);
      }
      if (*mutation != std::bit_cast<std::uint64_t>(expected.AsInteger())) {
        return std::unexpected(BuiltinError::ConcurrentModification);
      }
      return Value::Null();
    }
    default:
      std::unreachable();
  }
}

}  // namespace vela::runtime
