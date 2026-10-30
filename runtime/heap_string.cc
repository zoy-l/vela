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

#include <absl/container/inlined_vector.h>

#include "base/execution_status.h"
#include "base/utf8.h"
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

constexpr std::size_t kInlineFormattedParts = 8;

MutableStringBuffer& Buffer(HeapObject& object) noexcept {
  return *StorageAfter<MutableStringBuffer>(object);
}

const MutableStringBuffer& Buffer(const HeapObject& object) noexcept {
  return *StorageAfter<MutableStringBuffer>(object);
}

std::span<char> StringStorage(StringObject& string, std::size_t length) noexcept {
  return {StorageAfter<char>(string), length};
}

std::expected<std::string_view, StringJoinError>
StringText(Value value, std::span<char, kScalarTextCapacity> buffer, TextContext context) {
  if (value.IsObject()) {
    return std::unexpected(StringJoinError::TypeMismatch);
  }
  if (context == TextContext::ArrayElement) {
    return value.IsNull() ? "" : FormatArrayValue(value, buffer);
  }
  if (value.IsNull()) {
    return "null";
  }
  if (value.IsString()) {
    return value.AsString()->value();
  }
  return FormatScalar(value, buffer);
}

template <class Read, class Allocate>
std::expected<StringObject*, StringJoinError> JoinFormatted(std::size_t count,
                                                            std::string_view separator,
                                                            TextContext context,
                                                            const Read& read,
                                                            const Allocate& allocate) {
  std::size_t length = 0;
  std::size_t code_points = 0;
  if (count > 1) {
    const auto gaps = count - 1;
    if (separator.size() > kMaximumStringLength / gaps) {
      return std::unexpected(StringJoinError::OutOfMemory);
    }
    length = gaps * separator.size();
    code_points = gaps * Utf8Length(separator);
  }
  absl::InlinedVector<char, kInlineFormattedParts * kScalarTextCapacity> scalars;
  absl::InlinedVector<std::size_t, kInlineFormattedParts> scalar_lengths;
  std::array<char, kScalarTextCapacity> buffer{};
  for (std::size_t index = 0; index < count; ++index) {
    const auto value = read(index);
    const auto text = StringText(value, buffer, context);
    if (!text) {
      return std::unexpected(text.error());
    }
    if (text->size() > kMaximumStringLength - length) {
      return std::unexpected(StringJoinError::OutOfMemory);
    }
    length += text->size();
    code_points += value.IsString() ? value.AsString()->length() : text->size();
    if (!value.IsString() && !value.IsNull()) {
      scalars.insert(scalars.end(), text->begin(), text->end());
      scalar_lengths.push_back(text->size());
    }
  }
  auto* string = allocate({.bytes = length, .code_points = code_points});
  if (string == nullptr) {
    return std::unexpected(StringJoinError::OutOfMemory);
  }
  auto remaining = StringStorage(*string, length);
  auto cached = std::span<const char>(scalars);
  auto lengths = std::span<const std::size_t>(scalar_lengths);
  for (std::size_t index = 0; index < count; ++index) {
    if (index != 0) {
      std::ranges::copy(separator, remaining.begin());
      remaining = remaining.subspan(separator.size());
    }
    // read 只读取保根的稳定值；字符串直接借用原视图，标量使用首轮格式化的缓存。
    const auto value = read(index);
    std::string_view text;
    if (value.IsString() || value.IsNull()) {
      text = *StringText(value, buffer, context);
    } else {
      const auto size = lengths.front();
      lengths = lengths.subspan(1);
      text = {cached.data(), size};
      cached = cached.subspan(size);
    }
    std::ranges::copy(text, remaining.begin());
    remaining = remaining.subspan(text.size());
  }
  return string;
}

}  // namespace

StringObject* Heap::AllocateStringStorage(StringSize size) noexcept {
  const auto length = size.bytes;
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
  header.string_length = static_cast<std::uint32_t>(size.code_points);
  Publish(header, allocation);
  return string;
}

