#include "compiler/frontend/syntax/ast.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <absl/container/flat_hash_map.h>
#include <absl/functional/function_ref.h>

#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/syntax/integer_literal.h"
#include "compiler/frontend/syntax/names.h"
#include "compiler/frontend/typing/type_names.h"

namespace vela::frontend {
namespace {

bool ContainsSuperCall(const Expression* expression) {
  if (expression == nullptr || expression->kind == Expression::Kind::Lambda) {
    return false;
  }
  if (expression->kind == Expression::Kind::Call &&
      expression->callee->kind == Expression::Kind::Name &&
      expression->callee->text == kSuperName) {
    return true;
  }
  bool found = false;
  VisitExpressionChildren(
      *expression, [&](const Expression* child) { found = found || ContainsSuperCall(child); });
  return found;
}

bool ContainsSuperCall(const Statement* statement) {
  if (statement == nullptr) {
    return false;
  }
  bool found = false;
  VisitStatementChildren(
      *statement, [&](const Expression* child) { found = found || ContainsSuperCall(child); },
      [&](const Statement* child) { found = found || ContainsSuperCall(child); });
  return found;
}

}  // namespace

void VisitExpressionChildren(const Expression& expression,
                             absl::FunctionRef<void(const Expression*)> visit) {
  for (const auto* child : ExpressionOperands(expression)) {
    visit(child);
  }
  for (const auto& child : expression.elements) {
    visit(child.get());
  }
  for (const auto& entry : expression.entries) {
    visit(entry.key.get());
    visit(entry.value.get());
  }
  for (const auto& argument : expression.arguments) {
    visit(argument.value.get());
  }
  for (const auto& arm : expression.arms) {
    visit(arm.pattern.get());
    visit(arm.value.get());
  }
}

void VisitStatementChildren(const Statement& statement,
                            absl::FunctionRef<void(const Expression*)> expression,
                            absl::FunctionRef<void(const Statement*)> child) {
  for (const auto* node : {
           statement.expression.get(),
           statement.condition.get(),
           statement.increment.get(),
           statement.iterable.get(),
       }) {
    expression(node);
  }
  for (const auto* node : {
           statement.then_branch.get(),
           statement.else_branch.get(),
           statement.initializer.get(),
           statement.finally_branch.get(),
       }) {
    child(node);
  }
  for (const auto& node : statement.statements) {
    child(node.get());
  }
  for (const auto& branch : statement.cases) {
    expression(branch.label.get());
    child(branch.body.get());
  }
  for (const auto& caught : statement.catches) {
    child(caught.body.get());
  }
}

void VisitFunctionChildren(const Function& function,
                           absl::FunctionRef<void(const Expression*)> expression,
                           absl::FunctionRef<void(const Statement*)> statement) {
  for (const auto& parameter : function.parameters) {
    expression(parameter.initializer.get());
  }
  statement(function.body.get());
}

bool HasSuperConstructorCall(const Statement& body) {
  return ContainsSuperCall(&body);
}

void TypeName::SetSourceId(std::uint32_t source_id) {
  location.source_id = source_id;
  for (auto& argument : arguments) {
    argument.SetSourceId(source_id);
  }
}

ReflectionMemberKind ReflectedMethodKind(const Function& method) {
  if (method.is_getter) {
    return ReflectionMemberKind::Getter;
  }
  return method.is_setter ? ReflectionMemberKind::Setter : ReflectionMemberKind::Method;
}

const std::string& ReflectedMethodName(const Function& method) {
  return method.application ? method.application->name : method.name;
}

std::optional<std::int64_t> Expression::IntegerValue() const noexcept {
  std::string_view digits = text;
  const bool negative = digits.starts_with('-');
  if (negative) {
    digits.remove_prefix(1);
  }
  const auto radix = IntegerLiteralRadix(digits);
  if (radix != IntegerRadix::Decimal) {
    digits.remove_prefix(2);
  }
  std::uint64_t magnitude = 0;
  const auto parsed =
      std::from_chars(std::to_address(digits.begin()), std::to_address(digits.end()), magnitude,
                      static_cast<int>(radix));
  constexpr auto kMaximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  constexpr auto kMinimumMagnitude = kMaximum + 1;
  const auto limit = negative ? kMinimumMagnitude : kMaximum;
  if (parsed.ec != std::errc() || magnitude > limit) {
    return std::nullopt;
  }
  if (negative && magnitude == kMinimumMagnitude) {
    return std::numeric_limits<std::int64_t>::min();
  }
  const auto value = static_cast<std::int64_t>(magnitude);
  return negative ? -value : value;
}

TypeName SubstituteTypeName(TypeName type, const TypeSubstitutions& substitutions) {
  if (type.kind == TypeName::Kind::Parameter) {
    const auto found = substitutions.find(type.parameter);
    return found == substitutions.end() ? type : found->second;
  }
  // 只有由参数替换产生的内层可空才合并；源码直接写出的双重可空仍由类型检查报告。
  const bool substitutes_element = SubstitutesNullableElement(type, substitutions);
  for (auto& argument : type.arguments) {
    argument = SubstituteTypeName(std::move(argument), substitutions);
  }
  if (substitutes_element && IsNullableTypeName(type.arguments.front())) {
    return std::move(type.arguments.front());
  }
  return type;
}

bool SubstitutesNullableElement(const TypeName& type, const TypeSubstitutions& substitutions) {
  return type.kind == TypeName::Kind::Nullable &&
         type.arguments.front().kind == TypeName::Kind::Parameter &&
         substitutions.contains(type.arguments.front().parameter);
}

bool IsNullableTypeName(const TypeName& type) {
  return type.kind == TypeName::Kind::Nullable ||
         (type.kind == TypeName::Kind::Named && type.name == kDynamicTypeName);
}

TypeName ParameterTypeName(const TypeParameter& parameter, SourceLocation location) {
  return {
      .name = parameter.name,
      .location = location,
      .kind = TypeName::Kind::Parameter,
      .arguments = {},
      .parameter = parameter.identity,
  };
}

bool HasTypeParameters(const TypeName& type) {
  return type.kind == TypeName::Kind::Parameter ||
         std::ranges::any_of(type.arguments, HasTypeParameters);
}

bool IsSameTypeConstructor(const TypeName& left, const TypeName& right) {
  if (left.kind != right.kind) {
    return false;
  }
  if (left.kind == TypeName::Kind::Named || left.kind == TypeName::Kind::Generic) {
    return left.name == right.name;
  }
  return left.kind != TypeName::Kind::Parameter || left.parameter == right.parameter;
}

namespace {

enum class TypeText : std::uint8_t { Display, Canonical };

void RenderType(const TypeName& type, TypeText mode, std::string& text) {
  using Kind = TypeName::Kind;
  switch (type.kind) {
    case Kind::Inferred:
      return;
    case Kind::Named:
      text += type.name;
      return;
    case Kind::Parameter:
      if (mode == TypeText::Display) {
        text += type.name;
      } else {
        text += '@';
        text += type.parameter.owner;
        text += '#';
        text += std::to_string(type.parameter.index);
      }
      return;
    case Kind::Array:
    case Kind::Nullable: {
      const auto& child = type.arguments.front();
      const bool function = child.kind == Kind::Function;
      if (function) {
        text += '(';
      }
      RenderType(child, mode, text);
      if (function) {
        text += ')';
      }
      text += type.kind == Kind::Array ? "[]" : "?";
      return;
    }
    case Kind::Generic:
      text += type.name;
      text += '<';
      for (std::size_t index = 0; index < type.arguments.size(); ++index) {
        if (index != 0) {
          text += ", ";
        }
        RenderType(type.arguments[index], mode, text);
      }
      text += '>';
      return;
    case Kind::Function:
      text += '(';
      for (std::size_t index = 0; index + 1 < type.arguments.size(); ++index) {
        if (index != 0) {
          text += ", ";
        }
        RenderType(type.arguments[index], mode, text);
      }
      text += ") => ";
      RenderType(type.arguments.back(), mode, text);
      return;
  }
  std::unreachable();
}

}  // namespace

std::string TypeName::ToString() const {
  std::string text;
  RenderType(*this, TypeText::Display, text);
  return text;
}

std::string CanonicalTypeText(const TypeName& type) {
  std::string text;
  RenderType(type, TypeText::Canonical, text);
  return text;
}

}  // namespace vela::frontend
