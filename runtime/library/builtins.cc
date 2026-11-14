#include "runtime/library/builtins.h"
#include "runtime/library/numeric_parse.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <utility>

#include "base/builtin.h"
#include "base/execution_status.h"
#include "base/utf8.h"
#include "runtime/collections/array_operations.h"
#include "runtime/collections/collection_operations.h"
#include "runtime/heap/heap.h"
#include "runtime/heap/native_buffer.h"
#include "runtime/strings/string.h"
#include "runtime/strings/unicode_operations.h"
#include "runtime/testing/test_inspection.h"
#include "runtime/value/value.h"
#include "runtime/value/value_io.h"
#include "runtime/value/value_operations.h"

namespace vela::runtime {
namespace {

using Result = std::expected<Value, ExecutionStatus>;

Result ScalarToString(Value value, Heap& heap) {
  if (!value.IsInteger() && !value.IsDouble() && !value.IsBoolean()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  return AllocatedValue(heap.AllocateScalarString(value));
}

Result ArrayJoin(std::span<const Value> arguments, Heap& heap) {
  const auto array = arguments[0];
  const auto separator = arguments[1];
  auto* object = array.AsObjectOrNull();
  if (object == nullptr || !separator.IsString()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto joined = heap.JoinArray(*object, separator.AsString()->value());
  if (!joined) {
    return std::unexpected(StringJoinStatus(joined.error()));
  }
  return Value::String(*joined);
}

Result AppendCodePoint(HeapObject& builder, Value value, Heap& heap) {
  if (!value.IsInteger()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto point = value.AsInteger();
  if (point < 0 || std::cmp_greater(point, kUnicodeMaximum) ||
      (std::cmp_greater_equal(point, kHighSurrogateFirst) &&
       std::cmp_less_equal(point, kLowSurrogateLast))) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  constexpr std::size_t kMaximumUtf8Width = 4;
  std::array<char, kMaximumUtf8Width> bytes{};
  const auto width = EncodeUtf8(static_cast<std::uint32_t>(point), bytes);
  if (const auto appended = heap.AppendString(builder, {bytes.data(), width}); !appended) {
    return std::unexpected(appended.error());
  }
  return Value::Null();
}

Result MutableStringOperation(Builtin builtin, std::span<const Value> arguments, Heap& heap) {
  if (builtin == Builtin::NewMutableString) {
    return AllocatedValue(heap.AllocateMutableString());
  }
  const auto value = arguments.front();
  auto* object = value.AsObjectOrNull();
  if (object == nullptr || !Heap::IsMutableString(*object)) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  auto& builder = *object;
  if (builtin == Builtin::MutableStringToString) {
    return AllocatedValue(heap.BuildString(builder));
  }
  const auto text = arguments[1];
  if (builtin == Builtin::MutableStringAppendCodePoint) {
    return AppendCodePoint(builder, text, heap);
  }
  if (!text.IsString()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  if (!heap.AppendString(builder, text.AsString()->value())) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  return Value::Null();
}

Result Slice(Builtin builtin,
             const StringObject& string,
             std::span<const Value> arguments,
             Heap& heap) {
  if (!arguments[1].IsInteger() || (builtin == Builtin::StringSlice && !arguments[2].IsInteger())) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto start = arguments[1].AsInteger();
  if (start < 0 || std::cmp_greater(start, string.length())) {
    return std::unexpected(ExecutionStatus::IndexOutOfBounds);
  }
  const auto indexed = builtin != Builtin::StringSlice;
  const auto end = indexed ? start + 1 : arguments[2].AsInteger();
  if (end < start || std::cmp_greater(end, string.length())) {
    return std::unexpected(ExecutionStatus::IndexOutOfBounds);
  }
  const auto offset = string.ByteOffset(static_cast<std::uint32_t>(start));
  const auto text = string.value();
  if (builtin == Builtin::StringCodePointAt) {
    return Value::Integer(DecodeUtf8Prefix(text.substr(offset)).value);
  }
  const auto end_offset = indexed ? offset + DecodeUtf8Prefix(text.substr(offset)).width
                                  : string.ByteOffset(static_cast<std::uint32_t>(end));
  return AllocatedValue(heap.AllocateString(text.substr(offset, end_offset - offset)));
}

std::expected<std::int64_t, ExecutionStatus> Find(std::string_view text_view,
                                                  std::string_view part_view,
                                                  Heap& heap) {
  if (part_view.empty()) {
    return 0;
  }
  if (part_view.size() > text_view.size()) {
    return -1;
  }
  if (part_view.size() == 1) {
    const auto found = text_view.find(part_view.front());
    return found == std::string_view::npos
               ? -1
               : static_cast<std::int64_t>(Utf8Length(text_view.substr(0, found)));
  }
  const auto text = std::span(text_view);
  const auto part = std::span(part_view);
  // KMP 保证重复前缀等最坏输入仍为线性时间；常用短模式无需分配临时堆存储。
  constexpr std::size_t kInlinePrefix = 64;
  NativeBuffer<std::size_t, kInlinePrefix> storage(heap, part.size());
  if (!storage.IsAllocated()) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  const auto prefix = storage.values();
  prefix[0] = 0;
  for (std::size_t index = 1, matched = 0; index < part.size(); ++index) {
    while (matched != 0 && part[index] != part[matched]) {
      matched = prefix[matched - 1];
    }
    if (part[index] == part[matched]) {
      ++matched;
    }
    prefix[index] = matched;
  }
  const auto part_length = Utf8Length(part_view);
  std::size_t points = 0;
  for (std::size_t index = 0, matched = 0; index < text.size(); ++index) {
    const auto byte = text[index];
    // 合法 UTF-8 中每个非续字节恰好开始一个码点；不再回扫命中前缀。
    if (!IsContinuationByte(byte)) {
      ++points;
    }
    while (matched != 0 && byte != part[matched]) {
      matched = prefix[matched - 1];
    }
    if (byte == part[matched]) {
      ++matched;
    }
    if (matched == part.size()) {
      return static_cast<std::int64_t>(points - part_length);
    }
  }
  return -1;
}

Result ConvertNumber(Builtin builtin, Value value) {
  if (!value.IsInteger() && !value.IsDouble()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  if (builtin == Builtin::NumericToDouble) {
    return value.IsDouble() ? value : Value::Double(static_cast<double>(value.AsInteger()));
  }
  if (value.IsInteger()) {
    return value;
  }
  const auto truncated = std::trunc(value.AsDouble());
  // int64 上界转为 double 会舍入到 2^63，因此用精确的半开范围检查后再转换。
  constexpr double kIntegerLower = -0x1p63;
  constexpr double kIntegerUpper = 0x1p63;
  if (!std::isfinite(truncated) || truncated < kIntegerLower || truncated >= kIntegerUpper) {
    return std::unexpected(ExecutionStatus::InvalidNumericConversion);
  }
  return Value::Integer(static_cast<std::int64_t>(truncated));
}

}  // namespace

Result ExecuteBuiltin(Builtin builtin, std::span<const Value> arguments, Heap& heap) {
  switch (builtin) {
    case Builtin::TestInspect:
      return InspectTestValue(arguments, heap);
    case Builtin::TestSame:
      return Value::Boolean(IsEqualValue(arguments.front(), arguments[1]));
    case Builtin::NumericToInt:
    case Builtin::NumericToDouble:
      return ConvertNumber(builtin, arguments.front());
    case Builtin::IntParse:
    case Builtin::IntTryParse:
    case Builtin::DoubleParse:
    case Builtin::DoubleTryParse:
      return ExecuteNumericParse(builtin, arguments);
    case Builtin::StringToLowerCase:
    case Builtin::StringToUpperCase: {
      const auto source = arguments.front();
      if (!source.IsString()) {
        return std::unexpected(ExecutionStatus::TypeError);
      }
      return AllocatedValue(heap.ConvertStringCase(
          *source.AsString(),
          builtin == Builtin::StringToLowerCase ? StringCase::Lower : StringCase::Upper));
    }
    case Builtin::ArraySlice:
    case Builtin::ArrayFill:
    case Builtin::ArrayReverse:
    case Builtin::ArrayCapacity:
    case Builtin::ArrayPush:
    case Builtin::ArrayPop:
    case Builtin::ArrayPushMany:
    case Builtin::ArrayShift:
    case Builtin::ArrayUnshift:
    case Builtin::ArraySplice:
    case Builtin::ArraySortDefault:
    case Builtin::ArraySortIntAscending:
    case Builtin::ArraySortIntDescending:
    case Builtin::ArrayClear:
    case Builtin::ArrayReserve:
    case Builtin::ArrayMutation:
    case Builtin::ArrayCheckMutation:
    case Builtin::ArrayLength:
      return ExecuteArrayBuiltin(builtin, arguments, heap);
    case Builtin::CollectionSize:
    case Builtin::SetAdd:
    case Builtin::CollectionHas:
    case Builtin::CollectionDelete:
    case Builtin::CollectionClear:
    case Builtin::MapGet:
    case Builtin::MapSet:
    case Builtin::CollectionKeys:
    case Builtin::CollectionValues:
    case Builtin::SetUnion:
    case Builtin::SetIntersection:
    case Builtin::SetDifference:
      return ExecuteCollectionBuiltin(builtin, arguments, heap);
    case Builtin::StringCodePoints:
    case Builtin::StringGraphemes:
    case Builtin::StringGraphemeLength:
    case Builtin::StringGraphemeSlice:
      return ExecuteUnicodeBuiltin(builtin, arguments, heap);
    case Builtin::ScalarToString:
      return ScalarToString(arguments.front(), heap);
    case Builtin::ScalarTextLength: {
      const auto value = arguments.front();
      if (!value.IsInteger() && !value.IsDouble() && !value.IsBoolean()) {
        return std::unexpected(ExecutionStatus::TypeError);
      }
      return Value::Integer(static_cast<std::int64_t>(ScalarTextLength(value)));
    }
    case Builtin::ArrayJoin:
      return ArrayJoin(arguments, heap);
    case Builtin::NewMutableString:
    case Builtin::MutableStringAppend:
    case Builtin::MutableStringAppendCodePoint:
    case Builtin::MutableStringToString:
      return MutableStringOperation(builtin, arguments, heap);
    default:
      break;
  }
  if (!arguments.front().IsString()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto& string = *arguments.front().AsString();
  const auto text = string.value();
  if (builtin == Builtin::StringLength || builtin == Builtin::StringByteLength) {
    return Value::Integer(static_cast<std::int64_t>(
        builtin == Builtin::StringLength ? string.length() : text.size()));
  }
  if (builtin == Builtin::StringIndex || builtin == Builtin::StringSlice ||
      builtin == Builtin::StringCodePointAt) {
    return Slice(builtin, string, arguments, heap);
  }
  if (!arguments[1].IsString()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto part = arguments[1].AsString()->value();
  if (builtin == Builtin::StringStartsWith) {
    return Value::Boolean(text.starts_with(part));
  }
  if (builtin == Builtin::StringEndsWith) {
    return Value::Boolean(text.ends_with(part));
  }
  const auto found = Find(text, part, heap);
  if (!found) {
    return std::unexpected(found.error());
  }
  if (builtin == Builtin::StringIncludes) {
    return Value::Boolean(*found >= 0);
  }
  if (builtin == Builtin::StringIndexOf) {
    return Value::Integer(*found);
  }
  std::unreachable();
}

}  // namespace vela::runtime
