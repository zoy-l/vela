#include "runtime/builtins.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <new>
#include <span>
#include <string_view>
#include <utility>

#include "base/builtin.h"
#include "base/utf8.h"
#include "runtime/array_operations.h"
#include "runtime/collection_operations.h"
#include "runtime/heap.h"
#include "runtime/string.h"
#include "runtime/unicode_operations.h"
#include "runtime/value.h"
#include "runtime/value_io.h"

namespace vela::runtime {
namespace {

using Result = std::expected<Value, BuiltinError>;
constexpr std::size_t kStackSearchLength = 64;

struct PrefixDeleter final {
  void operator()(void* memory) const noexcept { ::operator delete(memory); }
};

Result StringResult(StringObject* string) {
  if (string == nullptr) {
    return std::unexpected(BuiltinError::OutOfMemory);
  }
  return Value::String(string);
}

Result ScalarToString(Value value, Heap& heap) {
  if (!value.IsInteger() && !value.IsDouble() && !value.IsBoolean()) {
    return std::unexpected(BuiltinError::TypeError);
  }
  std::array<char, kScalarTextCapacity> bytes{};
  return StringResult(heap.AllocateString(FormatScalar(value, bytes)));
}

Result ArrayJoin(std::span<const Value> arguments, Heap& heap) {
  const auto array = arguments.subspan(0).front();
  const auto separator = arguments.subspan(1).front();
  if (!array.IsObject() || array.AsObject() == nullptr || !separator.IsString()) {
    return std::unexpected(BuiltinError::TypeError);
  }
  const auto joined = heap.JoinArray(*array.AsObject(), separator.AsString()->value());
  if (!joined) {
    return std::unexpected(joined.error() == StringJoinError::TypeMismatch
                               ? BuiltinError::TypeError
                               : BuiltinError::OutOfMemory);
  }
  return Value::String(*joined);
}

Result MutableStringOperation(Builtin builtin, std::span<const Value> arguments, Heap& heap) {
  if (builtin == Builtin::NewMutableString) {
    auto* builder = heap.AllocateMutableString();
    if (builder == nullptr) {
      return std::unexpected(BuiltinError::OutOfMemory);
    }
    return Value::Object(builder);
  }
  const auto value = arguments.front();
  if (!value.IsObject() || value.AsObject() == nullptr ||
      !Heap::IsMutableString(*value.AsObject())) {
    return std::unexpected(BuiltinError::TypeError);
  }
  auto& builder = *value.AsObject();
  if (builtin == Builtin::MutableStringToString) {
    return StringResult(heap.BuildString(builder));
  }
  const auto text = arguments.subspan(1).front();
  if (!text.IsString()) {
    return std::unexpected(BuiltinError::TypeError);
  }
  if (!heap.AppendString(builder, text.AsString()->value())) {
    return std::unexpected(BuiltinError::OutOfMemory);
  }
  return Value::Null();
}

Result Slice(Builtin builtin, std::string_view text, std::span<const Value> arguments, Heap& heap) {
  if (!arguments.subspan(1).front().IsInteger() ||
      (builtin == Builtin::StringSlice && !arguments.subspan(2).front().IsInteger())) {
    return std::unexpected(BuiltinError::TypeError);
  }
  const auto start = arguments.subspan(1).front().AsInteger();
  const auto end =
      builtin == Builtin::StringIndex ? start : arguments.subspan(2).front().AsInteger();
  if (start < 0 || end < start || std::cmp_greater(end, text.size())) {
    return std::unexpected(BuiltinError::IndexOutOfBounds);
  }
  const auto offset = Utf8Offset(text, static_cast<std::size_t>(start));
  if (!offset) {
    return std::unexpected(BuiltinError::IndexOutOfBounds);
  }
  const auto suffix = text.substr(*offset);
  const auto count = builtin == Builtin::StringIndex ? 1 : static_cast<std::size_t>(end - start);
  const auto size = Utf8Offset(suffix, count);
  if (!size) {
    return std::unexpected(BuiltinError::IndexOutOfBounds);
  }
  return StringResult(heap.AllocateString(suffix.substr(0, *size)));
}

std::expected<std::size_t, BuiltinError> Find(std::string_view text_view,
                                              std::string_view part_view) {
  if (part_view.size() <= 1 || part_view.size() > text_view.size()) {
    return text_view.find(part_view);
  }
  const auto text = std::span(text_view);
  const auto part = std::span(part_view);
  // KMP 保证重复前缀等最坏输入仍为线性时间；常用短模式无需分配临时堆存储。
  std::array<std::size_t, kStackSearchLength> local{};
  std::unique_ptr<void, PrefixDeleter> storage;
  std::span<std::size_t> prefix(local);
  if (part.size() > local.size()) {
    storage.reset(::operator new(part.size() * sizeof(std::size_t), std::nothrow));
    if (storage == nullptr) {
      return std::unexpected(BuiltinError::OutOfMemory);
    }
    prefix = {static_cast<std::size_t*>(storage.get()), part.size()};
  }
  prefix.subspan(0).front() = 0;
  for (std::size_t index = 1, matched = 0; index < part.size(); ++index) {
    while (matched != 0 && part.subspan(index).front() != part.subspan(matched).front()) {
      matched = prefix.subspan(matched - 1).front();
    }
    if (part.subspan(index).front() == part.subspan(matched).front()) {
      ++matched;
    }
    prefix.subspan(index).front() = matched;
  }
  for (std::size_t index = 0, matched = 0; index < text.size(); ++index) {
    while (matched != 0 && text.subspan(index).front() != part.subspan(matched).front()) {
      matched = prefix.subspan(matched - 1).front();
    }
    if (text.subspan(index).front() == part.subspan(matched).front()) {
      ++matched;
    }
    if (matched == part.size()) {
      return index + 1 - matched;
    }
  }
  return std::string_view::npos;
}

}  // namespace

Result ConvertNumber(NumericConversion conversion, Value value) {
  if (!value.IsInteger() && !value.IsDouble()) {
    return std::unexpected(BuiltinError::TypeError);
  }
  if (conversion == NumericConversion::ToDouble) {
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
    return std::unexpected(BuiltinError::InvalidNumericConversion);
  }
  return Value::Integer(static_cast<std::int64_t>(truncated));
}

Result ExecuteBuiltin(Builtin builtin, std::span<const Value> arguments, Heap& heap) {
  switch (builtin) {
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
    case Builtin::ArrayDefaultCompare:
    case Builtin::ArrayClear:
    case Builtin::ArrayReserve:
    case Builtin::ArrayMutation:
    case Builtin::ArrayCheckMutation:
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
    case Builtin::ArrayLength:
    case Builtin::StringCodePoints:
    case Builtin::StringGraphemes:
    case Builtin::StringGraphemeLength:
    case Builtin::StringGraphemeSlice:
      return ExecuteUnicodeBuiltin(builtin, arguments, heap);
    case Builtin::NumericToInt:
    case Builtin::NumericToDouble:
      return ConvertNumber(builtin == Builtin::NumericToDouble ? NumericConversion::ToDouble
                                                               : NumericConversion::ToInt,
                           arguments.front());
    case Builtin::ScalarToString:
      return ScalarToString(arguments.front(), heap);
    case Builtin::ArrayJoin:
      return ArrayJoin(arguments, heap);
    case Builtin::NewMutableString:
    case Builtin::MutableStringAppend:
    case Builtin::MutableStringToString:
      return MutableStringOperation(builtin, arguments, heap);
    default:
      break;
  }
  if (!arguments.front().IsString()) {
    return std::unexpected(BuiltinError::TypeError);
  }
  const auto text = arguments.front().AsString()->value();
  if (builtin == Builtin::StringLength || builtin == Builtin::StringByteLength) {
    return Value::Integer(static_cast<std::int64_t>(
        builtin == Builtin::StringLength ? Utf8Length(text) : text.size()));
  }
  if (builtin == Builtin::StringIndex || builtin == Builtin::StringSlice) {
    return Slice(builtin, text, arguments, heap);
  }
  if (!arguments.subspan(1).front().IsString()) {
    return std::unexpected(BuiltinError::TypeError);
  }
  const auto part = arguments.subspan(1).front().AsString()->value();
  if (builtin == Builtin::StringStartsWith) {
    return Value::Boolean(text.starts_with(part));
  }
  if (builtin == Builtin::StringEndsWith) {
    return Value::Boolean(text.ends_with(part));
  }
  const auto found = Find(text, part);
  if (!found) {
    return std::unexpected(found.error());
  }
  if (builtin == Builtin::StringIncludes) {
    return Value::Boolean(*found != std::string_view::npos);
  }
  return Value::Integer(*found == std::string_view::npos
                            ? -1
                            : static_cast<std::int64_t>(Utf8Length(text.substr(0, *found))));
}

}  // namespace vela::runtime
