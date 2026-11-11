#include "compiler/frontend/typing/type_checker.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <absl/container/flat_hash_set.h>
#include <absl/container/node_hash_map.h>

#include "base/reflection.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/syntax/token.h"
#include "compiler/frontend/typing/semantic_model.h"

namespace vela::frontend {
namespace {

ReflectionMemberBinding SelectReflectedMethod(const Function& method,
                                              const FunctionDescriptor& signature) {
  return {
      .kind = ReflectedMethodKind(method),
      .name = ReflectedMethodName(method),
      .type_arguments = signature.type_arguments,
  };
}

}  // namespace

bool TypeChecker::IsAttributeSubtype(const Type& type, std::string_view base) const {
  const auto target = types_.find(base);
  return type.kind == Type::Kind::Class && !type.nullable && target != types_.end() &&
         IsSubtype(type, target->second);
}

std::optional<AttributeValue> TypeChecker::AttributeData(const Expression& expression,
                                                         const Type& expected,
                                                         AttributeValueSource source) {
  constexpr std::string_view kScalarDataError =
      "attribute arguments and defaults require direct scalar data";
  using Kind = Expression::Kind;
  const auto kind = expression.kind;
  const bool negative = kind == Kind::Unary && expression.operation == TokenKind::Minus &&
                        (expression.operand->kind == Kind::IntegerLiteral ||
                         expression.operand->kind == Kind::DoubleLiteral);
  if (kind != Kind::NullLiteral && kind != Kind::BooleanLiteral && kind != Kind::StringLiteral &&
      kind != Kind::DoubleLiteral && kind != Kind::IntegerLiteral && kind != Kind::Member &&
      !negative) {
    AddDiagnostic(expression.location, std::string(kScalarDataError));
    return std::nullopt;
  }
  const auto checked = facts_.expression_types.find(&expression);
  const auto actual = checked == facts_.expression_types.end()
                          ? CheckExpression(expression, &expected)
                          : checked->second;
  if (source == AttributeValueSource::Argument && !CanAssign(expected, actual)) {
    AddDiagnostic(expression.location, "attribute value does not match its declared field type");
    return std::nullopt;
  }
  AttributeValue result{.type = actual, .value = {}};
  switch (expression.kind) {
    case Kind::NullLiteral:
      return result;
    case Kind::BooleanLiteral:
      result.value = expression.boolean_value;
      return result;
    case Kind::StringLiteral:
      result.value = expression.text;
      return result;
    case Kind::DoubleLiteral:
      result.value = expression.double_value;
      return result;
    case Kind::IntegerLiteral:
    case Kind::Member:
    case Kind::Unary: {
      if (negative && expression.operand->kind == Kind::DoubleLiteral) {
        result.value = -expression.operand->double_value;
        return result;
      }
      const auto value = EvaluateConstant(expression);
      if (value && std::holds_alternative<std::int64_t>(*value)) {
        result.value = std::get<std::int64_t>(*value);
        return result;
      }
      break;
    }
    default:
      break;
  }
  AddDiagnostic(expression.location, std::string(kScalarDataError));
  return std::nullopt;
}

std::optional<AttributeBinding> TypeChecker::BindAttribute(const Attribute& attribute) {
  const auto found = attribute_bindings_.find(&attribute);
  if (found != attribute_bindings_.end()) {
    return found->second;
  }
  auto binding = ResolveAttribute(attribute);
  attribute_bindings_.emplace(&attribute, binding);
  return binding;
}

std::optional<AttributeBinding> TypeChecker::ResolveAttribute(const Attribute& attribute) {
  const auto begin = diagnostics_.size();
  const auto type = ResolveType(attribute.type, VoidTypePolicy::Forbidden);
  if (!IsAttributeSubtype(type, reflection::kAttributeName) ||
      (type.is_abstract && !checking_definitions_)) {
    AddDiagnostic(attribute.location, "attribute requires a concrete type derived from Attribute");
    return std::nullopt;
  }
  facts_.attribute_types.insert_or_assign(&attribute, type);
  const auto& descriptor = classes_.at(type.name);
  AttributeArguments arguments;
  for (const auto& argument : attribute.arguments) {
    if (!arguments.emplace(argument.name, argument.value.get()).second) {
      AddDiagnostic(argument.location, "duplicate attribute argument '" + argument.name + "'");
    }
    if (!descriptor.constructor.parameter_indices.contains(argument.name)) {
      AddDiagnostic(argument.location, "unknown attribute argument '" + argument.name + "'");
    }
  }
  AttributeBinding result{.target = nullptr, .type = type, .fields = {}};
  result.fields.resize(descriptor.field_order.size());
  const AttributeArguments inherited;
  const auto* current = &descriptor;
  while (current != nullptr) {
    const auto& owner = *current->declaration;
    if (owner.constructor || owner.constructor_kind != ConstructorKind::Generated ||
        !owner.native_key.empty()) {
      AddDiagnostic(attribute.location,
                    "attribute data requires generated constructors without user bodies");
      return std::nullopt;
    }
    BindAttributeFields(*current, current == &descriptor ? arguments : inherited,
                        attribute.location, result.fields);
    current = current->base_class ? &classes_.at(*current->base_class) : nullptr;
  }
  return diagnostics_.size() == begin ? std::optional(std::move(result)) : std::nullopt;
}

void TypeChecker::BindAttributeFields(const ClassInfo& owner,
                                      const AttributeArguments& arguments,
                                      SourceLocation location,
                                      std::span<AttributeValue> values) {
  for (const auto& field : owner.declaration->fields) {
    if (field.storage != MemberStorage::Instance) {
      continue;
    }
    const auto& contract = owner.fields.at(field.name);
    const auto found = arguments.find(field.name);
    const auto* value = found != arguments.end() ? found->second : field.initializer.get();
    if (found != arguments.end() && field.initializer) {
      static_cast<void>(
          AttributeData(*field.initializer, contract.type, AttributeValueSource::Default));
    }
    if (value == nullptr) {
      AddDiagnostic(location, "missing attribute value for field '" + field.name + "'");
      continue;
    }
    const auto source =
        found != arguments.end() ? AttributeValueSource::Argument : AttributeValueSource::Default;
    if (auto data = AttributeData(*value, contract.type, source)) {
      values[contract.index] = std::move(*data);
    }
  }
}

bool TypeChecker::CanRepeatAttribute(const Type& type) {
  const auto& declaration = *classes_.at(type.name).declaration;
  const Attribute* usage = nullptr;
  for (const auto& attribute : declaration.attributes) {
    const auto bound = ResolveType(attribute.type, VoidTypePolicy::Forbidden);
    if (bound.name != reflection::kAttributeUsageName) {
      continue;
    }
    if (usage != nullptr) {
      AddDiagnostic(attribute.location, "AttributeUsage may appear only once on an attribute type");
      return false;
    }
    usage = &attribute;
  }
  if (usage == nullptr) {
    return false;
  }
  const auto bound = BindAttribute(*usage);
  return bound && !bound->fields.empty() &&
         std::holds_alternative<bool>(bound->fields.front().value) &&
         std::get<bool>(bound->fields.front().value);
}

void TypeChecker::CheckAttributeTarget(std::span<const Attribute> attributes,
                                       Type type,
                                       const void* target,
                                       const std::optional<ReflectionMemberBinding>& member) {
  absl::flat_hash_set<std::string_view> seen;
  for (const auto& attribute : attributes) {
    auto bound = BindAttribute(attribute);
    if (!bound) {
      continue;
    }
    if (!seen.insert(bound->type.name).second && !CanRepeatAttribute(bound->type)) {
      AddDiagnostic(attribute.location,
                    "attribute type does not allow multiple annotations on one declaration");
    }
    if (bound->type.name == reflection::kAttributeUsageName &&
        (member || !IsAttributeSubtype(type, reflection::kAttributeName))) {
      AddDiagnostic(attribute.location, "AttributeUsage requires an attribute class declaration");
    }
    if (!checking_definitions_) {
      if (IsAttributeSubtype(bound->type, reflection::kReflectName)) {
        RegisterAttributeReflection(type, member);
      }
      bound->target = target;
      attributes_.push_back(std::move(*bound));
    }
  }
}

void TypeChecker::RegisterAttributeReflection(
    const Type& type,
    const std::optional<ReflectionMemberBinding>& member) {
  ReflectionBinding registration{.type = type, .members = {}};
  if (member) {
    registration.members.push_back(*member);
  } else if (const auto found = classes_.find(type.name); found != classes_.end()) {
    const auto& descriptor = found->second;
    const auto& owner = *descriptor.declaration;
    for (const auto& field : owner.fields) {
      if (field.visibility == Visibility::Public) {
        registration.members.push_back(
            {.kind = ReflectionMemberKind::Field, .name = field.name, .type_arguments = {}});
      }
    }
    for (const auto& method : owner.methods) {
      if (method.visibility == Visibility::Public) {
        registration.members.push_back(
            SelectReflectedMethod(method, descriptor.methods.at(method.name)));
      }
    }
    if (owner.constructor_kind != ConstructorKind::Unavailable &&
        (!owner.constructor || owner.constructor->visibility == Visibility::Public)) {
      registration.members.push_back(
          {.kind = ReflectionMemberKind::Constructor, .name = {}, .type_arguments = {}});
    }
  }
  reflection_.push_back(std::move(registration));
}

void TypeChecker::CheckClassAttributes(const Class& owner, const Type& type) {
  CheckAttributeTarget(owner.attributes, type, &owner);
  const auto& methods = classes_.at(type.name).methods;
  for (const auto& field : owner.fields) {
    if (field.attributes.empty()) {
      continue;
    }
    CheckAttributeTarget(field.attributes, type, &field,
                         ReflectionMemberBinding{
                             .kind = ReflectionMemberKind::Field,
                             .name = field.name,
                             .type_arguments = {},
                         });
  }
  CheckMethodAttributes(owner.methods, type, methods);
  if (owner.constructor && !owner.constructor->attributes.empty()) {
    CheckAttributeTarget(owner.constructor->attributes, type, owner.constructor.get(),
                         ReflectionMemberBinding{
                             .kind = ReflectionMemberKind::Constructor,
                             .name = {},
                             .type_arguments = {},
                         });
  }
}

void TypeChecker::CheckInterfaceAttributes(const Interface& owner, const Type& type) {
  CheckAttributeTarget(owner.attributes, type, &owner);
  const auto& methods = interfaces_.at(type.name).methods;
  CheckMethodAttributes(owner.methods, type, methods);
  if (checking_definitions_) {
    CheckInterfaceTemplateAttributes(owner, type);
  }
}

void TypeChecker::CheckMethodAttributes(
    std::span<const Function> methods,
    const Type& type,
    const absl::node_hash_map<std::string, FunctionDescriptor>& signatures) {
  for (const auto& method : methods) {
    if (method.attributes.empty()) {
      continue;
    }
    const auto& signature = signatures.at(method.name);
    CheckAttributeTarget(method.attributes, type, &method,
                         SelectReflectedMethod(method, signature));
  }
}

void TypeChecker::CheckInterfaceTemplateAttributes(const Interface& owner, const Type& type) {
  for (const auto& method : owner.method_templates) {
    if (method.attributes.empty()) {
      continue;
    }
    CheckAttributeTarget(method.attributes, type, &method,
                         ReflectionMemberBinding{
                             .kind = ReflectionMemberKind::Method,
                             .name = method.name,
                             .type_arguments = {},
                         });
  }
}

void TypeChecker::CheckTopLevelFunctionAttributes(const Function& function) {
  if (!function.attributes.empty()) {
    AddDiagnostic(function.attributes.front().location,
                  "top-level function attributes are not supported");
  }
}

void TypeChecker::CheckAttributes(const Program& program) {
  for (const auto& owner : program.classes) {
    CheckClassAttributes(owner, classes_.at(owner.name).type);
  }
  for (const auto& owner : program.interfaces) {
    CheckInterfaceAttributes(owner, interfaces_.at(owner.name).type);
  }
  for (const auto& owner : program.enums) {
    const auto& type = types_.at(owner.name);
    CheckAttributeTarget(owner.attributes, type, &owner);
    for (const auto& member : owner.members) {
      if (member.attributes.empty()) {
        continue;
      }
      CheckAttributeTarget(member.attributes, type, &member,
                           ReflectionMemberBinding{
                               .kind = ReflectionMemberKind::EnumMember,
                               .name = member.name,
                               .type_arguments = {},
                           });
    }
  }
  for (const auto& function : program.functions) {
    CheckTopLevelFunctionAttributes(function);
  }
}

}  // namespace vela::frontend
