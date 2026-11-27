#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/frontend/syntax/names.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "compiler/frontend/generics/specialization.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/type_name.h"
#include "compiler/frontend/typing/type_names.h"

namespace vela::frontend {

const ClassDescriptor* SemanticModel::FindClassDescriptor(const Class& declaration) const {
  const auto find = [&](std::string_view name) -> const ClassDescriptor* {
    const auto found = classes.find(name);
    return found == classes.end() ? nullptr : &found->second;
  };
  if (declaration.type_parameters.empty()) {
    return find(declaration.name);
  }
  std::vector<TypeName> arguments;
  arguments.reserve(declaration.type_parameters.size());
  for (const auto& parameter : declaration.type_parameters) {
    arguments.push_back(ParameterTypeName(parameter, parameter.location));
  }
  return find(SpecializationName(declaration.name, arguments));
}

bool IsSameType(const TypeDescriptor& left, const TypeDescriptor& right) noexcept {
  return left.kind == right.kind && left.name == right.name && left.nullable == right.nullable;
}

std::string ClassFieldKey(std::string_view owner, std::string_view name) {
  return IsPrivateName(name) ? std::string(owner) + '.' + std::string(name) : std::string(name);
}

std::string ClassDescriptor::FieldKey(std::string_view name) const {
  return ClassFieldKey(type.name, name);
}

const ClassDescriptor::Field* ClassDescriptor::FindField(std::string_view name) const {
  const auto found = IsPrivateName(name) ? fields.find(FieldKey(name)) : fields.find(name);
  return found == fields.end() ? nullptr : &found->second;
}

bool ClassDescriptor::UsesFieldConstructor() const {
  return constructor_kind == ConstructorKind::Generated &&
         std::ranges::all_of(constructor_bindings,
                             [](const auto& binding) { return binding.field != nullptr; });
}

bool FunctionDescriptor::HasDefaults() const {
  return std::ranges::any_of(parameters, &Parameter::has_default);
}

void FunctionDescriptor::AddParameter(std::string name,
                                      TypeDescriptor type,
                                      ParameterPresence presence) {
  if (presence == ParameterPresence::Optional) {
    optional_parameters.insert(name);
  }
  parameter_indices.emplace(name, static_cast<std::uint32_t>(parameters.size()));
  parameters.push_back({.name = std::move(name), .type = std::move(type)});
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
  if (type.kind == TypeDescriptor::Kind::Never || type.kind == TypeDescriptor::Kind::Null) {
    return {.kind = TypeDescriptor::Kind::Null, .name = kNullTypeName};
  }
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

TypeDescriptor MakeFunctionType(std::span<const FunctionDescriptor::Parameter> parameters,
                                const TypeDescriptor& return_type,
                                std::optional<TypeDescriptor> narrowed_parameter,
                                std::optional<TypeDescriptor> false_narrowed_parameter) {
  std::string name = "(";
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    name += index == 0 ? "" : ", ";
    name += parameters[index].type.name;
  }
  name += ") => ";
  name += return_type.name;
  if (narrowed_parameter) {
    name += " [0 is ";
    name += narrowed_parameter->name;
    name += ']';
  }
  if (false_narrowed_parameter) {
    name += " [0 false is ";
    name += false_narrowed_parameter->name;
    name += ']';
  }
  return {
      .kind = TypeDescriptor::Kind::Function,
      .name = InternTypeName(std::move(name)),
      .is_abstract =
          return_type.is_abstract ||
          std::ranges::any_of(parameters,
                              [](const auto& parameter) { return parameter.type.is_abstract; }),
  };
}

}  // namespace vela::frontend
