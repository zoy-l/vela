#include "base/builtin.h"
#include "base/member.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/flow/initialization.h"
#include "compiler/frontend/syntax/names.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/frontend/typing/type_checker.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/regexp.h"
#include "compiler/frontend/syntax/ast.h"

#include "compiler/frontend/builtins/async_builtins.h"
#include "compiler/frontend/builtins/builtins.h"
#include "compiler/frontend/builtins/standard_environment.h"
#include "compiler/frontend/typing/type_name.h"
#include "compiler/frontend/typing/type_names.h"

namespace vela::frontend {

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
  for (const Class& class_declaration : program.classes) {
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
  info.constructor.return_type = info.type;
  ResolveClassRelations(class_declaration, info);
  ResolveClassMembers(class_declaration, info);
}

void TypeChecker::ResolveClassRelations(const Class& class_declaration, ClassInfo& info) {
  if (class_declaration.base_class) {
    const auto base = ResolveType(*class_declaration.base_class, VoidTypePolicy::Forbidden);
    if (base.kind != Type::Kind::Class || base.nullable) {
      AddDiagnostic(class_declaration.base_class->location, "extends requires a class type");
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
    ResolveField(field, class_declaration, info);
  }
  ResolveClassMethods(class_declaration, info);
  ResolveClassConstructor(class_declaration, info);
}

void TypeChecker::ResolveClassMethods(const Class& class_declaration, ClassInfo& info) {
  for (const Function& method : class_declaration.methods) {
    ResolveClassMethod(method, info);
  }
  for (const auto& method : class_declaration.method_templates) {
    if (info.fields.contains(method.name) || info.static_fields.contains(method.name) ||
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
  if (info.fields.contains(method.name) || info.static_fields.contains(method.name)) {
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
  CheckAccessor(method);
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
  }
  if (incoming.is_setter && !incoming.parameters.empty()) {
    existing.parameters = incoming.parameters;
    existing.parameter_indices = incoming.parameter_indices;
    existing.setter_parameter_type = incoming.parameters.front().type;
    existing.setter_return_type = incoming.return_type;
  }
}

void TypeChecker::CheckAccessor(const Function& method) {
  if (method.is_getter && !method.parameters.empty()) {
    AddDiagnostic(method.location, "getter must not declare parameters");
  }
  if (method.is_setter &&
      (method.parameters.size() != 1 ||
       ResolveType(method.return_type, VoidTypePolicy::Allowed).kind != Type::Kind::Void)) {
    AddDiagnostic(method.location, "setter must declare one parameter and return void");
  }
}

void TypeChecker::ResolveClassConstructor(const Class& class_declaration, ClassInfo& info) {
  if (!class_declaration.constructor) {
    return;
  }
  const auto signature = ResolveSignature(*class_declaration.constructor);
  info.methods.emplace(kConstructorName, signature);
  info.constructor.visibility = class_declaration.constructor->visibility;
  if (class_declaration.constructor_kind == ConstructorKind::Explicit) {
    info.constructor = signature;
    info.constructor.return_type = info.type;
  }
}

void TypeChecker::ResolveField(const Field& field,
                               const Class& class_declaration,
                               ClassInfo& info) {
  const Type type = ResolveType(field.type, VoidTypePolicy::Forbidden);
  const ClassDescriptor::Field descriptor{
      .type = type,
      .index = static_cast<std::uint32_t>(info.field_order.size()),
      .visibility = field.visibility,
  };
  if (field.storage == MemberStorage::Static) {
    if (!info.static_fields.emplace(field.name, descriptor).second ||
        info.fields.contains(field.name)) {
      AddDiagnostic(field.location, "duplicate field '" + field.name + "'");
    }
    return;
  }
  if (info.static_fields.contains(field.name)) {
    AddDiagnostic(field.location, "duplicate field '" + field.name + "'");
  }
  if (!info.fields.emplace(field.name, descriptor).second) {
    AddDiagnostic(field.location, "duplicate field '" + field.name + "'");
  } else {
    info.field_order.push_back(field.name);
  }
  if (class_declaration.IsConstructorField(field)) {
    using Presence = FunctionDescriptor::ParameterPresence;
    info.constructor.AddParameter(field.name, type,
                                  field.initializer ? Presence::Optional : Presence::Required);
  }
}

TypeChecker::Type TypeChecker::CheckClassConstruction(
    const ClassInfo& class_info,
    const Expression& expression,
    std::span<const std::optional<Type>> checked_arguments) {
  CheckAccess(class_info.constructor.visibility, class_info.type.name, expression.location);
  for (const CallArgument& argument : expression.arguments) {
    if (class_info.declaration->constructor_kind == ConstructorKind::Generated &&
        argument.name.empty()) {
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
  if (expression.object->kind == Expression::Kind::Name &&
      LookupLocal(expression.object->text) == nullptr) {
    const auto enumeration = types_.find(expression.object->text);
    if (enumeration != types_.end() && enumeration->second.kind == Type::Kind::Enum) {
      if (enum_values_.contains(std::string(enumeration->second.name) + "." + expression.text)) {
        return enumeration->second;
      }
      AddDiagnostic(expression.location, "unknown enum member '" + expression.text + "'");
      return InvalidType();
    }
  }

  auto object = CheckExpression(*expression.object, nullptr);
  object = ParameterCapability(object, expression.object->location);
  if (object.nullable) {
    if (!expression.null_safe) {
      AddDiagnostic(expression.object->location, "nullable receiver requires a non-null check");
      return InvalidType();
    }
    auto result = CheckObjectMember(expression, NonNullable(object), expected);
    return IsValid(result) ? NullableType(result) : result;
  }
  return CheckObjectMember(expression, object, expected);
}

TypeChecker::Type TypeChecker::CheckSuperMember(const Expression& expression,
                                                const Type* expected) {
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
  return method->second.is_getter ? method->second.return_type : FunctionType(method->second);
}

TypeChecker::Type TypeChecker::CheckStaticMember(const Expression& expression,
                                                 const ClassInfo& owner,
                                                 const Type* expected) {
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
    return FunctionType(method->second);
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
    if (IsRegExpBuiltin(*builtin)) {
      return BuiltinSignature(*builtin).return_type;
    }
    return {.kind = Type::Kind::Int, .name = kIntTypeName};
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
      return FunctionType(method->second);
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
  const auto field = owner.fields.find(expression.text);
  if (field == owner.fields.end()) {
    const auto method = owner.methods.find(expression.text);
    if (method != owner.methods.end()) {
      if (method->second.is_getter) {
        CheckAccess(method->second.visibility, method->second.member_owner, expression.location);
        InvalidateCapturedRefinements();
        return method->second.return_type;
      }
      CheckAccess(method->second.visibility, method->second.member_owner, expression.location);
      if (method->second.storage == MemberStorage::Static) {
        AddDiagnostic(expression.location, "static method must be accessed through its class");
      }
      facts_.instance_members.insert_or_assign(
          &expression,
          MemberReference{.owner = method->second.member_owner, .member = expression.text});
      return FunctionType(method->second);
    }
    AddDiagnostic(expression.location, "unknown field '" + expression.text + "'");
    return InvalidType();
  }
  CheckAccess(field->second.visibility, owner.type.name, expression.location);
  return field->second.type;
}

void TypeChecker::CheckAccess(Visibility visibility,
                              std::string_view owner,
                              SourceLocation location) {
  if (visibility == Visibility::Private && (owner_ == nullptr || owner_->type.name != owner)) {
    AddDiagnostic(location,
                  "private member is accessible only inside class '" + std::string(owner) + "'");
  }
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
                                 : info.fields.at(field.name).type;
      const auto actual = CheckExpression(*field.initializer, &expected);
      RecordConversion(*field.initializer, expected, actual);
      if (IsValid(actual) && !CanAssign(expected, actual)) {
        AddDiagnostic(field.location, "field initializer does not match its declared type");
      }
    } else if (field.storage == MemberStorage::Static ||
               (owner.constructor_kind == ConstructorKind::Generated &&
                !owner.IsConstructorField(field))) {
      AddDiagnostic(field.location,
                    "field requires an initializer or explicit constructor initialization");
    }
  }
  owner_ = nullptr;
}

void TypeChecker::CheckClassBody(const Class& source, const ClassInfo& info) {
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
  const bool explicit_super = explicit_super_constructors_.contains(info.type.name);
  if (base != nullptr && !explicit_super &&
      std::ranges::any_of(base->constructor.parameters, [&](const auto& parameter) {
        return !base->constructor.optional_parameters.contains(parameter.name);
      })) {
    AddDiagnostic(source.constructor ? source.constructor->location : source.location,
                  "derived constructor must call super(...) because the base constructor has "
                  "required parameters");
    return;
  }
  const auto initialization =
      CheckConstructorInitialization(source, {
                                                 .owner = &info,
                                                 .base = base,
                                                 .lambdas = facts_.lambdas,
                                                 .lambda_indices = &facts_.lambda_indices,
                                                 .control_flow = &facts_.control_flow,
                                                 .has_explicit_super_call = explicit_super,
                                             });
  for (const auto& diagnostic : initialization) {
    AddDiagnostic(diagnostic.location, diagnostic.message);
  }
}

}  // namespace vela::frontend
