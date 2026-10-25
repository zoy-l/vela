#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <variant>

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
  [[nodiscard]] static constexpr Value Boolean(bool value) noexcept { return {value}; }
  [[nodiscard]] static constexpr Value Integer(std::int64_t value) noexcept { return {value}; }
  [[nodiscard]] static Value Double(double value) noexcept { return {value}; }
  [[nodiscard]] static Value Object(HeapObject* object) noexcept { return {object}; }
  [[nodiscard]] static Value String(StringObject* value) noexcept { return {value}; }

  [[nodiscard]] constexpr ValueKind kind() const noexcept {
    return static_cast<ValueKind>(payload_.index());
  }
  [[nodiscard]] constexpr bool IsNull() const noexcept { return kind() == ValueKind::Null; }
  [[nodiscard]] constexpr bool IsBoolean() const noexcept { return kind() == ValueKind::Boolean; }
  [[nodiscard]] constexpr bool IsInteger() const noexcept { return kind() == ValueKind::Integer; }
  [[nodiscard]] constexpr bool IsDouble() const noexcept { return kind() == ValueKind::Double; }
  [[nodiscard]] constexpr bool IsObject() const noexcept { return kind() == ValueKind::Object; }
  [[nodiscard]] constexpr bool IsString() const noexcept { return kind() == ValueKind::String; }

  [[nodiscard]] constexpr bool AsBoolean() const noexcept { return std::get<bool>(payload_); }
  [[nodiscard]] constexpr std::int64_t AsInteger() const noexcept {
    return std::get<std::int64_t>(payload_);
  }
  [[nodiscard]] double AsDouble() const noexcept { return std::get<double>(payload_); }
  [[nodiscard]] HeapObject* AsObject() const noexcept { return std::get<HeapObject*>(payload_); }
  [[nodiscard]] StringObject* AsString() const noexcept {
    return std::get<StringObject*>(payload_);
  }

 private:
  template <class T>
  constexpr Value(T value) noexcept : payload_(value) {}

  // 各分支直接保留原始类型，避免指针经整数往返时丢失优化所需的别名信息。
  std::variant<std::monostate, bool, std::int64_t, double, HeapObject*, StringObject*> payload_;
};

inline constexpr std::size_t kValueSlotBytes = 16;
static_assert(std::is_trivially_copyable_v<Value>);
static_assert(sizeof(Value) == kValueSlotBytes);
static_assert(sizeof(double) == sizeof(std::uint64_t));
static_assert(std::numeric_limits<double>::is_iec559);

}  // namespace vela::runtime
