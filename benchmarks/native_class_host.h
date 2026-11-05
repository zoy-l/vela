#pragma once

#include <cstdint>
#include <expected>
#include <memory>

#include "runtime/native/native_context.h"
#include "vela/bindings.h"
#include "vela/error.h"

namespace vela::benchmarks {

class VELA_CLASS("benchmark.Host") NativeClassHost final {
 public:
  using Reentry = std::int64_t (*)(void*, std::int64_t) noexcept;
  struct Counts final {
    std::uint64_t constructed;
    std::uint64_t destroyed;
  };

  explicit NativeClassHost(std::int64_t initial) noexcept;
  ~NativeClassHost() noexcept;
  NativeClassHost(const NativeClassHost&) = delete;
  NativeClassHost& operator=(const NativeClassHost&) = delete;
  NativeClassHost(NativeClassHost&&) = delete;
  NativeClassHost& operator=(NativeClassHost&&) = delete;

  void Empty() const noexcept;
  [[nodiscard]] std::int64_t Integer(std::int64_t value) const noexcept;
  [[nodiscard]] double Floating(double value) const noexcept;
  [[nodiscard]] bool Boolean(bool value) const noexcept;
  [[nodiscard]] std::int64_t Nullable(const NativeClassHost* other) const noexcept;
  void Write(std::int64_t value) noexcept;
  VELA_GETTER("value")
  [[nodiscard]] std::int64_t Value() const noexcept;
  VELA_SETTER("value")
  void SetValue(std::int64_t value) noexcept;
  [[nodiscard]] static std::int64_t Static(std::int64_t value) noexcept;
  [[nodiscard]] static std::unique_ptr<NativeClassHost> Unique(std::int64_t initial) noexcept;
  [[nodiscard]] static std::shared_ptr<NativeClassHost> Shared(std::int64_t initial) noexcept;
  [[nodiscard]] static std::shared_ptr<NativeClassHost> Repeated() noexcept;
  [[nodiscard]] std::expected<std::int64_t, vela::Error> Failure(bool fail) const noexcept;
  std::int64_t Collect(runtime::NativeContext& context, std::int64_t delta) const noexcept;
  [[nodiscard]] std::int64_t Reenter(std::int64_t value) const noexcept;

  VELA_IGNORE
  void SetReentry(Reentry callback, void* state) noexcept;
  VELA_IGNORE
  [[nodiscard]] static Counts AllocationCounts() noexcept;

 private:
  std::int64_t value_;
  Reentry reentry_ = nullptr;
  void* reentry_state_ = nullptr;
  inline static Counts counts_{};
};

// 原始指针基线和等价手写适配入口保留独立调用边界，用于解释包装读取以外的成本。
std::int64_t NativeClassIntegerPointer(const NativeClassHost* object, std::int64_t value) noexcept;
std::int64_t NativeClassIntegerAdapter(const NativeClassHost& object, std::int64_t value) noexcept;

}  // namespace vela::benchmarks
