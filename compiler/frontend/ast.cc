#include "compiler/frontend/ast.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <absl/container/flat_hash_map.h>

namespace vela::frontend {

TypeName SubstituteTypeName(TypeName type,
                            const absl::flat_hash_map<std::string, TypeName>& substitutions) {
  if (type.kind == TypeName::Kind::Named) {
    if (const auto found = substitutions.find(type.name); found != substitutions.end()) {
      return found->second;
    }
  }
  for (auto& argument : type.arguments) {
    argument = SubstituteTypeName(std::move(argument), substitutions);
  }
  return type;
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

std::string TypeName::ToString() const {
  switch (kind) {
    case Kind::Named:
      return name;
    case Kind::Array:
    case Kind::Nullable: {
      const auto& child = arguments.front();
      auto text = child.ToString();
      if (child.kind == Kind::Function) {
        text = '(' + text + ')';
      }
      return text + (kind == Kind::Array ? "[]" : "?");
    }
    case Kind::Generic: {
      auto text = name + '<';
      for (std::size_t index = 0; index < arguments.size(); ++index) {
        text += (index == 0 ? "" : ", ") + arguments.at(index).ToString();
      }
      return text + '>';
    }
    case Kind::Function: {
      std::string text = "(";
      for (std::size_t index = 0; index + 1 < arguments.size(); ++index) {
        text += (index == 0 ? "" : ", ") + arguments.at(index).ToString();
      }
      return text + ") => " + arguments.back().ToString();
    }
  }
  std::unreachable();
}

}  // namespace vela::frontend
