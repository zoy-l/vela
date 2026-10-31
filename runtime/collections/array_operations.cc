#include "runtime/collections/array_operations.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>

#include "base/builtin.h"
#include "base/execution_status.h"
#include "base/utf8.h"
#include "runtime/collections/index_range.h"
#include "runtime/heap/heap.h"
#include "runtime/strings/string.h"
#include "runtime/value/value.h"
#include "runtime/value/value_io.h"

namespace vela::runtime {
namespace {

constexpr std::uint32_t kSurrogatePayloadMask = (1U << kSurrogatePayloadBits) - 1;

ExecutionStatus Error(ArrayAccessError error) {
  switch (error) {
    case ArrayAccessError::NotArray:
    case ArrayAccessError::ElementTypeMismatch:
      return ExecutionStatus::TypeError;
    case ArrayAccessError::OutOfBounds:
      return ExecutionStatus::IndexOutOfBounds;
    case ArrayAccessError::OutOfMemory:
      return ExecutionStatus::OutOfMemory;
  }
  std::unreachable();
}

template <class T, class Convert>
std::expected<Value, ExecutionStatus> Result(std::expected<T, ArrayAccessError> result,
                                             const Convert& convert) {
  if (!result) {
    return std::unexpected(Error(result.error()));
  }
  return convert(*result);
}

std::expected<Value, ExecutionStatus> MutationResult(std::expected<void, ArrayAccessError> result) {
  if (!result) {
    return std::unexpected(Error(result.error()));
  }
  return Value::Null();
}

std::expected<Value, ExecutionStatus> CopyArray(HeapObject& array,
                                                std::span<const Value> arguments,
                                                Heap& heap) {
  const auto length = Heap::ArrayLength(array);
  if (!length) {
    return std::unexpected(Error(length.error()));
  }
  const auto first = arguments.subspan(1).front();
  const auto last = arguments.subspan(2).front();
  if (!first.IsInteger() || !last.IsInteger()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto [start, end] = ClampIndexRange(first.AsInteger(), last.AsInteger(), *length);
  return Result(heap.SliceArray(array, start, end),
                [](HeapObject* copy) -> Value { return Value::Object(copy); });
}

std::expected<Value, ExecutionStatus> ReplaceRange(Builtin builtin,
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
    return std::unexpected(ExecutionStatus::TypeError);
  }
  std::uint32_t start = builtin == Builtin::ArrayPushMany ? *length : 0;
  std::uint32_t count = 0;
  if (splice) {
    const auto position = arguments.subspan(1).front();
    const auto deleted = arguments.subspan(2).front();
    if (!position.IsInteger() || !deleted.IsInteger()) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    start = RelativeIndex(position.AsInteger(), *length);
    count = static_cast<std::uint32_t>(
        std::clamp<std::int64_t>(deleted.AsInteger(), 0, *length - start));
  }
  auto* items = input.AsObject();
  auto roots = std::to_array({Value::Null(), Value::Null()});
  RootRange root_range(heap, roots.data(), roots.size(), RootOwner::Runtime);
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

// 默认排序按 UTF-16 code unit 比较；Vela 字符串内部仍保持 UTF-8。
std::uint32_t NextUnit(std::string_view& text, std::uint32_t& pending) {
  if (pending != 0) {
    return std::exchange(pending, 0);
  }
  const auto decoded = DecodeUtf8Prefix(text);
  text.remove_prefix(decoded.width);
  const auto scalar = decoded.value;
  if (scalar < kSupplementaryFirst) {
    return scalar;
  }
  const auto payload = scalar - kSupplementaryFirst;
  pending = kLowSurrogateFirst + (payload & kSurrogatePayloadMask);
  return kHighSurrogateFirst + (payload >> kSurrogatePayloadBits);
}

Value CompareText(std::string_view first, std::string_view second) {
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

std::expected<Value, ExecutionStatus> SortByDefault(HeapObject& array, Heap& heap) {
  const auto* layout = Heap::ArrayTypeOf(array);
  if (layout == nullptr) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto& type = layout->element;
  if (type.nominal != nullptr ||
      (!type.dynamic && type.kind != ValueKind::Integer && type.kind != ValueKind::Double &&
       type.kind != ValueKind::Boolean && type.kind != ValueKind::String)) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto count = static_cast<std::size_t>(*Heap::ArrayLength(array));
  if (count == 0) {
    return Value::Object(&array);
  }
  struct Entry final {
    Value value;
    std::uint32_t position = 0;
    std::array<char, kScalarTextCapacity> buffer{};
    std::size_t length = 0;

    [[nodiscard]] std::string_view text() const {
      return value.IsString() ? value.AsString()->value() : std::string_view(buffer.data(), length);
    }
  };
  if (count > std::numeric_limits<std::size_t>::max() / sizeof(Entry)) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  const auto release = [&heap](Entry* entries) { heap.FreeNative(entries); };
  const std::unique_ptr<Entry, decltype(release)> storage(
      static_cast<Entry*>(heap.AllocateNative(count * sizeof(Entry))), release);
  if (storage == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  const auto entries = std::span(storage.get(), count);
  // 格式化仅执行一次；排序只移动值及其文本，不分配 GC 对象，原数组保根所有引用。
  for (std::uint32_t index = 0; index < count; ++index) {
    const auto value = *Heap::GetElement(array, index);
    if (value.IsObject()) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    auto& entry = *std::construct_at(&entries.subspan(index).front());
    entry.value = value;
    entry.position = index;
    if (!value.IsString()) {
      const auto text = FormatArrayValue(value, entry.buffer);
      if (text.data() != entry.buffer.data()) {
        std::ranges::copy(text, entry.buffer.begin());
      }
      entry.length = text.size();
    }
  }
  std::ranges::sort(entries, [](const Entry& left, const Entry& right) {
    const auto comparison = CompareText(left.text(), right.text()).AsInteger();
    return comparison == 0 ? left.position < right.position : comparison < 0;
  });
  for (std::uint32_t index = 0; index < count; ++index) {
    std::ignore = heap.SetElement(array, index, entries.subspan(index).front().value);
  }
  return Value::Object(&array);
}

}  // namespace

std::expected<Value, ExecutionStatus> ExecuteArrayBuiltin(Builtin builtin,
                                                          std::span<const Value> arguments,
                                                          Heap& heap) {
  const auto receiver = arguments.front();
  if (!receiver.IsObject() || receiver.AsObject() == nullptr) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  auto& array = *receiver.AsObject();
  const auto integer = [](auto value) -> Value {
    return Value::Integer(static_cast<std::int64_t>(value));
  };
  const auto identity = [](Value value) -> Value { return value; };
  switch (builtin) {
    case Builtin::ArraySortDefault:
      return SortByDefault(array, heap);
    case Builtin::ArrayLength:
      return Result(Heap::ArrayLength(array), integer);
    case Builtin::ArraySlice:
      return CopyArray(array, arguments, heap);
    case Builtin::ArrayFill: {
      const auto length = Heap::ArrayLength(array);
      const auto first = arguments.subspan(2).front();
      const auto last = arguments.subspan(3).front();
      if (!length || !first.IsInteger() || !last.IsInteger()) {
        return std::unexpected(ExecutionStatus::TypeError);
      }
      const auto [start, end] = ClampIndexRange(first.AsInteger(), last.AsInteger(), *length);
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
        return std::unexpected(ExecutionStatus::TypeError);
      }
      if (capacity.AsInteger() < 0) {
        return std::unexpected(ExecutionStatus::IndexOutOfBounds);
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
        return std::unexpected(ExecutionStatus::TypeError);
      }
      if (*mutation != std::bit_cast<std::uint64_t>(expected.AsInteger())) {
        return std::unexpected(ExecutionStatus::ConcurrentModification);
      }
      return Value::Null();
    }
    default:
      std::unreachable();
  }
}

}  // namespace vela::runtime
