#pragma once

#include <cstdint>
#include <expected>
#include <limits>
#include <utility>

#include "base/execution_status.h"
#include "base/opcode.h"

namespace vela {

// 编译期求值与运行时共用整数语义；叶函数内联，错误不构造字符串或分配内存。
[[clang::always_inline]] inline std::expected<std::int64_t, ExecutionStatus>
EvaluateIntegerArithmetic(Opcode opcode, std::int64_t left, std::int64_t right) noexcept {
  constexpr std::uint64_t kShiftMask = std::numeric_limits<std::uint64_t>::digits - 1;
  std::int64_t result = 0;
  bool overflow = false;
  switch (opcode) {
    case Opcode::AddInt:
      overflow = __builtin_add_overflow(left, right, &result);
      break;
    case Opcode::SubInt:
      overflow = __builtin_sub_overflow(left, right, &result);
      break;
    case Opcode::MulInt:
      overflow = __builtin_mul_overflow(left, right, &result);
      break;
    case Opcode::DivInt:
    case Opcode::ModInt:
      if (right == 0) {
        return std::unexpected(ExecutionStatus::DivisionByZero);
      }
      overflow = left == std::numeric_limits<std::int64_t>::min() && right == -1;
      if (!overflow) {
        result = opcode == Opcode::DivInt ? left / right : left % right;
      }
      break;
    case Opcode::BitAndInt:
      return left & right;
    case Opcode::BitOrInt:
      return left | right;
    case Opcode::BitXorInt:
      return left ^ right;
    case Opcode::ShiftLeftInt:
      return static_cast<std::int64_t>(static_cast<std::uint64_t>(left)
                                       << (static_cast<std::uint64_t>(right) & kShiftMask));
    case Opcode::ShiftRightInt:
      return left >> (static_cast<std::uint64_t>(right) & kShiftMask);
    case Opcode::ShiftUnsignedRightInt:
      return static_cast<std::int64_t>(static_cast<std::uint64_t>(left) >>
                                       (static_cast<std::uint64_t>(right) & kShiftMask));
    default:
      std::unreachable();
  }
  if (overflow) {
    return std::unexpected(ExecutionStatus::IntegerOverflow);
  }
  return result;
}

}  // namespace vela
