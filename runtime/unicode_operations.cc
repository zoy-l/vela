#include "runtime/unicode_operations.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>

#include <utf8proc.h>

#include "base/builtin.h"
#include "base/utf8.h"
#include "runtime/builtins.h"
#include "runtime/heap.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

using Result = std::expected<Value, BuiltinError>;
using Step = std::expected<void, BuiltinError>;

std::size_t Decode(std::string_view text, utf8proc_int32_t& point) {
  // 字符串在输入边界已验证；这里读取非空 UTF-8 后缀，不重复做全串验证。
  const auto decoded = DecodeUtf8Prefix(text);
  point = static_cast<utf8proc_int32_t>(decoded.value);
  return decoded.width;
}

template <typename Visitor>
std::expected<std::size_t, BuiltinError> WalkGraphemes(std::string_view text,
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
  if (!numbers) {
    return std::unexpected(BuiltinError::TypeError);
  }
  std::size_t offset = 0;
  std::size_t index = 0;
  while (offset < text.size() && index < numbers->size()) {
    utf8proc_int32_t point = 0;
    offset += Decode(text.substr(offset), point);
    numbers->subspan(index++).front() = point;
  }
  if (offset != text.size() || index != numbers->size()) {
    return std::unexpected(BuiltinError::TypeError);
  }
  return Value::Object(&array);
}

Result FillGraphemes(std::string_view text, HeapObject& array, Heap& heap) {
  const auto count =
      WalkGraphemes(text, [&](std::size_t index, std::size_t begin, std::size_t end) -> Step {
        auto* segment = heap.AllocateString(text.substr(begin, end - begin));
        if (segment == nullptr) {
          return std::unexpected(BuiltinError::OutOfMemory);
        }
        if (!heap.SetElement(array, static_cast<std::uint32_t>(index), Value::String(segment))) {
          return std::unexpected(BuiltinError::TypeError);
        }
        return {};
      });
  if (!count) {
    return std::unexpected(count.error());
  }
  const auto length = Heap::ArrayLength(array);
  if (!length || *count != *length) {
    return std::unexpected(BuiltinError::TypeError);
  }
  return Value::Object(&array);
}

Result GraphemeSlice(std::string_view text, std::span<const Value> arguments, Heap& heap) {
  const auto start_value = arguments.subspan(1).front();
  const auto end_value = arguments.subspan(2).front();
  if (!start_value.IsInteger() || !end_value.IsInteger()) {
    return std::unexpected(BuiltinError::TypeError);
  }
  const auto start = start_value.AsInteger();
  const auto end = end_value.AsInteger();
  if (start < 0 || end < start) {
    return std::unexpected(BuiltinError::IndexOutOfBounds);
  }
  std::optional<std::size_t> first = start == 0 ? std::optional<std::size_t>(0) : std::nullopt;
  std::optional<std::size_t> last = end == 0 ? std::optional<std::size_t>(0) : std::nullopt;
  const auto count = WalkGraphemes(
      text, [&](std::size_t index, std::size_t /*begin*/, std::size_t finish) -> Step {
        if (index + 1 == static_cast<std::uint64_t>(start)) {
          first = finish;
        }
        if (index + 1 == static_cast<std::uint64_t>(end)) {
          last = finish;
        }
        return {};
      });
  if (!count) {
    return std::unexpected(count.error());
  }
  if (!first || !last) {
    return std::unexpected(BuiltinError::IndexOutOfBounds);
  }
  auto* result = heap.AllocateString(text.substr(*first, *last - *first));
  return result == nullptr ? Result(std::unexpected(BuiltinError::OutOfMemory))
                           : Result(Value::String(result));
}

}  // namespace

Result ExecuteUnicodeBuiltin(Builtin builtin, std::span<const Value> arguments, Heap& heap) {
  const auto receiver = arguments.front();
  if (!receiver.IsString()) {
    return std::unexpected(BuiltinError::TypeError);
  }
  const auto text = receiver.AsString()->value();
  if (builtin == Builtin::StringGraphemeLength) {
    const auto count =
        WalkGraphemes(text, [](std::size_t, std::size_t, std::size_t) -> Step { return {}; });
    return count ? Result(Value::Integer(static_cast<std::int64_t>(*count)))
                 : Result(std::unexpected(count.error()));
  }
  if (builtin == Builtin::StringGraphemeSlice) {
    return GraphemeSlice(text, arguments, heap);
  }
  const auto target = arguments.subspan(1).front();
  if (!target.IsObject() || target.AsObject() == nullptr) {
    return std::unexpected(BuiltinError::TypeError);
  }
  return builtin == Builtin::StringCodePoints ? FillCodePoints(text, *target.AsObject())
                                              : FillGraphemes(text, *target.AsObject(), heap);
}

}  // namespace vela::runtime
