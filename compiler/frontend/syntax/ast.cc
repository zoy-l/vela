#include "compiler/frontend/syntax/ast.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <absl/container/flat_hash_map.h>

#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/typing/type_names.h"

namespace vela::frontend {

TypeName SubstituteTypeName(TypeName type, const TypeSubstitutions& substitutions) {
  if (type.kind == TypeName::Kind::Parameter) {
    const auto found = substitutions.find(type.parameter);
    return found == substitutions.end() ? type : found->second;
  }
  // 只有由参数替换产生的内层可空才合并；源码直接写出的双重可空仍由类型检查报告。
  const bool substitutes_element = type.kind == TypeName::Kind::Nullable &&
                                   type.arguments.front().kind == TypeName::Kind::Parameter &&
                                   substitutions.contains(type.arguments.front().parameter);
  for (auto& argument : type.arguments) {
    argument = SubstituteTypeName(std::move(argument), substitutions);
  }
  if (substitutes_element && IsNullableTypeName(type.arguments.front())) {
    return std::move(type.arguments.front());
  }
  return type;
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

std::optional<std::string_view> CompoundAssignmentOperation(std::string_view text) {
  constexpr auto operations = std::to_array<std::string_view>({
      "+=",
      "-=",
      "*=",
      "/=",
      "%=",
      "&=",
      "|=",
      "^=",
      "<<=",
      ">>=",
      ">>>=",
  });
  const auto found = std::ranges::find(operations, text);
  return found == operations.end() ? std::nullopt
                                   : std::optional(found->substr(0, found->size() - 1));
}

namespace {

enum class TypeText : std::uint8_t { Display, Canonical };

std::string RenderType(const TypeName& type, TypeText mode) {
  using Kind = TypeName::Kind;
  switch (type.kind) {
    case Kind::Named:
      return type.name;
    case Kind::Parameter:
      return mode == TypeText::Display
                 ? type.name
                 : '@' + type.parameter.owner + '#' + std::to_string(type.parameter.index);
    case Kind::Array:
    case Kind::Nullable: {
      const auto& child = type.arguments.front();
      auto text = RenderType(child, mode);
      if (child.kind == Kind::Function) {
        text = '(' + text + ')';
      }
      return text + (type.kind == Kind::Array ? "[]" : "?");
    }
    case Kind::Generic: {
      auto text = type.name + '<';
      for (std::size_t index = 0; index < type.arguments.size(); ++index) {
        text += (index == 0 ? "" : ", ") + RenderType(type.arguments.at(index), mode);
      }
      return text + '>';
    }
    case Kind::Function: {
      std::string text = "(";
      for (std::size_t index = 0; index + 1 < type.arguments.size(); ++index) {
        text += (index == 0 ? "" : ", ") + RenderType(type.arguments.at(index), mode);
      }
      return text + ") => " + RenderType(type.arguments.back(), mode);
    }
  }
  std::unreachable();
}

}  // namespace

std::string TypeName::ToString() const {
  return RenderType(*this, TypeText::Display);
}

std::string CanonicalTypeText(const TypeName& type) {
  return RenderType(type, TypeText::Canonical);
}

}  // namespace vela::frontend
