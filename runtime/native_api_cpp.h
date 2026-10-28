#pragma once

#include <bit>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

#include "runtime/native_api.h"

namespace vela::runtime::aot {

struct HandleRelease final {
  void operator()(VelaAotHandle* value) const noexcept { vela_aot_v1_handle_release(value); }
};

// Tag 来自生成的签名；句柄可移动，借用 get() 不转移所有权。
template <class Tag>
class Handle final {
 public:
  Handle() = default;
  [[nodiscard]] static Handle Adopt(VelaAotHandle* value) { return Handle(value); }
  [[nodiscard]] VelaAotHandle* get() const noexcept { return value_.get(); }
  [[nodiscard]] VelaAotHandle* Release() noexcept { return value_.release(); }

 private:
  explicit Handle(VelaAotHandle* value) : value_(value) {}
  std::unique_ptr<VelaAotHandle, HandleRelease> value_;
};

struct AnyTag final {};
struct StringTag final {};
using String = Handle<StringTag>;

struct Error final {
  VelaAotStatus status = VELA_AOT_RETURNED;
  Handle<AnyTag> detail;

  [[nodiscard]] std::string_view code() const noexcept { return vela_aot_v1_status_code(status); }
  [[nodiscard]] std::string_view message() const noexcept {
    const auto* handle = detail.get();
    if (handle == nullptr) {
      return {};
    }
    const char* data = nullptr;
    std::uint64_t size = 0;
    const auto read = vela_aot_v1_error_message(handle, &data, &size);
    return read == VELA_AOT_RETURNED ? std::string_view(data, size) : std::string_view();
  }
};

template <class T>
using Result = std::expected<T, Error>;

[[nodiscard]] inline std::unexpected<Error> Failure(VelaAotStatus status, VelaAotHandle* error) {
  return std::unexpected(Error{.status = status, .detail = Handle<AnyTag>::Adopt(error)});
}

[[nodiscard]] inline VelaAotValue Encode(std::int64_t value) {
  return {.payload = std::bit_cast<std::uint64_t>(value), .kind = VELA_AOT_INTEGER};
}
[[nodiscard]] inline VelaAotValue Encode(double value) {
  return {.payload = std::bit_cast<std::uint64_t>(value), .kind = VELA_AOT_DOUBLE};
}
[[nodiscard]] inline VelaAotValue Encode(bool value) {
  return {.payload = value ? 1U : 0U, .kind = VELA_AOT_BOOLEAN};
}
template <class T>
[[nodiscard]] VelaAotValue Encode(std::optional<T> value) {
  return value ? Encode(*value) : VelaAotValue{};
}

template <class T>
[[nodiscard]] std::optional<T> DecodeNullable(VelaAotValue value) {
  if (value.kind == VELA_AOT_NULL) {
    return std::nullopt;
  }
  if constexpr (std::is_same_v<T, bool>) {
    return value.payload != 0;
  } else {
    return std::bit_cast<T>(value.payload);
  }
}

[[nodiscard]] inline VelaAotValue EncodeHandle(VelaAotHandle* handle) {
  return {
      .payload = std::bit_cast<std::uint64_t>(handle),
      .kind = static_cast<std::uint8_t>(handle != nullptr ? VELA_AOT_HANDLE : VELA_AOT_NULL),
  };
}

class Value final {
 public:
  Value() = default;
  explicit Value(std::int64_t value) : value_(Encode(value)) {}
  explicit Value(double value) : value_(Encode(value)) {}
  explicit Value(bool value) : value_(Encode(value)) {}
  template <class Tag>
  explicit Value(Handle<Tag> handle)
      : value_(EncodeHandle(handle.get())), handle_(Handle<AnyTag>::Adopt(handle.Release())) {}
  Value(Value&& other) noexcept
      : value_(std::exchange(other.value_, {})), handle_(std::move(other.handle_)) {}
  Value& operator=(Value&& other) noexcept {
    if (this != &other) {
      value_ = std::exchange(other.value_, {});
      handle_ = std::move(other.handle_);
    }
    return *this;
  }
  Value(const Value&) = delete;
  Value& operator=(const Value&) = delete;
  ~Value() = default;

  [[nodiscard]] static Value Adopt(VelaAotValue value) {
    Value result;
    result.value_ = value;
    if (value.kind == VELA_AOT_HANDLE) {
      result.handle_ = Handle<AnyTag>::Adopt(std::bit_cast<VelaAotHandle*>(value.payload));
    }
    return result;
  }
  [[nodiscard]] VelaAotValue raw() const noexcept { return value_; }

 private:
  VelaAotValue value_{};
  Handle<AnyTag> handle_;
};

struct InstanceRelease final {
  void operator()(VelaAotInstance* value) const noexcept { vela_aot_v1_instance_release(value); }
};
using Instance = std::unique_ptr<VelaAotInstance, InstanceRelease>;

[[nodiscard]] inline Result<String> CreateString(VelaAotIsolate* isolate, std::string_view text) {
  VelaAotHandle* output = nullptr;
  const auto status = vela_aot_v1_string_create(isolate, text.data(), text.size(), &output);
  return status == VELA_AOT_RETURNED ? Result<String>(String::Adopt(output))
                                     : Failure(status, nullptr);
}

enum class WaitMode : std::uint8_t { AvailableWork, WaitForHost };

template <class Tag>
[[nodiscard]] Result<Value> Wait(const Handle<Tag>& future,
                                 WaitMode mode = WaitMode::AvailableWork) {
  VelaAotValue output{};
  VelaAotHandle* error = nullptr;
  const auto status =
      vela_aot_v1_future_wait(future.get(), static_cast<std::uint8_t>(mode), &output, &error);
  return status == VELA_AOT_RETURNED ? Result<Value>(Value::Adopt(output)) : Failure(status, error);
}

}  // namespace vela::runtime::aot
