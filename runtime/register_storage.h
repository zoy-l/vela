#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <utility>

#include "runtime/value.h"

namespace vela::runtime {

// 连续载荷与紧凑标签分别保存；整数/浮点数不再占用带填充的宿主 Value 槽。
class alignas(std::uint64_t) RegisterPayload final {
 public:
  template <typename T>
  void Store(T value) noexcept {
    static_assert(std::is_trivially_copyable_v<T> && sizeof(T) <= sizeof(bytes_));
    std::memcpy(bytes_.data(), static_cast<const void*>(&value), sizeof(T));
  }
  template <typename T>
  [[nodiscard]] auto Read() const noexcept -> T {
    static_assert(std::is_trivially_copyable_v<T> && sizeof(T) <= sizeof(bytes_));
    T value;
    std::memcpy(static_cast<void*>(&value), bytes_.data(), sizeof(T));
    return value;
  }

 private:
  // 以对象表示保存载荷，避免读取 inactive union 成员；固定尺寸 memcpy 会折叠为普通加载/存储。
  std::array<std::byte, sizeof(std::uint64_t)> bytes_;
};

inline constexpr auto kRegisterSlotBytes = sizeof(RegisterPayload) + sizeof(ValueKind);
static_assert(sizeof(RegisterPayload) == sizeof(std::uint64_t));
static_assert(std::is_trivially_copyable_v<RegisterPayload>);

// 借用单个槽位，不拥有载荷；赋值写回原槽，不能把引用改绑到其他槽。
class RegisterReference final {
 public:
  RegisterReference(RegisterPayload& payload, ValueKind& kind) noexcept
      : payload_(&payload), kind_(&kind) {}
  RegisterReference(const RegisterReference&) noexcept = default;
  RegisterReference(RegisterReference&&) noexcept = default;
  ~RegisterReference() = default;
  auto operator=(RegisterReference&& value) noexcept -> RegisterReference& {
    operator=(static_cast<const RegisterReference&>(value));
    return *this;
  }
  auto operator=(const RegisterReference& value) noexcept -> RegisterReference& {
    if (this != &value) {
      *payload_ = *value.payload_;
      *kind_ = *value.kind_;
    }
    return *this;
  }
  auto operator=(Value value) noexcept -> RegisterReference& {
    const auto kind = value.kind();
    switch (kind) {
      case ValueKind::Null:
        break;
      case ValueKind::Boolean:
        payload_->Store(value.AsBoolean());
        break;
      case ValueKind::Integer:
        payload_->Store(value.AsInteger());
        break;
      case ValueKind::Double:
        payload_->Store(value.AsDouble());
        break;
      case ValueKind::Object:
        payload_->Store(value.AsObject());
        break;
      case ValueKind::String:
        payload_->Store(value.AsString());
        break;
    }
    *kind_ = kind;
    return *this;
  }
  [[nodiscard]] auto kind() const noexcept -> ValueKind { return *kind_; }
  [[nodiscard]] auto IsNull() const noexcept -> bool { return *kind_ == ValueKind::Null; }
  [[nodiscard]] auto IsBoolean() const noexcept -> bool { return *kind_ == ValueKind::Boolean; }
  [[nodiscard]] auto IsInteger() const noexcept -> bool { return *kind_ == ValueKind::Integer; }
  [[nodiscard]] auto IsDouble() const noexcept -> bool { return *kind_ == ValueKind::Double; }
  [[nodiscard]] auto IsObject() const noexcept -> bool { return *kind_ == ValueKind::Object; }
  [[nodiscard]] auto IsString() const noexcept -> bool { return *kind_ == ValueKind::String; }
  [[nodiscard]] auto AsBoolean() const noexcept -> bool { return payload_->Read<bool>(); }
  [[nodiscard]] auto AsInteger() const noexcept -> std::int64_t {
    return payload_->Read<std::int64_t>();
  }
  [[nodiscard]] auto AsDouble() const noexcept -> double { return payload_->Read<double>(); }
  [[nodiscard]] auto AsObject() const noexcept -> HeapObject* {
    return payload_->Read<HeapObject*>();
  }
  [[nodiscard]] auto AsString() const noexcept -> StringObject* {
    return payload_->Read<StringObject*>();
  }
  [[nodiscard]] auto Read() const noexcept -> Value {
    switch (*kind_) {
      case ValueKind::Null:
        return Value::Null();
      case ValueKind::Boolean:
        return Value::Boolean(AsBoolean());
      case ValueKind::Integer:
        return Value::Integer(AsInteger());
      case ValueKind::Double:
        return Value::Double(AsDouble());
      case ValueKind::Object:
        return Value::Object(AsObject());
      case ValueKind::String:
        return Value::String(AsString());
    }
    std::unreachable();
  }
  [[nodiscard]] operator Value() const noexcept { return Read(); }

 private:
  RegisterPayload* payload_;
  ValueKind* kind_;
};

class RegisterView final {
 public:
  RegisterView() = default;
  RegisterView(std::span<RegisterPayload> payloads, std::span<ValueKind> kinds) noexcept
      : payloads_(payloads), kinds_(kinds) {}
  [[nodiscard]] auto IsEmpty() const noexcept -> bool { return payloads_.empty(); }
  [[nodiscard]] auto size() const noexcept -> std::size_t { return payloads_.size(); }
  [[nodiscard]] auto payloads() const noexcept -> std::span<RegisterPayload> { return payloads_; }
  [[nodiscard]] auto kinds() const noexcept -> std::span<ValueKind> { return kinds_; }
  [[nodiscard]] auto front() const noexcept -> RegisterReference {
    return {payloads_.front(), kinds_.front()};
  }
  [[nodiscard]] auto subspan(std::size_t first,
                             std::size_t count = std::dynamic_extent) const noexcept
      -> RegisterView {
    return {payloads_.subspan(first, count), kinds_.subspan(first, count)};
  }
  void CopyFrom(RegisterView source) const noexcept {
    for (std::size_t index = 0; index < source.size(); ++index) {
      payloads_.subspan(index).front() = source.payloads_.subspan(index).front();
      kinds_.subspan(index).front() = source.kinds_.subspan(index).front();
    }
  }
  void CopyTo(std::span<Value> destination) const noexcept {
    for (std::size_t index = 0; index < size(); ++index) {
      destination.subspan(index).front() = subspan(index).front().Read();
    }
  }

 private:
  std::span<RegisterPayload> payloads_;
  std::span<ValueKind> kinds_;
};

}  // namespace vela::runtime
