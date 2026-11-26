#include "base/builtin.h"
#include "base/member.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/flow/constructor_observation.h"
#include "compiler/frontend/flow/initialization.h"
#include "compiler/frontend/syntax/names.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/frontend/typing/type_checker.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include "compiler/frontend/syntax/ast.h"

#include "compiler/frontend/builtins/async_builtins.h"
#include "compiler/frontend/builtins/builtins.h"
#include "compiler/frontend/builtins/standard_environment.h"
#include "compiler/frontend/typing/type_name.h"
#include "compiler/frontend/typing/type_names.h"

namespace vela::frontend {
namespace {

bool HasUnforwardedParameters(std::span<const FunctionDescriptor::Parameter> parameters,
                              std::span<const ClassDescriptor::ConstructorBinding> bindings) {
  std::vector<bool> forwarded(parameters.size());
  for (const auto& binding : bindings) {
    if (binding.base_parameter) {
      forwarded[*binding.base_parameter] = true;
    }
  }
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    if (!parameters[index].has_default && !forwarded[index]) {
      return true;
    }
  }
  return false;
}

}  // namespace

void TypeChecker::CollectTypes(const Program& program) {
  const auto register_builtin = [this](Type::Kind kind, std::string_view name) {
    types_.emplace(name, Type{.kind = kind, .name = name});
  };
  register_builtin(Type::Kind::Void, kVoidTypeName);
  register_builtin(Type::Kind::Dynamic, kDynamicTypeName);
  register_builtin(Type::Kind::Bool, kBoolTypeName);
  register_builtin(Type::Kind::Int, kIntTypeName);
  register_builtin(Type::Kind::Double, kDoubleTypeName);
  register_builtin(Type::Kind::String, kStringTypeName);
  register_builtin(Type::Kind::MutableString, kMutableStringTypeName);
  register_builtin(Type::Kind::ByteArray, kByteArrayTypeName);
  register_builtin(Type::Kind::ByteData, kByteDataTypeName);
  register_builtin(Type::Kind::RegExp, kRegExpTypeName);
  register_builtin(Type::Kind::RegExpMatch, kRegExpMatchTypeName);
  register_builtin(Type::Kind::Timer, kTimerTypeName);
  register_builtin(Type::Kind::CancellationToken, kCancellationTokenTypeName);
  static_cast<void>(ArrayType(types_.at(kDynamicTypeName)));
  static_cast<void>(ArrayType(types_.at(kStringTypeName)));
  static_cast<void>(ArrayType(types_.at(kIntTypeName)));
  static_cast<void>(ArrayType(types_.at(kRegExpMatchTypeName)));
  static_cast<void>(NullableType(types_.at(kRegExpMatchTypeName)));
  static_cast<void>(ArrayType(NullableType(types_.at(kStringTypeName))));
  const Type json_object{
      .kind = Type::Kind::Map,
      .name = InternTypeName(std::string(kMapTypeName) + "<" + std::string(kStringTypeName) + ", " +
                             std::string(kDynamicTypeName) + ">"),
  };
  types_.emplace(json_object.name, json_object);
  generic_arguments_.emplace(
      json_object.name, std::vector<Type>{types_.at(kStringTypeName), types_.at(kDynamicTypeName)});
  types_.reserve(types_.size() + program.enums.size() + program.classes.size());
  classes_.reserve(program.classes.size());
  for (const auto& interface : program.interfaces) {
    const Type type{.kind = Type::Kind::Interface, .name = InternTypeName(interface.name)};
    if (IsBuiltinName(interface.name) || !types_.emplace(interface.name, type).second) {
      AddDiagnostic(interface.location, "duplicate or reserved interface '" + interface.name + "'");
    }
  }

  for (const Enum& enumeration : program.enums) {
    if (IsBuiltinName(enumeration.name)) {
      AddDiagnostic(enumeration.location, "type name is reserved for a builtin");
      continue;
    }
    const Type type{.kind = Type::Kind::Enum, .name = InternTypeName(enumeration.name)};
    if (!types_.emplace(enumeration.name, type).second) {
      AddDiagnostic(enumeration.location, "duplicate type '" + enumeration.name + "'");
      continue;
    }
    std::int64_t ordinal = 0;
    enum_sizes_.emplace(enumeration.name, enumeration.members.size());
    for (const auto& member : enumeration.members) {
      if (!enum_values_.try_emplace(enumeration.name + "." + member.name, ordinal++).second) {
        AddDiagnostic(member.location, "duplicate enum member '" + member.name + "'");
      }
    }
  }

  for (const Class& class_declaration : program.classes) {
    const auto& name = class_declaration.name;
    if (IsStandardTypeNamespace(program, class_declaration)) {
      ClassInfo info;
      info.type = types_.at(name);
      classes_.emplace(name, std::move(info));
      continue;
    }
    if (IsBuiltinName(class_declaration.name)) {
      AddDiagnostic(class_declaration.location, "type name is reserved for a builtin");
      continue;
    }
    const Type type{.kind = Type::Kind::Class, .name = InternTypeName(class_declaration.name)};
    if (!types_.emplace(class_declaration.name, type).second) {
      AddDiagnostic(class_declaration.location, "duplicate type '" + class_declaration.name + "'");
      continue;
    }
    ClassInfo info;
    info.type = type;
    info.declaration = &class_declaration;
    classes_.emplace(class_declaration.name, std::move(info));
  }
  for (const auto& alias : program.aliases) {
    if (types_.contains(alias.name) || IsBuiltinName(alias.name) ||
        !aliases_.emplace(alias.name, &alias).second) {
      AddDiagnostic(alias.location, "duplicate or reserved type alias '" + alias.name + "'");
    }
  }
}

