#include "compiler/frontend/ast.h"

#include <cstddef>
#include <string>
#include <utility>

namespace vela::frontend {

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
