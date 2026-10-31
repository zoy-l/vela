#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <utility>

#include "runtime/value/value.h"

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
  [[nodiscard]] T Read() const noexcept {
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
  RegisterReference& operator=(RegisterReference&& value) noexcept {
    operator=(static_cast<const RegisterReference&>(value));
    return *this;
  }
  RegisterReference& operator=(const RegisterReference& value) noexcept {
    if (this != &value) {
      *payload_ = *value.payload_;
      *kind_ = *value.kind_;
    }
    return *this;
  }
  void SetNull() noexcept { *kind_ = ValueKind::Null; }
  void SetBoolean(bool value) noexcept {
    payload_->Store(value);
    *kind_ = ValueKind::Boolean;
  }
  void SetInteger(std::int64_t value) noexcept {
    payload_->Store(value);
    *kind_ = ValueKind::Integer;
  }
  void SetDouble(double value) noexcept {
    payload_->Store(value);
    *kind_ = ValueKind::Double;
  }
  void SetObject(HeapObject* value) noexcept {
    payload_->Store(value);
    *kind_ = ValueKind::Object;
  }
  void SetString(StringObject* value) noexcept {
    payload_->Store(value);
    *kind_ = ValueKind::String;
  }
  RegisterReference& operator=(Value value) noexcept {
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
  [[nodiscard]] ValueKind kind() const noexcept { return *kind_; }
  [[nodiscard]] bool IsNull() const noexcept { return *kind_ == ValueKind::Null; }
  [[nodiscard]] bool IsBoolean() const noexcept { return *kind_ == ValueKind::Boolean; }
  [[nodiscard]] bool IsInteger() const noexcept { return *kind_ == ValueKind::Integer; }
  [[nodiscard]] bool IsDouble() const noexcept { return *kind_ == ValueKind::Double; }
  [[nodiscard]] bool IsObject() const noexcept { return *kind_ == ValueKind::Object; }
  [[nodiscard]] bool IsString() const noexcept { return *kind_ == ValueKind::String; }
  [[nodiscard]] bool AsBoolean() const noexcept { return payload_->Read<bool>(); }
  [[nodiscard]] std::int64_t AsInteger() const noexcept { return payload_->Read<std::int64_t>(); }
  [[nodiscard]] double AsDouble() const noexcept { return payload_->Read<double>(); }
  [[nodiscard]] HeapObject* AsObject() const noexcept { return payload_->Read<HeapObject*>(); }
  [[nodiscard]] StringObject* AsString() const noexcept { return payload_->Read<StringObject*>(); }
  [[nodiscard]] Value Read() const noexcept {
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
  [[nodiscard]] bool IsEmpty() const noexcept { return payloads_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return payloads_.size(); }
  [[nodiscard]] std::span<RegisterPayload> payloads() const noexcept { return payloads_; }
  [[nodiscard]] std::span<ValueKind> kinds() const noexcept { return kinds_; }
  [[nodiscard]] RegisterReference front() const noexcept {
    return {payloads_.front(), kinds_.front()};
  }
  [[nodiscard]] RegisterView subspan(std::size_t first,
                                     std::size_t count = std::dynamic_extent) const noexcept {
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
