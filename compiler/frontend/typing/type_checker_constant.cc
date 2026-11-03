#include "compiler/frontend/syntax/token.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/frontend/typing/type_checker.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "base/integer_arithmetic.h"
#include "base/opcode.h"
#include "compiler/frontend/syntax/ast.h"

namespace vela::frontend {

std::string TypeChecker::ConstantKey(const ConstantValue& value) {
  return std::visit(
      [](const auto& item) -> std::string {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::string>) {
          return item;
        } else {
          return std::to_string(item);
        }
      },
      value);
}

namespace {

std::optional<ConstantValue> IntegerConstant(TokenKind operation,
                                             std::int64_t left,
                                             std::int64_t right) {
  constexpr std::array<std::pair<TokenKind, Opcode>, 5> kArithmetic = {
      {
          {TokenKind::Plus, Opcode::AddInt},
          {TokenKind::Minus, Opcode::SubInt},
          {TokenKind::Star, Opcode::MulInt},
          {TokenKind::Slash, Opcode::DivInt},
          {TokenKind::Percent, Opcode::ModInt},
      },
  };
  for (const auto& [symbol, opcode] : kArithmetic) {
    if (operation == symbol) {
      const auto result = EvaluateIntegerArithmetic(opcode, left, right);
      return result ? std::optional<ConstantValue>(*result) : std::nullopt;
    }
  }
  if (operation == TokenKind::Less) {
    return left < right;
  }
  if (operation == TokenKind::LessEqual) {
    return left <= right;
  }
  if (operation == TokenKind::Greater) {
    return left > right;
  }
  if (operation == TokenKind::GreaterEqual) {
    return left >= right;
  }
  return std::nullopt;
}

}  // namespace

std::optional<ConstantValue> TypeChecker::EvaluateUnaryConstant(
    const Expression& expression) const {
  const auto operand = EvaluateConstant(*expression.operand);
  if (!operand) {
    return std::nullopt;
  }
  if (expression.operation == TokenKind::Bang && std::holds_alternative<bool>(*operand)) {
    return !std::get<bool>(*operand);
  }
  if (expression.operation == TokenKind::Minus && std::holds_alternative<std::int64_t>(*operand)) {
    const auto result =
        EvaluateIntegerArithmetic(Opcode::SubInt, 0, std::get<std::int64_t>(*operand));
    return result ? std::optional<ConstantValue>(*result) : std::nullopt;
  }
  return std::nullopt;
}

std::optional<ConstantValue> TypeChecker::EvaluateBinaryConstant(
    const Expression& expression) const {
  const auto left = EvaluateConstant(*expression.left);
  const auto right = EvaluateConstant(*expression.right);
  if (!left || !right || left->index() != right->index()) {
    return std::nullopt;
  }
  if (expression.operation == TokenKind::EqualEqual) {
    return *left == *right;
  }
  if (expression.operation == TokenKind::BangEqual) {
    return *left != *right;
  }
  if (std::holds_alternative<bool>(*left)) {
    if (expression.operation == TokenKind::AndAnd) {
      return std::get<bool>(*left) && std::get<bool>(*right);
    }
    if (expression.operation == TokenKind::OrOr) {
      return std::get<bool>(*left) || std::get<bool>(*right);
    }
  }
  if (std::holds_alternative<std::string>(*left) && expression.operation == TokenKind::Plus) {
    return std::get<std::string>(*left) + std::get<std::string>(*right);
  }
  if (std::holds_alternative<std::int64_t>(*left)) {
    return IntegerConstant(expression.operation, std::get<std::int64_t>(*left),
                           std::get<std::int64_t>(*right));
  }
  return std::nullopt;
}

std::optional<ConstantValue> TypeChecker::EvaluateConstant(const Expression& expression) const {
  using Kind = Expression::Kind;
  switch (expression.kind) {
    case Kind::IntegerLiteral: {
      const auto value = expression.IntegerValue();
      return value ? std::optional<ConstantValue>(*value) : std::nullopt;
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
        const auto type = types_.find(expression.object->text);
        const auto name = type == types_.end() ? expression.object->text : type->second.name;
        const auto found = enum_values_.find(std::string(name) + "." + expression.text);
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
