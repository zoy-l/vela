#include "runtime/collections/array_operations.h"
#include <optional>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>

#include "base/builtin.h"
#include "base/execution_status.h"
#include "runtime/collections/index_range.h"
#include "runtime/heap/heap.h"
#include "runtime/heap/native_buffer.h"
#include "runtime/strings/string.h"
#include "runtime/value/value.h"
#include "runtime/value/value_io.h"

namespace vela::runtime {

ExecutionStatus ArrayExecutionStatus(ArrayAccessError error) {
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

namespace {

std::expected<Value, ExecutionStatus> CopyArray(HeapObject& array,
                                                std::span<const Value> arguments,
                                                Heap& heap) {
  const auto length = Heap::ArrayLength(array);
  if (!length) {
    return std::unexpected(ArrayExecutionStatus(length.error()));
  }
  const auto first = arguments[1];
  const auto last = arguments[2];
  if (!first.IsInteger() || !last.IsInteger()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto [start, end] = ClampIndexRange(first.AsInteger(), last.AsInteger(), *length);
  return heap.SliceArray(array, start, end)
      .transform([](HeapObject* copy) { return Value::Object(copy); })
      .transform_error(ArrayExecutionStatus);
}

std::expected<Value, ExecutionStatus> ReplaceRange(Builtin builtin,
                                                   HeapObject& array,
                                                   std::span<const Value> arguments,
                                                   Heap& heap) {
  const auto length = Heap::ArrayLength(array);
  if (!length) {
    return std::unexpected(ArrayExecutionStatus(length.error()));
  }
  const bool splice = builtin == Builtin::ArraySplice;
  const auto input = arguments[splice ? 3 : 1];
  auto* items = input.AsObjectOrNull();
  if (items == nullptr) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  std::uint32_t start = builtin == Builtin::ArrayPushMany ? *length : 0;
  std::uint32_t count = 0;
  if (splice) {
    const auto position = arguments[1];
    const auto deleted = arguments[2];
    if (!position.IsInteger() || !deleted.IsInteger()) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    start = RelativeIndex(position.AsInteger(), *length);
    count = static_cast<std::uint32_t>(
        std::clamp<std::int64_t>(deleted.AsInteger(), 0, *length - start));
  }
  auto roots = std::to_array({Value::Null(), Value::Null()});
  RootRange root_range(std::nullopt);
  if (splice || items == &array) {
    root_range.Attach(heap, roots.data(), roots.size());
  }
  // 输入可能复用 receiver；先快照，避免后缀移动覆盖待插入值。
  if (items == &array) {
    const auto copy = heap.SliceArray(array, 0, *length);
    if (!copy) {
      return std::unexpected(ArrayExecutionStatus(copy.error()));
    }
    items = *copy;
    roots.front() = Value::Object(items);
  }
  if (splice) {
    const auto removed = heap.SliceArray(array, start, start + count);
    if (!removed) {
      return std::unexpected(ArrayExecutionStatus(removed.error()));
    }
    roots.back() = Value::Object(*removed);
  }
  const auto changed = heap.ReplaceArrayRange(array, start, count, *items);
  if (!changed) {
    return std::unexpected(ArrayExecutionStatus(changed.error()));
  }
  return splice ? roots.back() : Value::Integer(*Heap::ArrayLength(array));
}

int CompareText(std::string_view first, std::string_view second) {
  const auto [first_difference, second_difference] = std::ranges::mismatch(first, second);
  if (first_difference == first.end() || second_difference == second.end()) {
    return static_cast<int>(first.size() > second.size()) -
           static_cast<int>(first.size() < second.size());
  }
  const auto left = static_cast<std::uint8_t>(*first_difference);
  const auto right = static_cast<std::uint8_t>(*second_difference);
  const auto order = left < right ? -1 : 1;
  // 合法 UTF-8 与 UTF-16 只在增补字符和 U+E000..U+FFFF 的相对次序上不同。
  constexpr std::uint8_t kSupplementaryLead = 0xF0;
  constexpr std::uint8_t kBmpTailLead = 0xEE;
  const auto reversed =
      (left >= kSupplementaryLead && right >= kBmpTailLead && right < kSupplementaryLead) ||
      (right >= kSupplementaryLead && left >= kBmpTailLead && left < kSupplementaryLead);
  return reversed ? -order : order;
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
  NativeBuffer<Entry> storage(heap, count);
  if (!storage.IsAllocated()) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  const auto entries = storage.values();
  // 格式化仅执行一次；排序只移动值及其文本，不分配 GC 对象，原数组保根所有引用。
  for (std::uint32_t index = 0; index < count; ++index) {
    const auto value = *Heap::GetElement(array, index);
    if (value.IsObject()) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    auto& entry = *std::construct_at(&entries[index]);
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
    const auto comparison = CompareText(left.text(), right.text());
    return comparison == 0 ? left.position < right.position : comparison < 0;
  });
  for (std::uint32_t index = 0; index < count; ++index) {
    std::ignore = heap.SetElement(array, index, entries[index].value);
  }
  return Value::Object(&array);
}

}  // namespace

std::expected<Value, ExecutionStatus> ExecuteArrayBuiltin(Builtin builtin,
                                                          std::span<const Value> arguments,
                                                          Heap& heap) {
  const auto receiver = arguments.front();
  auto* receiver_object = receiver.AsObjectOrNull();
  if (receiver_object == nullptr) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  auto& array = *receiver_object;
  const auto integer = [](auto value) -> Value {
    return Value::Integer(static_cast<std::int64_t>(value));
  };
  switch (builtin) {
    case Builtin::ArraySortDefault:
      return SortByDefault(array, heap);
    case Builtin::ArrayLength:
      return Heap::ArrayLength(array).transform(integer).transform_error(ArrayExecutionStatus);
    case Builtin::ArraySlice:
      return CopyArray(array, arguments, heap);
    case Builtin::ArrayFill: {
      const auto length = Heap::ArrayLength(array);
      const auto first = arguments[2];
      const auto last = arguments[3];
      if (!length || !first.IsInteger() || !last.IsInteger()) {
        return std::unexpected(ExecutionStatus::TypeError);
      }
      const auto [start, end] = ClampIndexRange(first.AsInteger(), last.AsInteger(), *length);
      const auto filled = heap.FillArray(array, arguments[1], start, end);
      if (!filled) {
        return std::unexpected(ArrayExecutionStatus(filled.error()));
      }
      return receiver;
    }
    case Builtin::ArrayReverse: {
      const auto reversed = Heap::ReverseArray(array);
      if (!reversed) {
        return std::unexpected(ArrayExecutionStatus(reversed.error()));
      }
      return receiver;
    }
    case Builtin::ArrayCapacity:
      return Heap::ArrayCapacity(array).transform(integer).transform_error(ArrayExecutionStatus);
    case Builtin::ArrayPush:
      return heap.PushArray(array, arguments[1])
          .transform(integer)
          .transform_error(ArrayExecutionStatus);
    case Builtin::ArrayPop:
      return Heap::PopArray(array).transform_error(ArrayExecutionStatus);
    case Builtin::ArrayShift:
      return Heap::ShiftArray(array).transform_error(ArrayExecutionStatus);
    case Builtin::ArrayClear:
      return Heap::ClearArray(array)
          .transform([] { return Value::Null(); })
          .transform_error(ArrayExecutionStatus);
    case Builtin::ArrayReserve: {
      const auto capacity = arguments[1];
      if (!capacity.IsInteger()) {
        return std::unexpected(ExecutionStatus::TypeError);
      }
      if (capacity.AsInteger() < 0) {
        return std::unexpected(ExecutionStatus::IndexOutOfBounds);
      }
      return heap.ReserveArray(array, static_cast<std::size_t>(capacity.AsInteger()))
          .transform([] { return Value::Null(); })
          .transform_error(ArrayExecutionStatus);
    }
    case Builtin::ArrayPushMany:
    case Builtin::ArrayUnshift:
    case Builtin::ArraySplice:
      return ReplaceRange(builtin, array, arguments, heap);
    case Builtin::ArrayMutation:
      return Heap::ArrayMutation(array)
          .transform([](std::uint64_t mutation) {
            return Value::Integer(std::bit_cast<std::int64_t>(mutation));
          })
          .transform_error(ArrayExecutionStatus);
    case Builtin::ArrayCheckMutation: {
      const auto mutation = Heap::ArrayMutation(array);
      const auto expected = arguments[1];
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
