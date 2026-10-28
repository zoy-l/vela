#pragma once

#include <cstdint>
#include <expected>
#include <limits>
#include <utility>

#include "base/execution_status.h"
#include "base/opcode.h"
#include "runtime/value.h"

namespace vela::runtime::internal {

// VM 热分派和动态数值操作共用叶实现；内联使 VM 保留同一优化边界。
inline constexpr std::uint64_t kIntShiftMask = std::numeric_limits<std::uint64_t>::digits - 1;

struct NumericOperands final {
  Value left;
  Value right;
};

[[clang::always_inline]] inline std::expected<Value, ExecutionStatus>
IntegerArithmetic(Opcode opcode, std::int64_t left, std::int64_t right) noexcept {
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
      if (right == 0) {
        return std::unexpected(ExecutionStatus::DivisionByZero);
      }
      overflow = left == std::numeric_limits<std::int64_t>::min() && right == -1;
      if (!overflow) {
        result = left / right;
      }
      break;
    case Opcode::ModInt:
      if (right == 0) {
        return std::unexpected(ExecutionStatus::DivisionByZero);
      }
      overflow = left == std::numeric_limits<std::int64_t>::min() && right == -1;
      if (!overflow) {
        result = left % right;
      }
      break;
    case Opcode::BitAndInt:
      result = left & right;
      break;
    case Opcode::BitOrInt:
      result = left | right;
      break;
    case Opcode::BitXorInt:
      result = left ^ right;
      break;
    case Opcode::ShiftLeftInt:
      result = static_cast<std::int64_t>(static_cast<std::uint64_t>(left)
                                         << (static_cast<std::uint64_t>(right) & kIntShiftMask));
      break;
    case Opcode::ShiftRightInt:
      result = left >> (static_cast<std::uint64_t>(right) & kIntShiftMask);
      break;
    case Opcode::ShiftUnsignedRightInt:
      result = static_cast<std::int64_t>(static_cast<std::uint64_t>(left) >>
                                         (static_cast<std::uint64_t>(right) & kIntShiftMask));
      break;
    default:
      std::unreachable();
  }
  if (overflow) {
    return std::unexpected(ExecutionStatus::IntegerOverflow);
  }
  return Value::Integer(result);
}

[[clang::always_inline]] inline std::expected<Value, ExecutionStatus> IntegerOperation(
    Opcode opcode,
    NumericOperands operands) noexcept {
  const auto& [left_value, right_value] = operands;
  if (!left_value.IsInteger() || !right_value.IsInteger()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const auto left = left_value.AsInteger();
  const auto right = right_value.AsInteger();
  bool result = false;
  switch (opcode) {
    case Opcode::EqualInt:
      result = left == right;
      break;
    case Opcode::NotEqualInt:
      result = left != right;
      break;
    case Opcode::LessInt:
      result = left < right;
      break;
    case Opcode::LessEqualInt:
      result = left <= right;
      break;
    case Opcode::GreaterInt:
      result = left > right;
      break;
    case Opcode::GreaterEqualInt:
      result = left >= right;
      break;
    default:
      return IntegerArithmetic(opcode, left, right);
  }
  return Value::Boolean(result);
}

[[clang::always_inline]] inline std::expected<Value, ExecutionStatus> DoubleOperation(
    Opcode opcode,
    NumericOperands operands) noexcept {
  const auto& [left_value, right_value] = operands;
  if (!left_value.IsDouble() || !right_value.IsDouble()) {
    return std::unexpected(ExecutionStatus::TypeError);
  }
  const double left = left_value.AsDouble();
  const double right = right_value.AsDouble();
  switch (opcode) {
    case Opcode::AddDouble:
      return Value::Double(left + right);
    case Opcode::SubDouble:
      return Value::Double(left - right);
    case Opcode::MulDouble:
      return Value::Double(left * right);
    case Opcode::DivDouble:
      if (right == 0.0) {
        return std::unexpected(ExecutionStatus::DivisionByZero);
      }
      return Value::Double(left / right);
    case Opcode::EqualDouble:
      return Value::Boolean(left == right);
    case Opcode::NotEqualDouble:
      return Value::Boolean(left != right);
    case Opcode::LessDouble:
      return Value::Boolean(left < right);
    case Opcode::LessEqualDouble:
      return Value::Boolean(left <= right);
    case Opcode::GreaterDouble:
      return Value::Boolean(left > right);
    case Opcode::GreaterEqualDouble:
      return Value::Boolean(left >= right);
    default:
      std::unreachable();
  }
}

}  // namespace vela::runtime::internal
