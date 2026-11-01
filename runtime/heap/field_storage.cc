#include "runtime/heap/field_storage.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "runtime/value/value.h"

namespace vela::runtime {

Value FieldStorage::Load(std::span<const std::byte> storage,
                         std::uint32_t count,
                         std::uint32_t index) noexcept {
  Value value;
  const auto payload = storage.subspan(static_cast<std::size_t>(index) * kPayloadBytes);
  const auto kind = storage.subspan(KindOffset(count, index));
  std::memcpy(value.payload_.data(), payload.data(), kPayloadBytes);
  std::memcpy(&value.kind_, kind.data(), kKindBytes);
  return value;
}

void FieldStorage::Store(std::span<std::byte> storage,
                         std::uint32_t count,
                         std::uint32_t index,
                         Value value) noexcept {
  const auto payload = storage.subspan(static_cast<std::size_t>(index) * kPayloadBytes);
  const auto kind = storage.subspan(KindOffset(count, index));
  std::memcpy(payload.data(), value.payload_.data(), kPayloadBytes);
  std::memcpy(kind.data(), &value.kind_, kKindBytes);
}

}  // namespace vela::runtime
