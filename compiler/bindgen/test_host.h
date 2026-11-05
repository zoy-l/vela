#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

#include "runtime/native/native_context.h"
#include "vela/bindings.h"
#include "vela/error.h"
#include "vela/handles.h"
#include "vela/types.h"

namespace vela::binding_fixture {

class Padding {
 public:
  Padding() = default;
  Padding(const Padding&) = delete;
  Padding& operator=(const Padding&) = delete;
  Padding(Padding&&) = delete;
  Padding& operator=(Padding&&) = delete;
  virtual ~Padding() = default;
  [[nodiscard]] std::int64_t PaddingValue() const noexcept { return padding_; }

 private:
  std::int64_t padding_ = 0;
};

class VirtualCounter {
 public:
  VirtualCounter() = default;
  VirtualCounter(const VirtualCounter&) = delete;
  VirtualCounter& operator=(const VirtualCounter&) = delete;
  VirtualCounter(VirtualCounter&&) = delete;
  VirtualCounter& operator=(VirtualCounter&&) = delete;
  virtual ~VirtualCounter() = default;
  virtual std::int64_t Adjust(std::int64_t delta) noexcept = 0;
};

class VELA_CLASS("fixture.Counter") Counter final : public Padding, public VirtualCounter {
 public:
  explicit Counter(std::int64_t initial) noexcept : value_(initial) {}
  VELA_NAME("add")
  std::int64_t Adjust(std::int64_t delta) noexcept override { return value_ += delta; }
  VELA_GETTER("value")
  [[nodiscard]] std::int64_t Value() const noexcept { return value_; }
  VELA_SETTER("value")
  void SetValue(std::int64_t value) noexcept { value_ = value; }
  std::int64_t Collect(runtime::NativeContext& context, std::int64_t delta) noexcept;
  [[nodiscard]] std::string Text(std::string_view text) const noexcept {
    return std::to_string(value_) + ':' + std::string(text);
  }
  [[nodiscard]] bool Equal(const Counter* other) const noexcept {
    return other != nullptr && value_ == other->value_;
  }
  [[nodiscard]] static std::shared_ptr<Counter> Shared(std::int64_t initial) noexcept;
  [[nodiscard]] static std::unique_ptr<Counter> Empty() noexcept { return nullptr; }
  [[nodiscard]] std::expected<std::int64_t, vela::Error> Checked(bool fail) const noexcept;
  [[nodiscard]] static Local<Array<std::int64_t>> Echo(runtime::NativeContext& context,
                                                       Local<Array<std::int64_t>> values) noexcept;
  VELA_IGNORE
  void Debug(int unsupported);

 private:
  std::int64_t value_;
};

}  // namespace vela::binding_fixture
