#include "runtime/strings/unicode_operations.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include <utf8proc.h>

#include "base/builtin.h"
#include "base/execution_status.h"
#include "base/utf8.h"
#include "runtime/heap/array_storage.h"
#include "runtime/heap/element_storage.h"
#include "runtime/heap/heap.h"
#include "runtime/strings/index_buffer.h"
#include "runtime/strings/string.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::runtime {
namespace {

using Result = std::expected<Value, ExecutionStatus>;
enum class GraphemeStep : std::uint8_t { Continue, Stop };
using GraphemeResult = std::expected<GraphemeStep, ExecutionStatus>;

std::size_t Decode(std::string_view text, utf8proc_int32_t& point) {
  // 字符串在输入边界已验证；这里读取非空 UTF-8 后缀，不重复做全串验证。
  const auto decoded = DecodeUtf8Prefix(text);
  point = static_cast<utf8proc_int32_t>(decoded.value);
  return decoded.width;
}

template <typename Visitor>
std::expected<std::size_t, ExecutionStatus> WalkGraphemes(std::string_view text,
                                                          const Visitor& visitor) {
  std::size_t offset = 0;
  std::size_t begin = 0;
  std::size_t count = 0;
  utf8proc_int32_t previous = 0;
  utf8proc_int32_t state = 0;
  while (offset < text.size()) {
    utf8proc_int32_t point = 0;
    const auto size = Decode(text.substr(offset), point);
    if (offset != 0 && utf8proc_grapheme_break_stateful(previous, point, &state)) {
      const auto delivered = visitor(count++, begin, offset);
      if (!delivered) {
        return std::unexpected(delivered.error());
      }
      if (*delivered == GraphemeStep::Stop) {
        return count;
      }
      begin = offset;
    }
    previous = point;
    offset += size;
  }
  if (!text.empty()) {
    const auto delivered = visitor(count++, begin, text.size());
    if (!delivered) {
      return std::unexpected(delivered.error());
    }
  }
  return count;
}

Result FillCodePoints(std::string_view text, HeapObject& array) {
  const auto numbers = Heap::IntegerElements(array);
  if (!numbers || Utf8Length(text) != numbers->size()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  std::size_t offset = 0;
  std::size_t index = 0;
  while (offset < text.size()) {
    utf8proc_int32_t point = 0;
    offset += Decode(text.substr(offset), point);
    numbers->subspan(index++).front() = point;
  }
  return Value::Object(&array);
}

Result GraphemeSlice(std::string_view text, std::span<const Value> arguments, Heap& heap) {
  const auto start_value = arguments.subspan(1).front();
  const auto end_value = arguments.subspan(2).front();
  if (!start_value.IsInteger() || !end_value.IsInteger()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto start = start_value.AsInteger();
  const auto end = end_value.AsInteger();
  if (start < 0 || end < start) {
    return std::unexpected(ExecutionStatus::IndexOutOfBounds);
  }
  std::optional<std::size_t> first = start == 0 ? std::optional<std::size_t>(0) : std::nullopt;
  std::optional<std::size_t> last = end == 0 ? std::optional<std::size_t>(0) : std::nullopt;
  if (end != 0) {
    const auto count = WalkGraphemes(
        text, [&](std::size_t index, std::size_t /*begin*/, std::size_t finish) -> GraphemeResult {
          const auto boundary = index + 1;
          if (std::cmp_equal(boundary, start)) {
            first = finish;
          }
          if (std::cmp_equal(boundary, end)) {
            last = finish;
          }
          return last ? GraphemeStep::Stop : GraphemeStep::Continue;
        });
    if (!count) {
      return std::unexpected(count.error());
    }
  }
  if (!first || !last) {
    return std::unexpected(ExecutionStatus::IndexOutOfBounds);
  }
  auto* result = heap.AllocateString(text.substr(*first, *last - *first));
  return result == nullptr ? Result(std::unexpected(ExecutionStatus::OutOfMemory))
                           : Result(Value::String(result));
}

}  // namespace

std::expected<void, ExecutionStatus> Heap::FillGraphemes(HeapObject& array, std::string_view text) {
  const auto strings = StringElements(array);
  if (!strings) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  IndexBuffer storage(strings->size());
  const auto allocated = storage.values();
  if (!allocated) {
    return std::unexpected(allocated.error());
  }
  const auto boundaries = *allocated;
  const auto count =
      WalkGraphemes(text, [&](std::size_t index, std::size_t, std::size_t end) -> GraphemeResult {
        if (index == boundaries.size()) {
          return std::unexpected(ExecutionStatus::TypeError);
        }
        boundaries.subspan(index).front() = end;
        return GraphemeStep::Continue;
      });
  if (!count) {
    return std::unexpected(count.error());
  }
  if (*count != strings->size()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  auto slots = ElementSlots<StringObject*>(internal::ArrayBytes(array, *array.array_layout()));
  std::size_t begin = 0;
  for (const auto end : boundaries) {
    auto* segment = AllocateString(text.substr(begin, end - begin));
    if (segment == nullptr) {
      return std::unexpected(ExecutionStatus::OutOfMemory);
    }
    slots.front() = segment;
    WriteBarrier(array, Value::String(segment));
    slots = slots.subspan(1);
    begin = end;
  }
  return {};
}

Result ExecuteUnicodeBuiltin(Builtin builtin, std::span<const Value> arguments, Heap& heap) {
  const auto receiver = arguments.front();
  if (!receiver.IsString()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto text = receiver.AsString()->value();
  if (builtin == Builtin::StringGraphemeLength) {
    const auto count =
        WalkGraphemes(text, [](std::size_t, std::size_t, std::size_t) -> GraphemeResult {
          return GraphemeStep::Continue;
        });
    return count ? Result(Value::Integer(static_cast<std::int64_t>(*count)))
                 : Result(std::unexpected(count.error()));
  }
  if (builtin == Builtin::StringGraphemeSlice) {
    return GraphemeSlice(text, arguments, heap);
  }
  const auto target = arguments.subspan(1).front();
  if (!target.IsObject() || target.AsObject() == nullptr) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  if (builtin == Builtin::StringCodePoints) {
    return FillCodePoints(text, *target.AsObject());
  }
  return heap.FillGraphemes(*target.AsObject(), text).transform([&]() { return target; });
}

}  // namespace vela::runtime