void TypeChecker::ResolveDeclarations(const Program& program) {
  functions_.reserve(program.functions.size());
  absl::flat_hash_set<std::string> native_keys;
  for (const Class& class_declaration : program.classes) {
    const auto& native_key = class_declaration.native_key;
    if (!native_key.empty() && !native_keys.insert(native_key).second) {
      AddDiagnostic(class_declaration.location, "duplicate native class key '" + native_key + "'");
    }
    const auto class_info = classes_.find(class_declaration.name);
    if (class_info == classes_.end()) {
      continue;
    }
    ResolveClass(class_declaration, class_info->second);
  }
  for (const Function& function : program.functions) {
    FunctionInfo function_info = ResolveSignature(function);
    if (IsBuiltinName(function.name) || types_.contains(function.name) ||
        aliases_.contains(function.name)) {
      AddDiagnostic(function.location, "function name is reserved for a type or builtin");
      continue;
    }
    if (!functions_.emplace(function.name, std::move(function_info)).second) {
      AddDiagnostic(function.location, "duplicate function '" + function.name + "'");
    }
  }
}

void TypeChecker::ResolveClass(const Class& class_declaration, ClassInfo& info) {
  info.declaration = &class_declaration;
  class_definitions_.try_emplace(info.type.name, &class_declaration);
  info.constructor.return_type = info.type;
  ResolveClassRelations(class_declaration, info);
  ResolveClassMembers(class_declaration, info);
  info.constructor_kind = class_declaration.constructor_kind;
  info.constructor.member_owner = info.type.name;
}

void TypeChecker::ResolveClassRelations(const Class& class_declaration, ClassInfo& info) {
  if (class_declaration.base_class) {
    const auto base = ResolveType(*class_declaration.base_class, VoidTypePolicy::Forbidden);
    if (base.kind != Type::Kind::Class || base.nullable) {
      AddDiagnostic(class_declaration.base_class->location, "extends requires a class type");
    } else if (const auto& base_info = classes_.at(base.name);
               base_info.declaration != nullptr && !base_info.declaration->native_key.empty()) {
      AddDiagnostic(class_declaration.base_class->location,
                    "scripts cannot extend an external class");
    } else {
      info.base_class = base.name;
    }
  }
  for (const auto& interface : class_declaration.interfaces) {
    const auto type = ResolveType(interface, VoidTypePolicy::Forbidden);
    if (type.kind != Type::Kind::Interface || type.nullable) {
      AddDiagnostic(interface.location, "implements requires an interface type");
    } else if (!info.interfaces.insert(std::string(type.name)).second) {
      AddDiagnostic(interface.location, "duplicate implemented interface");
    }
  }
}

