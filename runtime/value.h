#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <variant>

namespace vela::runtime {

struct HeapObject;
struct StringObject;

enum class ValueKind : std::uint8_t {
  Null,
  Boolean,
  Integer,
  Float,
  Object,
  String,
};

class Value final {
 public:
  constexpr Value() noexcept = default;

  [[nodiscard]] static constexpr auto Null() noexcept -> Value { return {}; }
  [[nodiscard]] static constexpr auto Boolean(bool value) noexcept -> Value { return {value}; }
  [[nodiscard]] static constexpr auto Integer(std::int64_t value) noexcept -> Value {
    return {value};
  }
  [[nodiscard]] static auto Float(double value) noexcept -> Value { return {value}; }
  [[nodiscard]] static auto Object(HeapObject* object) noexcept -> Value { return {object}; }
  [[nodiscard]] static auto String(StringObject* value) noexcept -> Value { return {value}; }

  [[nodiscard]] constexpr auto kind() const noexcept -> ValueKind {
    return static_cast<ValueKind>(payload_.index());
  }
  [[nodiscard]] constexpr auto IsNull() const noexcept -> bool { return kind() == ValueKind::Null; }
  [[nodiscard]] constexpr auto IsBoolean() const noexcept -> bool {
    return kind() == ValueKind::Boolean;
  }
  [[nodiscard]] constexpr auto IsInteger() const noexcept -> bool {
    return kind() == ValueKind::Integer;
  }
  [[nodiscard]] constexpr auto IsFloat() const noexcept -> bool {
    return kind() == ValueKind::Float;
  }
  [[nodiscard]] constexpr auto IsObject() const noexcept -> bool {
    return kind() == ValueKind::Object;
  }
  [[nodiscard]] constexpr auto IsString() const noexcept -> bool {
    return kind() == ValueKind::String;
  }

  [[nodiscard]] constexpr auto AsBoolean() const noexcept -> bool {
    return std::get<bool>(payload_);
  }
  [[nodiscard]] constexpr auto AsInteger() const noexcept -> std::int64_t {
    return std::get<std::int64_t>(payload_);
  }
  [[nodiscard]] auto AsFloat() const noexcept -> double { return std::get<double>(payload_); }
  [[nodiscard]] auto AsObject() const noexcept -> HeapObject* {
    return std::get<HeapObject*>(payload_);
  }
  [[nodiscard]] auto AsString() const noexcept -> StringObject* {
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

}  // namespace vela::runtime
