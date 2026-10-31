#pragma once

#include <cstdint>
#include <expected>
#include <utility>

#include "base/execution_status.h"
#include "base/integer_arithmetic.h"
#include "base/opcode.h"
#include "runtime/value.h"

namespace vela::runtime::internal {

// VM 热分派和动态数值操作共用叶实现；内联使 VM 保留同一优化边界。
struct NumericOperands final {
  Value left;
  Value right;
};

[[clang::always_inline]] inline std::expected<Value, ExecutionStatus>
IntegerArithmetic(Opcode opcode, std::int64_t left, std::int64_t right) noexcept {
  const auto result = EvaluateIntegerArithmetic(opcode, left, right);
  if (!result) {
    return std::unexpected(result.error());
  }
  return Value::Integer(*result);
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