void TypeChecker::ResolveClassMembers(const Class& class_declaration, ClassInfo& info) {
  info.fields.reserve(class_declaration.fields.size());
  info.field_order.reserve(class_declaration.fields.size());
  info.methods.reserve(class_declaration.methods.size());
  for (const Field& field : class_declaration.fields) {
    ResolveField(field, info);
  }
  ResolveClassMethods(class_declaration, info);
}

void TypeChecker::ResolveClassMethods(const Class& class_declaration, ClassInfo& info) {
  for (const Function& method : class_declaration.methods) {
    ResolveClassMethod(method, info);
  }
  for (const auto& method : class_declaration.method_templates) {
    if (info.FindField(method.name) != nullptr || info.static_fields.contains(method.name) ||
        (info.methods.contains(method.name) && method.intrinsic != Builtin::JsonTypedParse) ||
        !info.method_templates
             .emplace(method.name,
                      MethodTemplate{.owner = class_declaration.name, .declaration = &method})
             .second) {
      AddDiagnostic(method.location, "duplicate member '" + method.name + "'");
    }
  }
}

void TypeChecker::ResolveClassMethod(const Function& method, ClassInfo& info) {
  FunctionInfo method_info = ResolveSignature(method);
  method_info.member_owner = info.type.name;
  if (method_info.is_setter) {
    method_info.setter_owner = info.type.name;
    method_info.setter_visibility = method.visibility;
  }
  CheckAccessor(method, method_info);
  if (info.FindField(method.name) != nullptr || info.static_fields.contains(method.name)) {
    AddDiagnostic(method.location, "member name is already used by a field");
  }
  const auto found = info.methods.find(method.name);
  if (found == info.methods.end()) {
    if (method_info.is_setter && !method_info.parameters.empty()) {
      method_info.setter_parameter_type = method_info.parameters.front().type;
      method_info.setter_return_type = method_info.return_type;
    }
    info.methods.emplace(method.name, std::move(method_info));
  } else if ((found->second.is_getter && method_info.is_setter) ||
             (found->second.is_setter && method_info.is_getter)) {
    MergeAccessor(found->second, method_info, method.location);
  } else {
    AddDiagnostic(method.location, "duplicate method '" + method.name + "'");
  }
}

void TypeChecker::MergeAccessor(FunctionInfo& existing,
                                const FunctionInfo& incoming,
                                SourceLocation location) {
  if (existing.is_getter && incoming.is_setter &&
      (incoming.parameters.size() != 1 ||
       !IsSameType(existing.return_type, incoming.parameters.front().type))) {
    AddDiagnostic(location, "accessor getter and setter types must match");
  }
  if (existing.is_setter && incoming.is_getter &&
      (existing.parameters.size() != 1 ||
       !IsSameType(existing.parameters.front().type, incoming.return_type))) {
    AddDiagnostic(location, "accessor getter and setter types must match");
  }
  existing.is_getter = existing.is_getter || incoming.is_getter;
  existing.is_setter = existing.is_setter || incoming.is_setter;
  if (incoming.is_getter) {
    existing.return_type = incoming.return_type;
    existing.member_owner = incoming.member_owner;
    existing.visibility = incoming.visibility;
  }
  if (incoming.is_setter && !incoming.parameters.empty()) {
    existing.parameters = incoming.parameters;
    existing.parameter_indices = incoming.parameter_indices;
    existing.setter_parameter_type = incoming.parameters.front().type;
    existing.setter_return_type = incoming.return_type;
    existing.setter_owner = incoming.setter_owner;
    existing.setter_visibility = incoming.setter_visibility;
  }
}

void TypeChecker::CheckAccessor(const Function& method, const FunctionInfo& signature) {
  if (method.is_getter && !method.parameters.empty()) {
    AddDiagnostic(method.location, "getter must not declare parameters");
  }
  if (method.is_setter &&
      (method.parameters.size() != 1 || signature.return_type.kind != Type::Kind::Void)) {
    AddDiagnostic(method.location, "setter must declare one parameter and return void");
  }
}

