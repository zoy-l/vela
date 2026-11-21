#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <type_traits>

#include "base/execution_status.h"
#include "runtime/native/native_control.h"

namespace vela::runtime {
class Isolate;
class Value;

struct NativeInlineScalarOperand final {
  std::uint64_t bits = 0;
  std::uint32_t capture = std::numeric_limits<std::uint32_t>::max();
  bool from_cell = false;
};

// 低半部保存完整的整数结果，高半部保存状态。编译器按目标平台使用 Clang 的整数 ABI。
using NativeIntegerCallbackResult = unsigned __int128;
inline constexpr auto kIntegerCallbackResultBits = std::numeric_limits<std::uint64_t>::digits;
using NativeIntegerCallback =
    NativeIntegerCallbackResult (*)(std::int64_t argument,
                                    const NativeControlState* control) noexcept;
}  // namespace vela::runtime

namespace vela::internal {
// 入口地址借用函数值所保活的映像，控制状态借用同一 Isolate。
struct BoundScalarCallback final {
  runtime::NativeIntegerCallback entry = nullptr;
  const runtime::NativeControlState* control = nullptr;
  // 借用不可变的映像字段，或闭包保活且不会迁移的内联字段。
  const std::byte* operand = nullptr;
  const std::atomic<std::uint8_t>* requests = nullptr;
  std::uintptr_t stack_limit = 0;

  [[nodiscard]] static BoundScalarCallback Bind(runtime::Isolate& isolate,
                                                runtime::Value callable) noexcept;
  template <class T>
  [[nodiscard]] std::optional<T> Try(T argument) const noexcept {
    if (operand != nullptr) [[likely]] {
      std::byte stack_slot;
      const auto position = std::bit_cast<std::uintptr_t>(&stack_slot);
      const auto pending = requests->load(std::memory_order_relaxed);
      const auto active = std::bit_cast<std::uintptr_t>(control->host_execution);
      if ((active | static_cast<std::uintptr_t>(pending)) != 0 || position <= stack_limit) {
        return std::nullopt;
      }
      std::uint64_t bits;
      std::memcpy(&bits, operand, sizeof(bits));
      if constexpr (std::is_same_v<T, std::int64_t>) {
        std::int64_t result;
        if (__builtin_add_overflow(argument, std::bit_cast<std::int64_t>(bits), &result)) {
          return std::nullopt;
        }
        return result;
      } else if constexpr (std::is_same_v<T, double>) {
        return argument + std::bit_cast<double>(bits);
      } else {
        static_assert(std::is_same_v<T, bool>);
        return argument != (bits != 0);
      }
    }
    if constexpr (std::is_same_v<T, std::int64_t>) {
      if (entry != nullptr) {
        const auto result = entry(argument, control);
        if ((result >> runtime::kIntegerCallbackResultBits) ==
            static_cast<std::uint8_t>(ExecutionStatus::Returned)) [[likely]] {
          return static_cast<std::int64_t>(static_cast<std::uint64_t>(result));
        }
      }
    }
    return std::nullopt;
  }
};
}  // namespace vela::internal
