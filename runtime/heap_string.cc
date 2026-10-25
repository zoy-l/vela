#include "runtime/heap.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <string_view>

#include "runtime/object.h"
#include "runtime/string.h"
#include "runtime/value.h"

namespace vela::runtime {
namespace {

constexpr std::size_t kMaximumStringLength =
    std::numeric_limits<std::uint32_t>::max() - sizeof(StringObject);
constexpr std::size_t kInitialBuilderCapacity = 64;

struct BuilderBuffer final {
  char* data = nullptr;
  std::size_t capacity = 0;
};

auto Buffer(HeapObject& object) noexcept -> BuilderBuffer& {
  void* storage = std::span(&object, 1).last(0).data();
  return *static_cast<BuilderBuffer*>(storage);
}

auto Buffer(const HeapObject& object) noexcept -> const BuilderBuffer& {
  const void* storage = std::span(&object, 1).last(0).data();
  return *static_cast<const BuilderBuffer*>(storage);
}

auto StringStorage(StringObject& string, std::size_t length) noexcept -> std::span<char> {
  void* storage = std::span(&string, 1).last(0).data();
  return {static_cast<char*>(storage), length};
}

}  // namespace

auto Heap::AllocateStringStorage(std::size_t length) noexcept -> StringObject* {
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

auto Heap::AllocateString(std::string_view first, std::string_view second) noexcept
    -> StringObject* {
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

auto Heap::JoinStrings(std::span<StringObject* const> parts, std::string_view separator) noexcept
    -> StringObject* {
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

auto Heap::StringElements(const HeapObject& array) noexcept
    -> std::expected<std::span<StringObject* const>, ArrayAccessError> {
  if (array.kind != ObjectKind::Array) {
    return std::unexpected(ArrayAccessError::NotArray);
  }
  if (array.array_layout()->element_kind != ValueKind::String) {
    return std::unexpected(ArrayAccessError::ElementTypeMismatch);
  }
  const void* storage = std::span(&array, 1).last(0).data();
  return std::span(static_cast<StringObject* const*>(storage), array.length);
}

auto Heap::AllocateStringBuilder() noexcept -> HeapObject* {
  const auto size = sizeof(HeapObject) + sizeof(BuilderBuffer);
  MaybeCollect(size);
  const auto allocation = AllocateRaw(size);
  if (allocation.memory == nullptr) {
    return nullptr;
  }
  auto* object = std::construct_at(static_cast<HeapObject*>(allocation.memory));
  object->kind = ObjectKind::StringBuilder;
  void* storage = std::span(object, 1).last(0).data();
  std::construct_at(static_cast<BuilderBuffer*>(storage));
  Publish(*object, allocation.size);
  return object;
}

auto Heap::IsStringBuilder(const HeapObject& object) noexcept -> bool {
  return object.kind == ObjectKind::StringBuilder;
}

auto Heap::AppendString(HeapObject& builder, std::string_view text) noexcept
    -> std::expected<void, StringBuilderError> {
  if (!IsStringBuilder(builder)) {
    return std::unexpected(StringBuilderError::NotBuilder);
  }
  const auto length = builder.length;
  if (text.size() > kMaximumStringLength - length) {
    return std::unexpected(StringBuilderError::OutOfMemory);
  }
  const auto new_length = length + text.size();
  auto& buffer = Buffer(builder);
  if (new_length > buffer.capacity) {
    const auto capacity = std::max(
        {new_length, kInitialBuilderCapacity, std::min(kMaximumStringLength, buffer.capacity * 2)});
    MaybeCollect(capacity);
    auto* memory = static_cast<char*>(::operator new(capacity, std::nothrow));
    if (memory == nullptr) {
      return std::unexpected(StringBuilderError::OutOfMemory);
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

auto Heap::BuildString(const HeapObject& builder) noexcept -> StringObject* {
  const auto& buffer = Buffer(builder);
  return AllocateString(std::string_view(buffer.data, builder.length));
}

void Heap::ReleaseStringBuilder(HeapObject& object) noexcept {
  auto& buffer = Buffer(object);
  allocated_bytes_ -= buffer.capacity;
  committed_bytes_ -= buffer.capacity;
  ::operator delete(buffer.data);
  std::destroy_at(&buffer);
}

}  // namespace vela::runtime