void TypeChecker::ResolveClassConstructor(const Class& declaration, ClassInfo& info) {
  const auto* constructor = declaration.constructor.get();
  const auto* base = info.base_class ? &classes_.at(*info.base_class) : nullptr;
  const auto explicit_super = declaration.HasExplicitSuperCall();
  info.constructor_bindings.clear();
  FunctionInfo signature;
  if (info.constructor_kind == ConstructorKind::Generated) {
    signature = ResolveGeneratedConstructor(declaration, info, explicit_super ? nullptr : base);
  } else if (constructor != nullptr) {
    signature = ResolveExplicitConstructor(declaration, info, base);
  }
  signature.return_type = info.type;
  signature.member_owner = info.type.name;
  signature.visibility = constructor != nullptr ? constructor->visibility : Visibility::Public;
  if (info.constructor_kind != ConstructorKind::Unavailable && base != nullptr && !explicit_super) {
    if (base->constructor_kind == ConstructorKind::Unavailable) {
      AddDiagnostic(declaration.location, "base class has no callable constructor");
    } else if (base->constructor.visibility == Visibility::Private) {
      AddDiagnostic(declaration.location, "cannot inherit a private constructor");
    }
  }
  info.constructor = std::move(signature);
  if (constructor != nullptr) {
    auto body_signature = info.constructor_kind == ConstructorKind::Explicit
                              ? info.constructor
                              : ResolveSignature(*constructor);
    body_signature.return_type = ResolveType(constructor->return_type, VoidTypePolicy::Allowed);
    info.methods.insert_or_assign(kConstructorName, std::move(body_signature));
  }
}

TypeChecker::FunctionInfo TypeChecker::ResolveGeneratedConstructor(const Class& declaration,
                                                                   ClassInfo& info,
                                                                   const ClassInfo* base) {
  FunctionInfo signature;
  const auto append = [&](const FunctionInfo::Parameter& parameter,
                          ClassInfo::ConstructorBinding binding) {
    if (signature.parameter_indices.contains(parameter.name)) {
      AddDiagnostic(declaration.location,
                    "duplicate constructor parameter '" + parameter.name + "'");
      return;
    }
    using Presence = FunctionInfo::ParameterPresence;
    signature.AddParameter(parameter.name, parameter.type,
                           parameter.has_default ? Presence::Optional : Presence::Required);
    signature.parameters.back().has_default = parameter.has_default;
    info.constructor_bindings.push_back(binding);
  };
  if (base != nullptr) {
    for (std::uint32_t index = 0; index < base->constructor.parameters.size(); ++index) {
      const auto* const field = base->constructor_kind == ConstructorKind::Generated
                                    ? base->constructor_bindings.at(index).field
                                    : nullptr;
      append(base->constructor.parameters.at(index), {.field = field, .base_parameter = index});
    }
  }
  for (const auto& field : declaration.fields) {
    if (declaration.IsConstructorField(field)) {
      const auto& descriptor = *info.FindField(field.name);
      append(
          {
              .name = field.name,
              .type = descriptor.type,
              .has_default = field.initializer != nullptr,
          },
          {.field = &descriptor, .base_parameter = {}});
    }
  }
  return signature;
}

std::pair<TypeChecker::FunctionInfo::Parameter, TypeChecker::ClassInfo::ConstructorBinding>
TypeChecker::ResolveConstructorBinding(const Parameter& parameter,
                                       const ClassInfo& owner,
                                       const ClassInfo* base) {
  FunctionInfo::Parameter descriptor{.name = parameter.name, .type = {}};
  ClassInfo::ConstructorBinding binding;
  if (parameter.binding == Parameter::Binding::Field) {
    const auto* field = owner.FindField(parameter.member_name);
    if (field == nullptr || field->owner != owner.type.name) {
      AddDiagnostic(parameter.location,
                    "this parameter requires an own instance field '" + parameter.name + "'");
    } else {
      descriptor.type = field->type;
      descriptor.has_default = field->declaration->initializer != nullptr;
      binding.field = field;
    }
  } else if (parameter.binding == Parameter::Binding::Super) {
    if (owner.declaration->HasExplicitSuperCall()) {
      AddDiagnostic(parameter.location, "super parameters cannot be combined with super(...)");
    }
    if (base == nullptr || !base->constructor.parameter_indices.contains(parameter.name)) {
      AddDiagnostic(parameter.location,
                    "unknown super constructor parameter '" + parameter.name + "'");
    } else {
      const auto& constructor = base->constructor;
      const auto index = constructor.parameter_indices.at(parameter.name);
      descriptor = constructor.parameters.at(index);
      binding.base_parameter = index;
    }
  }
  return {std::move(descriptor), binding};
}

