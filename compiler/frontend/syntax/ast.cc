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

#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/syntax/integer_literal.h"
#include "compiler/frontend/typing/type_names.h"

namespace vela::frontend {

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
