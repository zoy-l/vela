#include "runtime/strings/string_byte_operations.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <utility>

#include "base/builtin.h"
#include "base/execution_status.h"
#include "base/utf8.h"
#include "runtime/heap/heap.h"
#include "runtime/strings/string.h"
#include "runtime/value/value.h"
#include "runtime/value/value_operations.h"

namespace vela::runtime {
namespace {

bool IsBoundary(std::string_view text, std::int64_t offset) {
  return offset >= 0 && std::cmp_less_equal(offset, text.size()) &&
         (std::cmp_equal(offset, text.size()) ||
          !IsContinuationByte(text[static_cast<std::size_t>(offset)]));
}

bool IsByteRange(std::string_view text, std::int64_t start, std::int64_t end) {
  return start <= end && IsBoundary(text, start) && IsBoundary(text, end);
}

bool IsWhitespace(std::uint32_t point) {
  // Unicode White_Space；不包含 BOM、零宽空格或内嵌 NUL。
  switch (point) {
    case U'\t':
    case U'\n':
    case U'\v':
    case U'\f':
    case U'\r':
    case U' ':
    case U'\u0085':
    case U'\u00a0':
    case U'\u1680':
    case U'\u2028':
    case U'\u2029':
    case U'\u202f':
    case U'\u205f':
    case U'\u3000':
      return true;
    default:
      return point >= U'\u2000' && point <= U'\u200a';
  }
}

struct TrimmedString final {
  std::string_view text;
  std::size_t removed_points;
};

TrimmedString Trim(std::string_view text) {
  std::size_t start = 0;
  auto end = text.size();
  std::size_t removed_points = 0;
  while (start < end) {
    const auto first = static_cast<std::uint8_t>(text[start]);
    const auto point = first < kContinuationTag ? Utf8Point{.value = first, .width = 1}
                                                : DecodeUtf8Prefix(text.substr(start));
    if (!IsWhitespace(point.value)) {
      break;
    }
    start += point.width;
    ++removed_points;
  }
  while (end > start) {
    auto previous = end - 1;
    while (IsContinuationByte(text[previous])) {
      --previous;
    }
    const auto first = static_cast<std::uint8_t>(text[previous]);
    const auto point =
        first < kContinuationTag ? first : DecodeUtf8Prefix(text.substr(previous)).value;
    if (!IsWhitespace(point)) {
      break;
    }
    end = previous;
    ++removed_points;
  }
  return {.text = text.substr(start, end - start), .removed_points = removed_points};
}

}  // namespace

std::expected<StringObject*, ExecutionStatus> SliceStringBytes(Heap& heap,
                                                               const StringObject& string,
                                                               std::int64_t start,
                                                               std::int64_t end) {
  const auto text = string.value();
  if (!IsByteRange(text, start, end)) {
    return std::unexpected(ExecutionStatus::IndexOutOfBounds);
  }
  auto* result = heap.AllocateString(
      text.substr(static_cast<std::size_t>(start), static_cast<std::size_t>(end - start)));
  if (result == nullptr) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  return result;
}

StringObject* TrimString(Heap& heap, StringObject& string) {
  const auto text = string.value();
  const auto trimmed = Trim(text);
  return trimmed.text.size() == text.size()
             ? &string
             : heap.AllocateString(trimmed.text, string.length() - trimmed.removed_points);
}

std::expected<Value, ExecutionStatus> ExecuteStringByteBuiltin(Builtin builtin,
                                                               std::span<const Value> arguments,
                                                               Heap& heap) {
  const auto receiver = arguments.front();
  if (!receiver.IsString()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  auto& string = *receiver.AsString();
  const auto text = string.value();
  if (builtin == Builtin::StringTrim) {
    return AllocatedValue(TrimString(heap, string));
  }
  if (!arguments[1].IsInteger()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto start = arguments[1].AsInteger();
  if (builtin == Builtin::StringIsCodePointBoundary) {
    return Value::Boolean(IsBoundary(text, start));
  }
  if (builtin == Builtin::StringByteOffsetAt) {
    if (start < 0 || std::cmp_greater(start, string.length())) {
      return Value::Null();
    }
    return Value::Integer(
        static_cast<std::int64_t>(string.ByteOffset(static_cast<std::uint32_t>(start))));
  }
  if (!arguments[2].IsInteger() ||
      (builtin == Builtin::StringReplaceBytes && !arguments[3].IsString())) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto end = arguments[2].AsInteger();
  if (builtin == Builtin::StringSliceBytes) {
    return SliceStringBytes(heap, string, start, end).transform([](StringObject* result) {
      return Value::String(result);
    });
  }
  if (!IsByteRange(text, start, end)) {
    return std::unexpected(ExecutionStatus::IndexOutOfBounds);
  }
  const auto& inserted = *arguments[3].AsString();
  // 接收者与 inserted 由调用窗口保根；堆复用码点元数据并只分配一次结果。
  return AllocatedValue(heap.ReplaceStringBytes(string, static_cast<std::size_t>(start),
                                                static_cast<std::size_t>(end), inserted));
}

}  // namespace vela::runtime