TypeChecker::FunctionInfo TypeChecker::ResolveExplicitConstructor(const Class& declaration,
                                                                  ClassInfo& info,
                                                                  const ClassInfo* base) {
  const auto& parameters = declaration.constructor->parameters;
  FunctionInfo context;
  for (const auto& parameter : parameters) {
    if (context.parameter_indices.contains(parameter.name)) {
      continue;
    }
    auto [descriptor, binding] = ResolveConstructorBinding(parameter, info, base);
    context.AddParameter(parameter.name, descriptor.type);
    context.parameters.back().has_default = descriptor.has_default;
    binding.declaration = &parameter;
    info.constructor_bindings.push_back(binding);
  }
  auto signature = ResolveSignature(*declaration.constructor, &context);
  for (std::size_t index = 0; index < signature.parameters.size(); ++index) {
    auto& parameter = signature.parameters[index];
    if (context.parameters[index].has_default) {
      parameter.has_default = true;
      signature.optional_parameters.insert(parameter.name);
    }
  }
  return signature;
}

void TypeChecker::ResolveField(const Field& field, ClassInfo& info) {
  const Type type = ResolveType(field.type, VoidTypePolicy::Forbidden);
  const ClassDescriptor::Field descriptor{
      .type = type,
      .index = static_cast<std::uint32_t>(info.field_order.size()),
      .visibility = field.visibility,
      .binding = field.binding,
      .owner = info.type.name,
      .declaration = &field,
  };
  if (field.storage == MemberStorage::Static) {
    if (!info.static_fields.emplace(field.name, descriptor).second ||
        info.FindField(field.name) != nullptr) {
      AddDiagnostic(field.location, "duplicate field '" + field.name + "'");
    }
    return;
  }
  if (info.static_fields.contains(field.name)) {
    AddDiagnostic(field.location, "duplicate field '" + field.name + "'");
  }
  auto key = info.FieldKey(field.name);
  if (!info.fields.emplace(key, descriptor).second) {
    AddDiagnostic(field.location, "duplicate field '" + field.name + "'");
  } else {
    info.field_order.push_back(std::move(key));
  }
}

TypeChecker::Type TypeChecker::CheckClassConstruction(
    const ClassInfo& class_info,
    const Expression& expression,
    std::span<const std::optional<Type>> checked_arguments) {
  if (class_info.constructor_kind == ConstructorKind::Unavailable) {
    AddDiagnostic(expression.location, "class has no public constructor");
    return InvalidType();
  }
  CheckAccess(class_info.constructor.visibility, class_info.type.name, expression.location);
  for (const CallArgument& argument : expression.arguments) {
    if (class_info.constructor_kind == ConstructorKind::Generated && argument.name.empty()) {
      AddDiagnostic(argument.location, "generated constructor arguments must be named");
    }
  }
  return CheckFunctionCall(class_info.constructor, expression, checked_arguments);
}

const TypeChecker::ClassInfo* TypeChecker::CheckSuperClass(SourceLocation location) {
  if (owner_ == nullptr || !owner_->base_class) {
    AddDiagnostic(location, "super requires a derived class context");
    return nullptr;
  }
  const auto base = classes_.find(*owner_->base_class);
  return base == classes_.end() ? nullptr : &base->second;
}

