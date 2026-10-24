#include "runtime/string_operations.h"

#include <algorithm>
#include <array>
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
#include "runtime/heap.h"
#include "runtime/string.h"
#include "runtime/value.h"
#include "runtime/value_io.h"

namespace vela::runtime {
namespace {

using Result = std::expected<Value, StringError>;
constexpr std::size_t kStackSearchLength = 64;

struct PrefixDeleter final {
  void operator()(void* memory) const noexcept { ::operator delete(memory); }
};

auto StringResult(StringObject* string) -> Result {
  if (string == nullptr) {
    return std::unexpected(StringError::OutOfMemory);
  }
  return Value::String(string);
}

auto ScalarToString(Value value, Heap& heap) -> Result {
  if (!value.IsInteger() && !value.IsDouble() && !value.IsBoolean()) {
    return std::unexpected(StringError::TypeError);
  }
  std::array<char, kScalarTextCapacity> bytes{};
  return StringResult(heap.AllocateString(FormatScalar(value, bytes)));
}

auto Join(std::span<const Value> arguments, Heap& heap) -> Result {
  const auto array = arguments.subspan(0).front();
  const auto separator = arguments.subspan(1).front();
  if (!array.IsObject() || array.AsObject() == nullptr || !separator.IsString()) {
    return std::unexpected(StringError::TypeError);
  }
  const auto parts = Heap::StringElements(*array.AsObject());
  if (!parts || std::ranges::any_of(*parts, [](auto* part) -> bool { return part == nullptr; })) {
    return std::unexpected(StringError::TypeError);
  }
  return StringResult(heap.JoinStrings(*parts, separator.AsString()->value()));
}

auto BuilderOperation(Builtin builtin, std::span<const Value> arguments, Heap& heap) -> Result {
  if (builtin == Builtin::NewStringBuilder) {
    auto* builder = heap.AllocateStringBuilder();
    if (builder == nullptr) {
      return std::unexpected(StringError::OutOfMemory);
    }
    return Value::Object(builder);
  }
  const auto value = arguments.front();
  if (!value.IsObject() || value.AsObject() == nullptr ||
      !Heap::IsStringBuilder(*value.AsObject())) {
    return std::unexpected(StringError::TypeError);
  }
  auto& builder = *value.AsObject();
  if (builtin == Builtin::BuilderToString) {
    return StringResult(heap.BuildString(builder));
  }
  const auto text = arguments.subspan(1).front();
  if (!text.IsString()) {
    return std::unexpected(StringError::TypeError);
  }
  if (!heap.AppendString(builder, text.AsString()->value())) {
    return std::unexpected(StringError::OutOfMemory);
  }
  return Value::Null();
}

auto Slice(Builtin builtin, std::string_view text, std::span<const Value> arguments, Heap& heap)
    -> Result {
  if (!arguments.subspan(1).front().IsInteger() ||
      (builtin == Builtin::StringSlice && !arguments.subspan(2).front().IsInteger())) {
    return std::unexpected(StringError::TypeError);
  }
  const auto start = arguments.subspan(1).front().AsInteger();
  const auto end =
      builtin == Builtin::StringIndex ? start : arguments.subspan(2).front().AsInteger();
  if (start < 0 || end < start || std::cmp_greater(end, text.size())) {
    return std::unexpected(StringError::IndexOutOfBounds);
  }
  const auto offset = Utf8Offset(text, static_cast<std::size_t>(start));
  if (!offset) {
    return std::unexpected(StringError::IndexOutOfBounds);
  }
  const auto suffix = text.substr(*offset);
  const auto count = builtin == Builtin::StringIndex ? 1 : static_cast<std::size_t>(end - start);
  const auto size = Utf8Offset(suffix, count);
  if (!size) {
    return std::unexpected(StringError::IndexOutOfBounds);
  }
  return StringResult(heap.AllocateString(suffix.substr(0, *size)));
}

auto Find(std::string_view text_view, std::string_view part_view)
    -> std::expected<std::size_t, StringError> {
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
      return std::unexpected(StringError::OutOfMemory);
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

auto ExecuteStringBuiltin(Builtin builtin, std::span<const Value> arguments, Heap& heap) -> Result {
  switch (builtin) {
    case Builtin::ScalarToString:
      return ScalarToString(arguments.front(), heap);
    case Builtin::Join:
      return Join(arguments, heap);
    case Builtin::NewStringBuilder:
    case Builtin::BuilderAppend:
    case Builtin::BuilderToString:
      return BuilderOperation(builtin, arguments, heap);
    default:
      break;
  }
  if (!arguments.front().IsString()) {
    return std::unexpected(StringError::TypeError);
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
    return std::unexpected(StringError::TypeError);
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
  if (builtin == Builtin::StringContains) {
    return Value::Boolean(*found != std::string_view::npos);
  }
  return Value::Integer(*found == std::string_view::npos
                            ? -1
                            : static_cast<std::int64_t>(Utf8Length(text.substr(0, *found))));
}

}  // namespace vela::runtime
