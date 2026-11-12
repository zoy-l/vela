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
#include "runtime/heap/heap.h"
#include "runtime/strings/string.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"
#include "runtime/value/value_operations.h"

namespace vela::runtime {
namespace {

using Result = std::expected<Value, ExecutionStatus>;
enum class GraphemeStep : std::uint8_t { Continue, Stop };
using GraphemeResult = std::expected<GraphemeStep, ExecutionStatus>;

template <typename Visitor>
std::expected<std::size_t, ExecutionStatus> WalkGraphemes(std::string_view text,
                                                          const Visitor& visitor) {
  std::size_t offset = 0;
  std::size_t count = 0;
  utf8proc_int32_t previous = 0;
  utf8proc_int32_t state = 0;
  while (offset < text.size()) {
    const auto decoded = DecodeUtf8Prefix(text.substr(offset));
    const auto point = static_cast<utf8proc_int32_t>(decoded.value);
    if (offset != 0 && utf8proc_grapheme_break_stateful(previous, point, &state)) {
      const auto delivered = visitor(count++, offset);
      if (!delivered) {
        return std::unexpected(delivered.error());
      }
      if (*delivered == GraphemeStep::Stop) {
        return count;
      }
    }
    previous = point;
    offset += decoded.width;
  }
  if (!text.empty()) {
    const auto delivered = visitor(count++, text.size());
    if (!delivered) {
      return std::unexpected(delivered.error());
    }
  }
  return count;
}

Result FillCodePoints(const StringObject& string, HeapObject& array) {
  const auto numbers = Heap::IntegerElements(array);
  if (!numbers || string.length() != numbers->size()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  std::size_t offset = 0;
  std::size_t index = 0;
  const auto text = string.value();
  while (offset < text.size()) {
    const auto decoded = DecodeUtf8Prefix(text.substr(offset));
    (*numbers)[index++] = static_cast<std::int64_t>(decoded.value);
    offset += decoded.width;
  }
  return Value::Object(&array);
}

Result GraphemeSlice(std::string_view text, std::span<const Value> arguments, Heap& heap) {
  const auto start_value = arguments[1];
  const auto end_value = arguments[2];
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
    const auto count =
        WalkGraphemes(text, [&](std::size_t index, std::size_t finish) -> GraphemeResult {
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
  return AllocatedValue(heap.AllocateString(text.substr(*first, *last - *first)));
}

}  // namespace

std::expected<void, ExecutionStatus> FillGraphemeBoundaries(std::string_view text,
                                                            std::span<std::size_t> boundaries) {
  const auto count = WalkGraphemes(text, [&](std::size_t index, std::size_t end) -> GraphemeResult {
    if (index == boundaries.size()) {
      return std::unexpected(ExecutionStatus::TypeError);
    }
    boundaries[index] = end;
    return GraphemeStep::Continue;
  });
  if (!count) {
    return std::unexpected(count.error());
  }
  if (*count != boundaries.size()) {
    return std::unexpected(ExecutionStatus::TypeError);
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
    const auto count = WalkGraphemes(
        text, [](std::size_t, std::size_t) -> GraphemeResult { return GraphemeStep::Continue; });
    return count ? Result(Value::Integer(static_cast<std::int64_t>(*count)))
                 : Result(std::unexpected(count.error()));
  }
  if (builtin == Builtin::StringGraphemeSlice) {
    return GraphemeSlice(text, arguments, heap);
  }
  const auto target = arguments[1];
  auto* object = target.AsObjectOrNull();
  if (object == nullptr) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  if (builtin == Builtin::StringCodePoints) {
    return FillCodePoints(*receiver.AsString(), *object);
  }
  return heap.FillGraphemes(*object, text).transform([target] { return target; });
}

}  // namespace vela::runtime