TypeChecker::Type TypeChecker::CheckMemberAccess(const Expression& expression,
                                                 const Type* expected) {
  if (const auto* owner = StaticOwner(expression)) {
    return CheckStaticMember(expression, *owner, expected);
  }
  if (expression.object->kind == Expression::Kind::Name && expression.object->text == kSuperName) {
    return CheckSuperMember(expression, expected);
  }
  if (const auto member = ResolveUnqualifiedEnumMember(expression)) {
    if (member->constant) {
      return member->type;
    }
    AddDiagnostic(expression.location, "unknown enum member '" + expression.text + "'");
    return InvalidType();
  }

  auto object = CheckExpression(*expression.object, nullptr);
  object = ParameterCapability(object, expression.object->location);
  object = ResolvePrivateReceiver(expression, object);
  if (!IsValid(object)) {
    return InvalidType();
  }
  if (CheckReceiverNullability(expression, object) == CheckStatus::Failed) {
    return InvalidType();
  }
  const auto result = CheckObjectMember(expression, object, expected);
  return expression.null_safe && IsValid(result) ? NullableType(result) : result;
}

TypeChecker::Type TypeChecker::CheckSuperMember(const Expression& expression,
                                                const Type* expected) {
  if (IsPrivateName(expression.text)) {
    AddDiagnostic(expression.location, "private members cannot be accessed through super");
    return InvalidType();
  }
  const auto* base = CheckSuperClass(expression.location);
  if (base == nullptr) {
    return InvalidType();
  }
  if (const auto applied = CheckMethodApplication(base->method_templates, expression, nullptr,
                                                  expected, MethodAccess::Super)) {
    return *applied;
  }
  const auto method = base->methods.find(expression.text);
  if (method == base->methods.end()) {
    AddDiagnostic(expression.location, "unknown super member '" + expression.text + "'");
    return InvalidType();
  }
  facts_.super_members.insert_or_assign(&expression, MemberReference{
                                                         .owner = method->second.member_owner,
                                                         .member = expression.text,
                                                     });
  return method->second.is_getter ? method->second.return_type
                                  : FunctionValueType(method->second, expected);
}

TypeChecker::Type TypeChecker::CheckStaticMember(const Expression& expression,
                                                 const ClassInfo& owner,
                                                 const Type* expected) {
  if (IsPrivateName(expression.text) &&
      (owner_ == nullptr || owner_->type.name != owner.type.name)) {
    CheckAccess(Visibility::Private, owner.type.name, expression.location);
    return InvalidType();
  }
  if (const auto applied = CheckMethodApplication(owner.method_templates, expression, nullptr,
                                                  expected, MethodAccess::Static)) {
    return *applied;
  }
  const auto field = owner.static_fields.find(expression.text);
  if (field != owner.static_fields.end()) {
    CheckAccess(field->second.visibility, owner.type.name, expression.location);
    facts_.static_members.insert_or_assign(
        &expression,
        MemberReference{.owner = std::string(owner.type.name), .member = expression.text});
    return field->second.type;
  }
  const auto method = owner.methods.find(expression.text);
  if (method != owner.methods.end() && method->second.storage == MemberStorage::Static) {
    CheckAccess(method->second.visibility, method->second.member_owner, expression.location);
    facts_.static_members.insert_or_assign(
        &expression,
        MemberReference{.owner = method->second.member_owner, .member = expression.text});
    return FunctionValueType(method->second, expected);
  }
  AddDiagnostic(expression.location, "class access requires a declared static member");
  return InvalidType();
}

TypeChecker::Type TypeChecker::CheckObjectMember(const Expression& expression,
                                                 const Type& object,
                                                 const Type* expected) {
  if (object.kind == Type::Kind::Dynamic) {
    if (!expression.type_arguments.empty()) {
      AddDiagnostic(expression.location,
                    "generic methods require a statically known class or interface receiver");
      return InvalidType();
    }
    InvalidateCapturedRefinements();
    return object;
  }
  if (const auto* member = FindAsyncMember(object.kind, MemberStorage::Instance, expression.text);
      member != nullptr && member->kind == MemberKind::Property) {
    return CheckAsyncProperty(expression, object);
  }
  if (const auto builtin = FindBuiltinProperty(object.kind, expression.text)) {
    facts_.builtin_calls.insert_or_assign(&expression, *builtin);
    return BuiltinSignature(*builtin).return_type;
  }
  if (object.kind == Type::Kind::Interface) {
    const auto& owner = interfaces_.at(object.name);
    if (const auto applied = CheckMethodApplication(owner.method_templates, expression, nullptr,
                                                    expected, MethodAccess::Instance)) {
      return *applied;
    }
    const auto method = owner.methods.find(expression.text);
    if (method != owner.methods.end()) {
      facts_.interface_members.insert_or_assign(
          &expression,
          MemberReference{.owner = std::string(object.name), .member = expression.text});
      return FunctionValueType(method->second, expected);
    }
  }
  if (object.kind != Type::Kind::Class) {
    return CheckBuiltinMethodValue(expression, object, expected);
  }
  const auto class_info = classes_.find(object.name);
  return class_info == classes_.end() ? InvalidType()
                                      : CheckClassMember(expression, class_info->second, expected);
}