StringObject* Heap::AllocateString(std::string_view first, std::string_view second) noexcept {
  if (first.size() > kMaximumStringLength || second.size() > kMaximumStringLength - first.size()) {
    return nullptr;
  }
  const auto length = first.size() + second.size();
  auto* string = AllocateStringStorage({
      .bytes = length,
      .code_points = Utf8Length(first) + Utf8Length(second),
  });
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
  std::size_t code_points = 0;
  if (parts.size() > 1) {
    const auto gaps = parts.size() - 1;
    if (separator.size() > kMaximumStringLength / gaps) {
      return nullptr;
    }
    length = gaps * separator.size();
    code_points = gaps * Utf8Length(separator);
  }
  for (const auto* part : parts) {
    const auto size = part->value().size();
    if (size > kMaximumStringLength - length) {
      return nullptr;
    }
    length += size;
    code_points += part->length();
  }
  auto* string = AllocateStringStorage({.bytes = length, .code_points = code_points});
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
  const auto& layout = *array.array_layout();
  if (layout.element.kind != ValueKind::String || layout.HasTaggedElements()) {
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
      [this](StringSize size) -> StringObject* { return AllocateStringStorage(size); });
}

std::expected<StringObject*, StringJoinError> Heap::JoinArray(const HeapObject& array,
                                                              std::string_view separator) noexcept {
  if (array.kind != ObjectKind::Array) {
    return std::unexpected(StringJoinError::TypeMismatch);
  }
  const auto& layout = *array.array_layout();
  if (layout.element.kind == ValueKind::String && !layout.element.nullable) {
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
  if (layout.element.nominal != nullptr ||
      (!layout.element.dynamic && layout.element.kind != ValueKind::String &&
       layout.element.kind != ValueKind::Integer && layout.element.kind != ValueKind::Double &&
       layout.element.kind != ValueKind::Boolean)) {
    return std::unexpected(StringJoinError::TypeMismatch);
  }
  const auto storage = internal::ArrayBytes(array, layout);
  return JoinFormatted(
      array.length, separator, TextContext::ArrayElement,
      [&layout, storage](std::size_t index) -> Value {
        return LoadElementStorage(layout, storage, index);
      },
      [this](StringSize size) -> StringObject* { return AllocateStringStorage(size); });
}

std::expected<StringObject*, StringJoinError> Heap::JoinValues(RegisterView parts) noexcept {
  return JoinFormatted(
      parts.size(), {}, TextContext::Interpolation,
      [parts](std::size_t index) -> Value { return parts.subspan(index).front().Read(); },
      [this](StringSize size) -> StringObject* { return AllocateStringStorage(size); });
}

HeapObject* Heap::AllocateMutableString() noexcept {
  const auto size = sizeof(HeapObject) + sizeof(MutableStringBuffer);
  return AllocateObject(ObjectKind::MutableString, size, [](HeapObject& object) {
    std::construct_at(StorageAfter<MutableStringBuffer>(object));
  });
}

bool Heap::IsMutableString(const HeapObject& object) noexcept {
  return object.kind == ObjectKind::MutableString;
}

std::expected<void, ExecutionStatus> Heap::AppendString(HeapObject& builder,
                                                        std::string_view text) noexcept {
  const auto output = AppendStringStorage(builder, text.size());
  if (!output) {
    return std::unexpected(output.error());
  }
  std::ranges::copy(text, output->begin());
  return {};
}

std::expected<std::span<char>, ExecutionStatus> Heap::AppendStringStorage(
    HeapObject& builder,
    std::size_t count) noexcept {
  const auto length = builder.length;
  if (count > kMaximumStringLength - length) {
    return std::unexpected(ExecutionStatus::OutOfMemory);
  }
  const auto new_length = length + count;
  auto& buffer = Buffer(builder);
  if (new_length > buffer.capacity) {
    const auto capacity = std::max({
        new_length,
        kInitialMutableStringCapacity,
        std::min(kMaximumStringLength, buffer.capacity * 2),
    });
    MaybeCollect(capacity);
    if (!CanAllocate(capacity - buffer.capacity)) {
      return std::unexpected(ExecutionStatus::OutOfMemory);
    }
    auto* memory = static_cast<char*>(::operator new(capacity, std::nothrow));
    if (memory == nullptr) {
      return std::unexpected(ExecutionStatus::OutOfMemory);
    }
    std::ranges::copy(std::span(buffer.data, length), memory);
    const auto old_capacity = buffer.capacity;
    ::operator delete(buffer.data);
    allocated_bytes_ += capacity - old_capacity;
    committed_bytes_ += capacity - old_capacity;
    buffer = {.data = memory, .capacity = capacity};
  }
  builder.length = static_cast<std::uint32_t>(new_length);
  return std::span(buffer.data, buffer.capacity).subspan(length, count);
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
