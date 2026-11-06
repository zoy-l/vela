#include "compiler/frontend/typing/semantic_model.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "compiler/frontend/typing/type_name.h"

namespace vela::frontend {

void FunctionDescriptor::AddParameter(std::string name,
                                      TypeDescriptor type,
                                      ParameterPresence presence) {
  if (presence == ParameterPresence::Optional) {
    optional_parameters.insert(name);
  }
  parameter_indices.emplace(name, static_cast<std::uint32_t>(parameters.size()));
  parameters.push_back({.name = std::move(name), .type = type});
}

TypeDescriptor MakeArrayType(const TypeDescriptor& element) {
  const std::string name = element.kind == TypeDescriptor::Kind::Function
                               ? "(" + std::string(element.name) + ")[]"
                               : std::string(element.name) + "[]";
  return {
      .kind = TypeDescriptor::Kind::Array,
      .name = InternTypeName(name),
      .is_abstract = element.is_abstract,
  };
}

TypeDescriptor MakeNullableType(TypeDescriptor type) {
  if (!type.nullable && type.kind != TypeDescriptor::Kind::Dynamic) {
    type.nullable = true;
    const std::string name = type.kind == TypeDescriptor::Kind::Function
                                 ? "(" + std::string(type.name) + ")?"
                                 : std::string(type.name) + '?';
    type.name = InternTypeName(name);
  }
  return type;
}

std::string_view NonNullableTypeName(const TypeDescriptor& type) noexcept {
  if (!type.nullable) {
    return type.name;
  }
  return type.kind == TypeDescriptor::Kind::Function ? type.name.substr(1, type.name.size() - 3)
                                                     : type.name.substr(0, type.name.size() - 1);
}

TypeDescriptor MakeFunctionType(const FunctionDescriptor& signature) {
  std::string name = "(";
  for (std::size_t index = 0; index < signature.parameters.size(); ++index) {
    name += index == 0 ? "" : ", ";
    name += signature.parameters.at(index).type.name;
  }
  name += ") => ";
  name += signature.return_type.name;
  return {
      .kind = TypeDescriptor::Kind::Function,
      .name = InternTypeName(std::move(name)),
      .is_abstract =
          signature.return_type.is_abstract ||
          std::ranges::any_of(signature.parameters,
                              [](const auto& parameter) { return parameter.type.is_abstract; }),
  };
}

}  // namespace vela::frontend