TypeChecker::Type TypeChecker::CheckClassMember(const Expression& expression,
                                                const ClassInfo& owner,
                                                const Type* expected) {
  if (const auto applied = CheckMethodApplication(owner.method_templates, expression, nullptr,
                                                  expected, MethodAccess::Instance)) {
    return *applied;
  }
  const auto* field = owner.FindField(expression.text);
  if (field == nullptr) {
    const auto method = owner.methods.find(expression.text);
    if (method != owner.methods.end()) {
      CheckAccess(method->second.visibility, method->second.member_owner, expression.location);
      if (method->second.is_getter) {
        InvalidateCapturedRefinements();
        return method->second.return_type;
      }
      if (method->second.storage == MemberStorage::Static) {
        AddDiagnostic(expression.location, "static method must be accessed through its class");
      }
      facts_.instance_members.insert_or_assign(
          &expression,
          MemberReference{.owner = method->second.member_owner, .member = expression.text});
      return FunctionValueType(method->second, expected);
    }
    AddDiagnostic(expression.location, "unknown field '" + expression.text + "'");
    return InvalidType();
  }
  CheckAccess(field->visibility, field->owner, expression.location);
  return field->type;
}

void TypeChecker::CheckAccess(Visibility visibility,
                              std::string_view owner,
                              SourceLocation location) {
  if (visibility == Visibility::Private && (owner_ == nullptr || owner_->type.name != owner)) {
    AddDiagnostic(location,
                  "private member is accessible only inside class '" + std::string(owner) + "'");
  }
}

TypeChecker::Type TypeChecker::ResolvePrivateReceiver(const Expression& member, Type receiver) {
  if (!IsPrivateName(member.text)) {
    return receiver;
  }
  if (owner_ == nullptr) {
    AddDiagnostic(member.location, "private member requires its declaring class scope");
    return InvalidType();
  }
  if (member.text == kPrivateConstructorName) {
    AddDiagnostic(member.location, "private constructor cannot be used as an instance member");
    return InvalidType();
  }
  if (receiver.kind != Type::Kind::Dynamic &&
      (receiver.kind != Type::Kind::Class || !CanAssign(owner_->type, NonNullable(receiver)))) {
    AddDiagnostic(member.location, "receiver does not carry the declaring class private members");
    return InvalidType();
  }
  facts_.private_members.insert_or_assign(&member, std::string(owner_->type.name));
  return receiver.nullable ? NullableType(owner_->type) : owner_->type;
}

const TypeChecker::ClassInfo* TypeChecker::StaticOwner(const Expression& member) const {
  const auto& object = *member.object;
  if (object.kind != Expression::Kind::Name || LookupLocal(object.text) != nullptr) {
    return nullptr;
  }
  const auto type = types_.find(object.text);
  if (type == types_.end() || type->second.nullable) {
    return nullptr;
  }
  const auto owner = classes_.find(type->second.name);
  return owner == classes_.end() ? nullptr : &owner->second;
}

void TypeChecker::CheckFieldInitializers(const Class& owner, const ClassInfo& info) {
  owner_ = &info;
  for (const auto& field : owner.fields) {
    if (field.initializer) {
      const auto& expected = field.storage == MemberStorage::Static
                                 ? info.static_fields.at(field.name).type
                                 : info.FindField(field.name)->type;
      const auto checked = facts_.expression_types.find(field.initializer.get());
      const auto actual = checked == facts_.expression_types.end()
                              ? CheckExpression(*field.initializer, &expected)
                              : checked->second;
      RecordConversion(*field.initializer, expected, actual);
      if (IsValid(actual) && !CanAssign(expected, actual)) {
        AddDiagnostic(field.location, "field initializer does not match its declared type");
      }
    } else if (field.storage == MemberStorage::Static ||
               (owner.constructor_kind == ConstructorKind::Generated &&
                !owner.IsConstructorField(field) && !owner.constructor)) {
      AddDiagnostic(field.location,
                    "field requires an initializer or explicit constructor initialization");
    }
  }
  owner_ = nullptr;
}

