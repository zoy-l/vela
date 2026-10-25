#include "compiler/frontend/type_checker.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "compiler/frontend/ast.h"

#include "compiler/frontend/builtins.h"

namespace vela::frontend {

void TypeChecker::CollectTypes(const Program& program) {
  types_.emplace("void", Type{.kind = Type::Kind::Void, .name = "void"});
  types_.emplace("bool", Type{.kind = Type::Kind::Bool, .name = "bool"});
  types_.emplace("int", Type{.kind = Type::Kind::Int, .name = "int"});
  types_.emplace("double", Type{.kind = Type::Kind::Double, .name = "double"});
  types_.emplace("string", Type{.kind = Type::Kind::String, .name = "string"});
  types_.emplace("String", Type{.kind = Type::Kind::MutableString, .name = "String"});
  static_cast<void>(ArrayType(types_.at("string")));
  static_cast<void>(ArrayType(types_.at("int")));
  types_.reserve(types_.size() + program.enums.size() + program.classes.size());
  classes_.reserve(program.classes.size());
  for (const auto& interface : program.interfaces) {
    const Type type{.kind = Type::Kind::Interface, .name = interface.name};
    if (IsBuiltinName(interface.name) || !types_.emplace(interface.name, type).second) {
      AddDiagnostic(interface.location, "duplicate or reserved interface '" + interface.name + "'");
    }
  }

  for (const Enum& enumeration : program.enums) {
    if (IsBuiltinName(enumeration.name)) {
      AddDiagnostic(enumeration.location, "type name is reserved for a builtin");
      continue;
    }
    const Type type{.kind = Type::Kind::Enum, .name = enumeration.name};
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
    if (IsBuiltinName(class_declaration.name)) {
      AddDiagnostic(class_declaration.location, "type name is reserved for a builtin");
      continue;
    }
    const Type type{.kind = Type::Kind::Class, .name = class_declaration.name};
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
  for (const auto& interface : class_declaration.interfaces) {
    const auto type = ResolveType(interface, false);
    if (type.kind != Type::Kind::Interface || type.nullable) {
      AddDiagnostic(interface.location, "implements requires an interface type");
    } else if (!info.interfaces.insert(type.name).second) {
      AddDiagnostic(interface.location, "duplicate implemented interface");
    }
  }
  info.fields.reserve(class_declaration.fields.size());
  info.field_order.reserve(class_declaration.fields.size());
  info.methods.reserve(class_declaration.methods.size());
  for (const Field& field : class_declaration.fields) {
    ResolveField(field, class_declaration, info);
  }
  for (const Function& method : class_declaration.methods) {
    FunctionInfo method_info = ResolveSignature(method);
    if (info.fields.contains(method.name) || info.static_fields.contains(method.name)) {
      AddDiagnostic(method.location, "member name is already used by a field");
    }
    if (!info.methods.emplace(method.name, std::move(method_info)).second) {
      AddDiagnostic(method.location, "duplicate method '" + method.name + "'");
    }
  }
  if (class_declaration.constructor) {
    const auto signature = ResolveSignature(*class_declaration.constructor);
    info.methods.emplace("$constructor", signature);
    info.constructor.visibility = class_declaration.constructor->visibility;
    if (class_declaration.constructor_kind == ConstructorKind::Explicit) {
      info.constructor = signature;
      info.constructor.return_type = info.type;
    }
  }
}

void TypeChecker::ResolveField(const Field& field,
                               const Class& class_declaration,
                               ClassInfo& info) {
  const Type type = ResolveType(field.type, false);
  const ClassDescriptor::Field descriptor{
      .type = type,
      .index = static_cast<std::uint32_t>(info.field_order.size()),
      .visibility = field.visibility,
  };
  if (field.storage == MemberStorage::Static) {
    if (field.role == FieldRole::Parameter) {
      AddDiagnostic(field.location, "static fields cannot use param");
    }
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
  if (field.role == FieldRole::Parameter) {
    if (class_declaration.constructor_kind == ConstructorKind::Explicit) {
      AddDiagnostic(
          field.location,
          "param fields and an explicit constructor parameter list are mutually exclusive");
    }
    info.constructor.parameter_indices.emplace(
        field.name, static_cast<std::uint32_t>(info.constructor.parameters.size()));
    info.constructor.parameters.push_back({.name = field.name, .type = type});
    if (field.initializer) {
      info.constructor.optional_parameters.insert(field.name);
    }
  }
}

TypeChecker::Type TypeChecker::CheckClassConstruction(const ClassInfo& class_info,
                                                      const Expression& expression) {
  CheckAccess(class_info.constructor.visibility, class_info.type.name, expression.location);
  for (const CallArgument& argument : expression.arguments) {
    if (class_info.declaration->constructor_kind == ConstructorKind::Generated &&
        argument.name.empty()) {
      AddDiagnostic(argument.location, "param constructor arguments must be named");
    }
  }
  return CheckFunctionCall(class_info.constructor, expression);
}

TypeChecker::Type TypeChecker::CheckMemberAccess(const Expression& expression) {
  if (const auto* owner = StaticOwner(expression)) {
    return CheckStaticMember(expression, *owner);
  }
  if (expression.object->kind == Expression::Kind::Name &&
      LookupLocal(expression.object->text) == nullptr) {
    const auto enumeration = types_.find(expression.object->text);
    if (enumeration != types_.end() && enumeration->second.kind == Type::Kind::Enum) {
      if (enum_values_.contains(enumeration->second.name + "." + expression.text)) {
        return enumeration->second;
      }
      AddDiagnostic(expression.location, "unknown enum member '" + expression.text + "'");
      return InvalidType();
    }
  }

  const Type object = CheckExpression(*expression.object, nullptr);
  if (object.nullable) {
    AddDiagnostic(expression.object->location, "nullable receiver requires a non-null check");
    return InvalidType();
  }
  return CheckObjectMember(expression, object);
}

TypeChecker::Type TypeChecker::CheckStaticMember(const Expression& expression,
                                                 const ClassInfo& owner) {
  const auto field = owner.static_fields.find(expression.text);
  if (field != owner.static_fields.end()) {
    CheckAccess(field->second.visibility, owner.type.name, expression.location);
    static_members_.insert_or_assign(
        &expression, MemberReference{.owner = owner.type.name, .member = expression.text});
    return field->second.type;
  }
  const auto method = owner.methods.find(expression.text);
  if (method != owner.methods.end() && method->second.storage == MemberStorage::Static) {
    CheckAccess(method->second.visibility, owner.type.name, expression.location);
    static_members_.insert_or_assign(
        &expression, MemberReference{.owner = owner.type.name, .member = expression.text});
    return FunctionType(method->second);
  }
  AddDiagnostic(expression.location, "class access requires a declared static member");
  return InvalidType();
}

TypeChecker::Type TypeChecker::CheckObjectMember(const Expression& expression, const Type& object) {
  if (const auto builtin = FindBuiltinProperty(object.kind, expression.text)) {
    builtin_calls_.insert_or_assign(&expression, *builtin);
    return {.kind = Type::Kind::Int, .name = "int"};
  }
  if (object.kind == Type::Kind::Interface) {
    const auto& owner = interfaces_.at(object.name);
    const auto method = owner.methods.find(expression.text);
    if (method != owner.methods.end()) {
      interface_members_.insert_or_assign(
          &expression, MemberReference{.owner = object.name, .member = expression.text});
      return FunctionType(method->second);
    }
  }
  if (object.kind != Type::Kind::Class) {
    AddDiagnostic(expression.location, "member access requires a class object");
    return InvalidType();
  }
  const auto class_info = classes_.find(object.name);
  if (class_info == classes_.end()) {
    return InvalidType();
  }
  const auto field = class_info->second.fields.find(expression.text);
  if (field == class_info->second.fields.end()) {
    const auto method = class_info->second.methods.find(expression.text);
    if (method != class_info->second.methods.end()) {
      CheckAccess(method->second.visibility, object.name, expression.location);
      if (method->second.storage == MemberStorage::Static) {
        AddDiagnostic(expression.location, "static method must be accessed through its class");
      }
      return FunctionType(method->second);
    }
    AddDiagnostic(expression.location, "unknown field '" + expression.text + "'");
    return InvalidType();
  }
  CheckAccess(field->second.visibility, object.name, expression.location);
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
  if (type == types_.end() || type->second.kind != Type::Kind::Class || type->second.nullable) {
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
      if (IsValid(actual) && !CanAssign(expected, actual)) {
        AddDiagnostic(field.location, "field initializer does not match its declared type");
      }
    } else if (field.storage == MemberStorage::Static ||
               (owner.constructor_kind == ConstructorKind::Generated &&
                field.role != FieldRole::Parameter)) {
      AddDiagnostic(field.location,
                    "field requires an initializer or explicit constructor initialization");
    }
  }
  owner_ = nullptr;
}

}  // namespace vela::frontend
