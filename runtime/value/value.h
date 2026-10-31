#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace vela::runtime {

struct HeapObject;
class StringObject;

enum class ValueKind : std::uint8_t {
  Null,
  Boolean,
  Integer,
  Double,
  Object,
  String,
};

class Value final {
 public:
  constexpr Value() noexcept = default;

  [[nodiscard]] static constexpr Value Null() noexcept { return {}; }
  [[nodiscard]] static constexpr Value Boolean(bool value) noexcept {
    return Create(static_cast<std::uint64_t>(value), ValueKind::Boolean);
  }
  [[nodiscard]] static constexpr Value Integer(std::int64_t value) noexcept {
    return Create(value, ValueKind::Integer);
  }
  [[nodiscard]] static Value Double(double value) noexcept {
    return Create(value, ValueKind::Double);
  }
  [[nodiscard]] static Value Object(HeapObject* object) noexcept {
    return Create(object, ValueKind::Object);
  }
  [[nodiscard]] static Value String(StringObject* value) noexcept {
    return Create(value, ValueKind::String);
  }

  [[nodiscard]] constexpr ValueKind kind() const noexcept { return kind_; }
  [[nodiscard]] constexpr bool IsNull() const noexcept { return kind() == ValueKind::Null; }
  [[nodiscard]] constexpr bool IsBoolean() const noexcept { return kind() == ValueKind::Boolean; }
  [[nodiscard]] constexpr bool IsInteger() const noexcept { return kind() == ValueKind::Integer; }
  [[nodiscard]] constexpr bool IsDouble() const noexcept { return kind() == ValueKind::Double; }
  [[nodiscard]] constexpr bool IsObject() const noexcept { return kind() == ValueKind::Object; }
  [[nodiscard]] constexpr bool IsString() const noexcept { return kind() == ValueKind::String; }
  [[nodiscard]] constexpr bool IsReference() const noexcept { return IsObject() || IsString(); }

  [[nodiscard]] constexpr bool AsBoolean() const noexcept {
    return std::bit_cast<std::uint64_t>(payload_) != 0;
  }
  [[nodiscard]] constexpr std::int64_t AsInteger() const noexcept {
    return std::bit_cast<std::int64_t>(payload_);
  }
  [[nodiscard]] double AsDouble() const noexcept { return std::bit_cast<double>(payload_); }
  [[nodiscard]] HeapObject* AsObject() const noexcept {
    return std::bit_cast<HeapObject*>(payload_);
  }
  [[nodiscard]] StringObject* AsString() const noexcept {
    return std::bit_cast<StringObject*>(payload_);
  }

 private:
  friend struct NativeMemoryLayout;

  template <class T>
  static constexpr Value Create(T value, ValueKind kind) noexcept {
    Value result;
    result.payload_ = std::bit_cast<decltype(payload_)>(value);
    result.kind_ = kind;
    return result;
  }

  // 显式的载荷与标签布局供 VM、GC 和 AOT 共用，不依赖标准库 variant 的私有表示。
  // 以对象表示保存各类型，避免读取 inactive union 成员或指针经整数往返。
  alignas(std::uint64_t) std::array<std::byte, sizeof(std::uint64_t)> payload_ = {};
  ValueKind kind_ = ValueKind::Null;
};

inline constexpr std::size_t kValueSlotBytes = 16;
static_assert(std::is_trivially_copyable_v<Value>);
static_assert(std::is_standard_layout_v<Value>);
static_assert(sizeof(Value) == kValueSlotBytes);
static_assert(sizeof(double) == sizeof(std::uint64_t));
static_assert(std::numeric_limits<double>::is_iec559);

}  // namespace vela::runtime
