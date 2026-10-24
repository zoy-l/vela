#pragma once

#include <bit>
#include <cstdint>
#include <type_traits>

namespace vela::runtime {

struct HeapObject;

enum class ValueKind : std::uint8_t {
  Null,
  Boolean,
  Integer,
  Float,
  Object,
};

class Value final {
 public:
  constexpr Value() noexcept
      : payload_(0), metadata_(static_cast<std::uint64_t>(ValueKind::Null)) {}

  [[nodiscard]] static constexpr Value Null() noexcept { return Value(); }

  [[nodiscard]] static constexpr Value Boolean(bool value) noexcept {
    return Value(static_cast<std::uint64_t>(value), ValueKind::Boolean);
  }

  [[nodiscard]] static constexpr Value Integer(std::int64_t value) noexcept {
    return Value(static_cast<std::uint64_t>(value), ValueKind::Integer);
  }

  [[nodiscard]] static Value Float(double value) noexcept {
    return Value(std::bit_cast<std::uint64_t>(value), ValueKind::Float);
  }

  [[nodiscard]] static Value Object(HeapObject* object) noexcept {
    return Value(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(object)),
                 ValueKind::Object);
  }

  [[nodiscard]] constexpr ValueKind kind() const noexcept {
    return static_cast<ValueKind>(metadata_);
  }
  [[nodiscard]] constexpr bool IsNull() const noexcept { return kind() == ValueKind::Null; }
  [[nodiscard]] constexpr bool IsBoolean() const noexcept { return kind() == ValueKind::Boolean; }
  [[nodiscard]] constexpr bool IsInteger() const noexcept { return kind() == ValueKind::Integer; }
  [[nodiscard]] constexpr bool IsFloat() const noexcept { return kind() == ValueKind::Float; }
  [[nodiscard]] constexpr bool IsObject() const noexcept { return kind() == ValueKind::Object; }

  [[nodiscard]] constexpr bool AsBoolean() const noexcept { return payload_ != 0; }

  [[nodiscard]] constexpr std::int64_t AsInteger() const noexcept {
    return static_cast<std::int64_t>(payload_);
  }

  [[nodiscard]] double AsFloat() const noexcept { return std::bit_cast<double>(payload_); }

  [[nodiscard]] HeapObject* AsObject() const noexcept {
    return reinterpret_cast<HeapObject*>(payload_);
  }

 private:
  constexpr Value(std::uint64_t payload, ValueKind kind) noexcept
      : payload_(payload), metadata_(static_cast<std::uint64_t>(kind)) {}

  std::uint64_t payload_;
  std::uint64_t metadata_;
};

static_assert(std::is_trivially_copyable_v<Value>);
static_assert(sizeof(Value) == 16);

}  // namespace vela::runtime
