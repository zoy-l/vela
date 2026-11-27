#include "runtime/collections/array_operations.h"
#include <optional>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "base/builtin.h"
#include "base/execution_status.h"
#include "runtime/collections/index_range.h"
#include "runtime/heap/array_storage.h"
#include "runtime/heap/element_storage.h"
#include "runtime/heap/heap.h"
#include "runtime/heap/native_buffer.h"
#include "runtime/strings/string.h"
#include "runtime/value/value.h"
#include "runtime/value/value_io.h"
#include "runtime/value/value_operations.h"

namespace vela::runtime {

namespace {

template <class Values, class Projection>
bool IncludesObjects(const Values& values, const HeapObject& target, Projection project) {
  const auto* layout = Heap::ClassOf(target);
  if (layout == nullptr || (!layout->HasReflectionIdentity() && !layout->IsEnum())) {
    return std::ranges::find(values, &target, project) != values.end();
  }
  return std::ranges::any_of(values, [&](const auto& element) {
    const auto* value = project(element);
    return value != nullptr && IsEqualObject(*value, target);
  });
}

template <class Predicate>
bool IncludesTagged(std::span<const Value> values, ValueKind kind, Predicate matches) {
  return std::ranges::any_of(values,
                             [=](Value value) { return value.kind() == kind && matches(value); });
}

bool IncludesValues(std::span<const Value> values, Value searched) {
  const auto kind = searched.kind();
  switch (kind) {
    case ValueKind::Null:
      return std::ranges::any_of(values, [](Value value) { return value.IsNull(); });
    case ValueKind::Boolean: {
      const auto target = searched.AsBoolean();
      return IncludesTagged(values, kind,
                            [target](Value value) { return value.AsBoolean() == target; });
    }
    case ValueKind::Integer: {
      const auto target = searched.AsInteger();
      return IncludesTagged(values, kind,
                            [target](Value value) { return value.AsInteger() == target; });
    }
    case ValueKind::Double: {
      const auto target = searched.AsDouble();
      return std::isnan(target)
                 ? IncludesTagged(values, kind,
                                  [](Value value) { return std::isnan(value.AsDouble()); })
                 : IncludesTagged(values, kind,
                                  [target](Value value) { return value.AsDouble() == target; });
    }
    case ValueKind::String: {
      const auto* target = searched.AsString();
      return IncludesTagged(values, kind, [target](Value value) {
        return IsEqualString(*value.AsString(), *target);
      });
    }
    case ValueKind::Object:
      return IncludesObjects(values, *searched.AsObject(),
                             [](Value value) { return value.AsObjectOrNull(); });
  }
  std::unreachable();
}

template <class T>
bool IncludesElements(std::span<const T> values, Value searched) {
  if constexpr (std::is_same_v<T, Value>) {
    return IncludesValues(values, searched);
  } else if constexpr (std::is_same_v<T, double>) {
    const auto number = searched.AsDouble();
    return std::isnan(number)
               ? std::ranges::any_of(values, [](double value) { return std::isnan(value); })
               : std::ranges::find(values, number) != values.end();
  } else if constexpr (std::is_pointer_v<T>) {
    const auto* target = ElementValue<T>(searched);
    if (target == nullptr) {
      return std::ranges::find(values, nullptr) != values.end();
    }
    if constexpr (std::is_same_v<T, HeapObject*>) {
      return IncludesObjects(values, *target, std::identity{});
    } else {
      return std::ranges::any_of(values, [target](const auto* value) {
        return value != nullptr && IsEqualString(*value, *target);
      });
    }
  } else {
    return std::ranges::find(values, ElementValue<T>(searched)) != values.end();
  }
}

std::expected<Value, ExecutionStatus> ArrayIncludes(const HeapObject& array,
                                                    std::span<const Value> arguments) {
  const auto* layout = Heap::ArrayTypeOf(array);
  const auto searched = arguments[1];
  const auto from = arguments[2];
  if (layout == nullptr || !from.IsInteger()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto length = *Heap::ArrayLength(array);
  if (!Heap::IsValueOfType(searched, layout->element)) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto start = RelativeIndex(from.AsInteger(), length);
  const auto bytes = internal::ElementBytes(array, *layout, length);
  if (layout->value_layout != nullptr) {
    return Value::Boolean(IncludesInlineElement(*layout, bytes, start, searched));
  }
  // 无用户回调、分配或 GC；只在入口选择一次存储表示，不逐元素装箱或检查边界。
  const auto found =
      VisitElementStorage(*layout, bytes, [searched, start]<class T>(std::span<const T> elements) {
        return IncludesElements(elements.subspan(start), searched);
      });
  return Value::Boolean(found);
}

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

std::expected<Value, ExecutionStatus> SortIntegers(Builtin builtin, HeapObject& array) {
  const auto borrowed = internal::BorrowIntegerElements(array);
  if (!borrowed) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  // 内部借用不逸出，排序只置换已有值；保留窄域事实，也不能重建已被宿主视图撤销的事实。
  const auto values = *borrowed;
  // 此入口只接收已证明无副作用、无失败的整数全序比较器。相等的 int 没有独立身份，
  // 原地重排与稳定排序等价，也无需比较器快照或临时托管数组。
  if (builtin == Builtin::ArraySortIntAscending) {
    std::ranges::sort(values);
  } else {
    std::ranges::sort(values, std::greater<>());
  }
  return Value::Object(&array);
}

std::expected<Value, ExecutionStatus> SortByDefault(HeapObject& array, Heap& heap) {
  const auto* layout = Heap::ArrayTypeOf(array);
  if (layout == nullptr) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto& type = layout->element;
  if (type.nominal != nullptr ||
      (!type.IsDynamic() && type.kind != ValueKind::Integer && type.kind != ValueKind::Double &&
       type.kind != ValueKind::Boolean && type.kind != ValueKind::String)) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto count = static_cast<std::size_t>(*Heap::ArrayLength(array));
  if (count == 0) {
    return Value::Object(&array);
  }
  struct Entry final {
    const char* data = nullptr;
    std::uint32_t position = 0;
    std::uint32_t length = 0;

    [[nodiscard]] std::string_view text() const { return {data, length}; }
  };
  NativeBuffer<Entry> storage(heap, count);
  if (!storage.IsAllocated()) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  const auto entries = storage.values();
  NativeBuffer<char> characters(heap, count * kScalarTextCapacity);
  if (!characters.IsAllocated()) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  const auto text = characters.values();
  std::array<char, kScalarTextCapacity> buffer{};
  std::size_t text_bytes = 0;
  for (std::uint32_t index = 0; index < count; ++index) {
    const auto value = *heap.GetElement(array, index);
    if (value.IsObject()) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    auto& entry = *std::construct_at(&entries[index]);
    entry.position = index;
    const auto formatted = FormatArrayValue(value, buffer);
    entry.length = static_cast<std::uint32_t>(formatted.size());
    if (value.IsString()) {
      entry.data = formatted.data();
    } else {
      const auto output = text.subspan(text_bytes, formatted.size());
      std::ranges::copy(formatted, output.begin());
      entry.data = output.data();
      text_bytes += formatted.size();
    }
  }
  // 文本连续打包，排序只移动固定大小的条目；原数组保根所有引用。
  std::ranges::sort(entries, [](const Entry& left, const Entry& right) {
    const auto comparison = CompareText(left.text(), right.text());
    return comparison == 0 ? left.position < right.position : comparison < 0;
  });
  // 按置换环写回，不分配或执行用户代码；临时保存的值在环闭合前不会遇到 GC。
  for (std::uint32_t index = 0; index < count; ++index) {
    if (entries[index].position == index) {
      continue;
    }
    const auto saved = *heap.GetElement(array, index);
    auto destination = index;
    while (true) {
      const auto source = entries[destination].position;
      entries[destination].position = destination;
      if (source == index) {
        std::ignore = heap.SetElement(array, destination, saved);
        break;
      }
      std::ignore = heap.SetElement(array, destination, *heap.GetElement(array, source));
      destination = source;
    }
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
    case Builtin::ArrayIncludes:
      return ArrayIncludes(array, arguments);
    case Builtin::ArraySortDefault:
      return SortByDefault(array, heap);
    case Builtin::ArraySortIntAscending:
    case Builtin::ArraySortIntDescending:
      return SortIntegers(builtin, array);
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
      return heap.PopArray(array).transform_error(ArrayExecutionStatus);
    case Builtin::ArrayShift:
      return heap.ShiftArray(array).transform_error(ArrayExecutionStatus);
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