void TypeChecker::CheckClassBody(const Class& source, const ClassInfo& info) {
  if (!source.native_key.empty() || !merged_classes_.contains(info.type.name)) {
    return;
  }
  const auto errors = ErrorCount(diagnostics_);
  CheckFieldInitializers(source, info);
  for (const auto& method : source.methods) {
    CheckFunction(method, &info);
  }
  if (source.constructor) {
    CheckFunction(*source.constructor, &info);
  }
  if (ErrorCount(diagnostics_) != errors) {
    return;
  }
  const auto* base = info.base_class ? &classes_.at(*info.base_class) : nullptr;
  const bool explicit_super = source.HasExplicitSuperCall();
  if (base != nullptr && !explicit_super &&
      HasUnforwardedParameters(base->constructor.parameters, info.constructor_bindings)) {
    AddDiagnostic(source.constructor ? source.constructor->location : source.location,
                  "derived constructor must call super(...) because the base constructor has "
                  "required parameters");
    return;
  }
  const auto initialization = ConstructorDiagnostics(info, source, info);
  for (const auto& diagnostic : initialization) {
    AddDiagnostic(diagnostic.location, diagnostic.message);
  }
  if (initialization.empty()) {
    initialized_definitions_.insert(&source);
    if (base != nullptr) {
      initialization_receivers_.push_back(info.type.name);
    }
  }
}

Diagnostics TypeChecker::ConstructorDiagnostics(const ClassInfo& receiver,
                                                const Class& source,
                                                const ClassInfo& owner) {
  const auto* base = owner.base_class ? &classes_.at(*owner.base_class) : nullptr;
  if (base != nullptr && &receiver == &owner) {
    absl::flat_hash_set<std::uint32_t> deferred;
    for (const auto& field : source.fields) {
      if (field.storage == MemberStorage::Instance && field.visibility == Visibility::Private &&
          !field.initializer) {
        deferred.insert(owner.FindField(field.name)->index);
      }
    }
    for (const auto& binding : owner.constructor_bindings) {
      if (deferred.empty()) {
        break;
      }
      if (binding.field != nullptr) {
        deferred.erase(binding.field->index);
      }
    }
    if (!deferred.empty()) {
      deferred_definitions_.insert(&source);
    }
  }
  ConstructorObservation observation;
  if (deferred_definitions_.contains(&source)) {
    observation = AnalyzeBaseConstruction(receiver, *base, classes_, class_definitions_);
  }
  const auto* body = source.constructor ? source.constructor->body.get() : nullptr;
  const auto& facts = definitions_.control_flow.contains(body) ? definitions_ : facts_;
  return CheckConstructorInitialization(
      source, {
                  .owner = &owner,
                  .base = base,
                  .lambdas = facts.lambdas,
                  .lambda_indices = &facts.lambda_indices,
                  .control_flow = &facts.control_flow,
                  .has_explicit_super_call = source.HasExplicitSuperCall(),
                  .base_observation = &observation,
              });
}

void TypeChecker::CheckInheritedConstructorInitialization() {
  if (deferred_definitions_.empty()) {
    return;
  }
  for (const auto name : initialization_receivers_) {
    const auto& receiver = classes_.at(name);
    auto parent = receiver.base_class;
    while (parent) {
      const auto& owner = classes_.at(*parent);
      const auto* source = class_definitions_.at(owner.type.name);
      if (deferred_definitions_.contains(source) && initialized_definitions_.contains(source)) {
        for (const auto& diagnostic : ConstructorDiagnostics(receiver, *source, owner)) {
          AddDiagnostic(receiver.declaration->location,
                        "construction of '" + std::string(name) + "': " + diagnostic.message);
        }
      }
      parent = owner.base_class;
    }
  }
}

}  // namespace vela::frontend
