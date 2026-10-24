#include "compiler/frontend/type_checker.h"

#include <charconv>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>

#include "compiler/frontend/ast.h"

namespace vela::frontend {
namespace {

auto IntegerConstant(std::string_view operation, std::int64_t left, std::int64_t right)
    -> std::optional<ConstantValue> {
  std::int64_t result = 0;
  if (operation == "+" && !__builtin_add_overflow(left, right, &result)) {
    return result;
  }
  if (operation == "-" && !__builtin_sub_overflow(left, right, &result)) {
    return result;
  }
  if (operation == "*" && !__builtin_mul_overflow(left, right, &result)) {
    return result;
  }
  if (operation == "/" && right != 0 &&
      (left != std::numeric_limits<std::int64_t>::min() || right != -1)) {
    return left / right;
  }
  if (operation == "<") {
    return left < right;
  }
  if (operation == "<=") {
    return left <= right;
  }
  if (operation == ">") {
    return left > right;
  }
  if (operation == ">=") {
    return left >= right;
  }
  return std::nullopt;
}

}  // namespace

auto TypeChecker::EvaluateUnaryConstant(const Expression& expression) const
    -> std::optional<ConstantValue> {
  const auto operand = EvaluateConstant(*expression.operand);
  if (!operand) {
    return std::nullopt;
  }
  if (expression.text == "!" && std::holds_alternative<bool>(*operand)) {
    return !std::get<bool>(*operand);
  }
  if (expression.text == "-" && std::holds_alternative<std::int64_t>(*operand)) {
    const auto integer = std::get<std::int64_t>(*operand);
    if (integer != std::numeric_limits<std::int64_t>::min()) {
      return -integer;
    }
  }
  return std::nullopt;
}

auto TypeChecker::EvaluateBinaryConstant(const Expression& expression) const
    -> std::optional<ConstantValue> {
  const auto left = EvaluateConstant(*expression.left);
  const auto right = EvaluateConstant(*expression.right);
  if (!left || !right || left->index() != right->index()) {
    return std::nullopt;
  }
  if (expression.text == "==") {
    return *left == *right;
  }
  if (expression.text == "!=") {
    return *left != *right;
  }
  if (std::holds_alternative<bool>(*left)) {
    if (expression.text == "&&") {
      return std::get<bool>(*left) && std::get<bool>(*right);
    }
    if (expression.text == "||") {
      return std::get<bool>(*left) || std::get<bool>(*right);
    }
  }
  if (std::holds_alternative<std::string>(*left) && expression.text == "+") {
    return std::get<std::string>(*left) + std::get<std::string>(*right);
  }
  if (std::holds_alternative<std::int64_t>(*left)) {
    return IntegerConstant(expression.text, std::get<std::int64_t>(*left),
                           std::get<std::int64_t>(*right));
  }
  return std::nullopt;
}

auto TypeChecker::EvaluateConstant(const Expression& expression) const
    -> std::optional<ConstantValue> {
  using Kind = Expression::Kind;
  switch (expression.kind) {
    case Kind::IntegerLiteral: {
      std::int64_t value = 0;
      const auto parsed =
          std::from_chars(expression.text.data(), std::to_address(expression.text.end()), value);
      return parsed.ec == std::errc() ? std::optional<ConstantValue>(value) : std::nullopt;
    }
    case Kind::BooleanLiteral:
      return expression.boolean_value;
    case Kind::StringLiteral:
      return expression.text;
    case Kind::Unary:
      return EvaluateUnaryConstant(expression);
    case Kind::Binary:
      return EvaluateBinaryConstant(expression);
    case Kind::Name: {
      const auto* binding = LookupLocal(expression.text);
      if (binding != nullptr) {
        const auto found = constant_bindings_.find(binding->id);
        if (found != constant_bindings_.end()) {
          return found->second;
        }
      }
      return std::nullopt;
    }
    case Kind::Member:
      if (expression.object->kind == Kind::Name &&
          LookupLocal(expression.object->text) == nullptr) {
        const auto found = enum_values_.find(expression.object->text + "." + expression.text);
        if (found != enum_values_.end()) {
          return found->second;
        }
      }
      return std::nullopt;
    default:
      return std::nullopt;
  }
}

}  // namespace vela::frontend
