#include "runtime/heap.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <new>
#include <span>
#include <string_view>

#include "runtime/array_storage.h"
#include "runtime/element_storage.h"
#include "runtime/object.h"
#include "runtime/register_storage.h"
#include "runtime/string.h"
#include "runtime/value.h"
#include "runtime/value_io.h"

namespace vela::runtime {
namespace {

constexpr std::size_t kInitialMutableStringCapacity = 64;
enum class TextContext : std::uint8_t { Interpolation, ArrayElement };

struct MutableStringBuffer final {
  char* data = nullptr;
  std::size_t capacity = 0;
};

MutableStringBuffer& Buffer(HeapObject& object) noexcept {
  void* storage = std::span(&object, 1).last(0).data();
  return *static_cast<MutableStringBuffer*>(storage);
}

const MutableStringBuffer& Buffer(const HeapObject& object) noexcept {
  const void* storage = std::span(&object, 1).last(0).data();
  return *static_cast<const MutableStringBuffer*>(storage);
}

std::span<char> StringStorage(StringObject& string, std::size_t length) noexcept {
  void* storage = std::span(&string, 1).last(0).data();
  return {static_cast<char*>(storage), length};
}

std::expected<std::string_view, StringJoinError>
StringText(Value value, std::span<char, kScalarTextCapacity> buffer, TextContext context) {
  if (value.IsNull()) {
    return context == TextContext::ArrayElement ? "" : "null";
  }
  if (value.IsString()) {
    return value.AsString()->value();
  }
  if (value.IsInteger() || value.IsDouble() || value.IsBoolean()) {
    return context == TextContext::ArrayElement ? FormatArrayScalar(value, buffer)
                                                : FormatScalar(value, buffer);
  }
  return std::unexpected(StringJoinError::TypeMismatch);
}

template <class Read, class Allocate>
std::expected<StringObject*, StringJoinError> JoinFormatted(std::size_t count,
                                                            std::string_view separator,
                                                            TextContext context,
                                                            const Read& read,
                                                            const Allocate& allocate) {
  std::size_t length = 0;
  if (count > 1) {
    const auto gaps = count - 1;
    if (separator.size() > kMaximumStringLength / gaps) {
      return std::unexpected(StringJoinError::OutOfMemory);
    }
    length = gaps * separator.size();
  }
  std::array<char, kScalarTextCapacity> buffer{};
  for (std::size_t index = 0; index < count; ++index) {
    const auto text = StringText(read(index), buffer, context);
    if (!text) {
      return std::unexpected(text.error());
    }
    if (text->size() > kMaximumStringLength - length) {
      return std::unexpected(StringJoinError::OutOfMemory);
    }
    length += text->size();
  }
  auto* string = allocate(length);
  if (string == nullptr) {
    return std::unexpected(StringJoinError::OutOfMemory);
  }
  auto remaining = StringStorage(*string, length);
  for (std::size_t index = 0; index < count; ++index) {
    if (index != 0) {
      std::ranges::copy(separator, remaining.begin());
      remaining = remaining.subspan(separator.size());
    }
    // read 只读取稳定值，不求值脚本表达式；分配前已检查所有值的可格式化性。
    const auto text = *StringText(read(index), buffer, context);
    std::ranges::copy(text, remaining.begin());
    remaining = remaining.subspan(text.size());
  }
  return string;
}

}  // namespace

StringObject* Heap::AllocateStringStorage(std::size_t length) noexcept {
  if (length > kMaximumStringLength) {
    return nullptr;
  }
  const auto requested_size = sizeof(StringObject) + length;
  MaybeCollect(requested_size);
  const auto allocation = AllocateRaw(requested_size);
  if (allocation.memory == nullptr) {
    return nullptr;
  }
  auto* string = std::construct_at(static_cast<StringObject*>(allocation.memory),
                                   StringObject::AllocationToken{});
  auto& header = string->header_;
  header.kind = ObjectKind::String;
  header.length = static_cast<std::uint32_t>(length);
  Publish(header, allocation.size);
  return string;
}

StringObject* Heap::AllocateString(std::string_view first, std::string_view second) noexcept {
  if (first.size() > kMaximumStringLength || second.size() > kMaximumStringLength - first.size()) {
    return nullptr;
  }
  const auto length = first.size() + second.size();
  auto* string = AllocateStringStorage(length);
  if (string == nullptr) {
    return nullptr;
  }
  const auto bytes = StringStorage(*string, length);
  std::ranges::copy(first, bytes.begin());
  std::ranges::copy(second, bytes.subspan(first.size()).begin());
  return string;
}

StringObject* Heap::JoinStrings(std::span<StringObject* const> parts,
                                std::string_view separator) noexcept {
  std::size_t length = 0;
  if (parts.size() > 1) {
    const auto gaps = parts.size() - 1;
    if (separator.size() > kMaximumStringLength / gaps) {
      return nullptr;
    }
    length = gaps * separator.size();
  }
  for (const auto* part : parts) {
    const auto size = part->value().size();
    if (size > kMaximumStringLength - length) {
      return nullptr;
    }
    length += size;
  }
  auto* string = AllocateStringStorage(length);
  if (string == nullptr) {
    return nullptr;
  }
  auto remaining = StringStorage(*string, length);
  for (std::size_t index = 0; index < parts.size(); ++index) {
    if (index != 0) {
      std::ranges::copy(separator, remaining.begin());
      remaining = remaining.subspan(separator.size());
    }
    const auto text = parts.subspan(index).front()->value();
    std::ranges::copy(text, remaining.begin());
    remaining = remaining.subspan(text.size());
  }
  return string;
}

std::expected<std::span<StringObject* const>, ArrayAccessError> Heap::StringElements(
    const HeapObject& array) noexcept {
  if (array.kind != ObjectKind::Array) {
    return std::unexpected(ArrayAccessError::NotArray);
  }
  if (array.array_layout()->element_kind != ValueKind::String) {
    return std::unexpected(ArrayAccessError::ElementTypeMismatch);
  }
  const void* storage = internal::ArrayBuffer(array).data;
  return std::span(static_cast<StringObject* const*>(storage), array.length);
}

std::expected<StringObject*, StringJoinError> Heap::JoinValues(
    std::span<const Value> parts) noexcept {
  return JoinFormatted(
      parts.size(), {}, TextContext::Interpolation,
      [parts](std::size_t index) -> Value { return parts.subspan(index).front(); },
      [this](std::size_t length) -> StringObject* { return AllocateStringStorage(length); });
}

std::expected<StringObject*, StringJoinError> Heap::JoinArray(const HeapObject& array,
                                                              std::string_view separator) noexcept {
  if (array.kind != ObjectKind::Array) {
    return std::unexpected(StringJoinError::TypeMismatch);
  }
  const auto& layout = *array.array_layout();
  if (layout.element_kind == ValueKind::String && !layout.nullable_elements) {
    const auto parts = *StringElements(array);
    if (std::ranges::any_of(parts, [](auto* part) -> bool { return part == nullptr; })) {
      return std::unexpected(StringJoinError::TypeMismatch);
    }
    auto* string = JoinStrings(parts, separator);
    if (string == nullptr) {
      return std::unexpected(StringJoinError::OutOfMemory);
    }
    return string;
  }
  if (layout.element_kind != ValueKind::String && layout.element_kind != ValueKind::Integer &&
      layout.element_kind != ValueKind::Double && layout.element_kind != ValueKind::Boolean) {
    return std::unexpected(StringJoinError::TypeMismatch);
  }
  const auto storage = internal::ArrayBytes(array, layout);
  return JoinFormatted(
      array.length, separator, TextContext::ArrayElement,
      [&layout, storage](std::size_t index) -> Value {
        return LoadElementStorage(layout, storage, index);
      },
      [this](std::size_t length) -> StringObject* { return AllocateStringStorage(length); });
}

std::expected<StringObject*, StringJoinError> Heap::JoinValues(RegisterView parts) noexcept {
  return JoinFormatted(
      parts.size(), {}, TextContext::Interpolation,
      [parts](std::size_t index) -> Value { return parts.subspan(index).front().Read(); },
      [this](std::size_t length) -> StringObject* { return AllocateStringStorage(length); });
}

HeapObject* Heap::AllocateMutableString() noexcept {
  const auto size = sizeof(HeapObject) + sizeof(MutableStringBuffer);
  MaybeCollect(size);
  const auto allocation = AllocateRaw(size);
  if (allocation.memory == nullptr) {
    return nullptr;
  }
  auto* object = std::construct_at(static_cast<HeapObject*>(allocation.memory));
  object->kind = ObjectKind::MutableString;
  void* storage = std::span(object, 1).last(0).data();
  std::construct_at(static_cast<MutableStringBuffer*>(storage));
  Publish(*object, allocation.size);
  return object;
}

bool Heap::IsMutableString(const HeapObject& object) noexcept {
  return object.kind == ObjectKind::MutableString;
}

std::expected<void, MutableStringError> Heap::AppendString(HeapObject& builder,
                                                           std::string_view text) noexcept {
  if (!IsMutableString(builder)) {
    return std::unexpected(MutableStringError::NotMutableString);
  }
  const auto length = builder.length;
  if (text.size() > kMaximumStringLength - length) {
    return std::unexpected(MutableStringError::OutOfMemory);
  }
  const auto new_length = length + text.size();
  auto& buffer = Buffer(builder);
  if (new_length > buffer.capacity) {
    const auto capacity = std::max({
        new_length,
        kInitialMutableStringCapacity,
        std::min(kMaximumStringLength, buffer.capacity * 2),
    });
    MaybeCollect(capacity);
    auto* memory = static_cast<char*>(::operator new(capacity, std::nothrow));
    if (memory == nullptr) {
      return std::unexpected(MutableStringError::OutOfMemory);
    }
    std::ranges::copy(std::span(buffer.data, length), memory);
    const auto old_capacity = buffer.capacity;
    ::operator delete(buffer.data);
    allocated_bytes_ += capacity - old_capacity;
    committed_bytes_ += capacity - old_capacity;
    buffer = {.data = memory, .capacity = capacity};
  }
  if (!text.empty()) {
    std::ranges::copy(text, std::span(buffer.data, buffer.capacity).subspan(length).begin());
  }
  builder.length = static_cast<std::uint32_t>(new_length);
  return {};
}

StringObject* Heap::BuildString(const HeapObject& builder) noexcept {
  const auto& buffer = Buffer(builder);
  return AllocateString(std::string_view(buffer.data, builder.length));
}

void Heap::ReleaseMutableString(HeapObject& object) noexcept {
  auto& buffer = Buffer(object);
  allocated_bytes_ -= buffer.capacity;
  committed_bytes_ -= buffer.capacity;
  ::operator delete(buffer.data);
  std::destroy_at(&buffer);
}

}  // namespace vela::runtime
