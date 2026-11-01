#include "base/declaration.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/frontend/typing/type_checker.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_set.h>

#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/flow/capture_analysis.h"
#include "compiler/frontend/generics/specialization.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/typing/type_name.h"

namespace vela::frontend {
namespace {

TypeName SelfApplication(const std::string& name,
                         std::span<const TypeParameter> parameters,
                         SourceLocation location) {
  TypeName result{
      .name = name,
      .location = location,
      .kind = TypeName::Kind::Generic,
      .arguments = {},
      .parameter = {},
  };
  result.arguments.reserve(parameters.size());
  for (const auto& parameter : parameters) {
    result.arguments.push_back(ParameterTypeName(parameter, parameter.location));
  }
  return result;
}

}  // namespace

void TypeChecker::RegisterTypeParameters(std::span<const TypeParameter> parameters) {
  for (const auto& parameter : parameters) {
    if (type_parameters_.contains(parameter.identity)) {
      continue;
    }
    const auto name =
        InternTypeName(CanonicalTypeText(ParameterTypeName(parameter, parameter.location)));
    const Type type{
        .kind = Type::Kind::Parameter,
        .name = name,
        .is_abstract = true,
        .parameter = &parameter,
    };
    type_parameters_.emplace(parameter.identity, TypeParameterInfo{
                                                     .declaration = &parameter,
                                                     .type = type,
                                                     .bound = std::nullopt,
                                                     .resolved = false,
                                                 });
    types_.try_emplace(name, type);
  }
}

void TypeChecker::RegisterMethodTemplates(std::span<const Function> methods) {
  for (const auto& method : methods) {
    RegisterTypeParameters(method.type_parameters);
  }
}

void TypeChecker::RegisterTemplateParameters(const Program& program) {
  const auto& templates = program.templates;
  for (const auto& function : templates.functions) {
    RegisterTypeParameters(function.type_parameters);
  }
  for (const auto& owner : templates.classes) {
    RegisterTypeParameters(owner.type_parameters);
    RegisterMethodTemplates(owner.method_templates);
  }
  for (const auto& interface : templates.interfaces) {
    RegisterTypeParameters(interface.type_parameters);
    RegisterMethodTemplates(interface.method_templates);
  }
  for (const auto& alias : templates.aliases) {
    RegisterTypeParameters(alias.type_parameters);
  }
  for (const auto& owner : program.classes) {
    RegisterMethodTemplates(owner.method_templates);
  }
  for (const auto& interface : program.interfaces) {
    RegisterMethodTemplates(interface.method_templates);
  }
}

const TypeChecker::TypeParameterInfo* TypeChecker::FindTypeParameter(const Type& type) const {
  if (type.kind != Type::Kind::Parameter) {
    return nullptr;
  }
  const auto found = type_parameters_.find(type.parameter->identity);
  return found == type_parameters_.end() ? nullptr : &found->second;
}

const TypeChecker::Type* TypeChecker::ResolveParameterBound(const TypeParameterInfo& parameter) {
  if (!parameter.resolved) {
    // 先标记已解析，上界中的自引用只读取参数类型本身，不重入上界解析。
    auto& info = type_parameters_.at(parameter.declaration->identity);
    info.resolved = true;
    if (const auto& bound = info.declaration->bound) {
      const auto type = ResolveType(*bound, VoidTypePolicy::Allowed);
      if (type.kind == Type::Kind::Void || type.kind == Type::Kind::Null) {
        AddDiagnostic(bound->location, "generic bound must be a value type");
      } else if (type.kind == Type::Kind::Dynamic) {
        AddDiagnostic(bound->location,
                      "dynamic cannot be a generic bound; omit 'extends' for an unconstrained "
                      "parameter");
      } else if (IsValid(type)) {
        info.bound = type;
      }
    }
  }
  return parameter.bound ? &*parameter.bound : nullptr;
}

void TypeChecker::ValidateTypeParameters(std::span<const TypeParameter> parameters) {
  for (const auto& parameter : parameters) {
    const auto found = type_parameters_.find(parameter.identity);
    if (found == type_parameters_.end()) {
      continue;
    }
    static_cast<void>(ResolveParameterBound(found->second));
    // 只由参数转发和可空包装组成的上界链没有实际类型构造，回到自身即为非法循环。
    absl::flat_hash_set<TypeParameterId> visited{parameter.identity};
    const auto* current = &found->second;
    while (current->bound && current->bound->kind == Type::Kind::Parameter) {
      const auto* next = FindTypeParameter(*current->bound);
      if (next == nullptr) {
        break;
      }
      if (!visited.insert(next->declaration->identity).second) {
        if (next->declaration->identity == parameter.identity) {
          AddDiagnostic(parameter.location, "generic parameter '" + parameter.name +
                                                "' has a cyclic bound without a type constructor");
        }
        break;
      }
      static_cast<void>(ResolveParameterBound(*next));
      current = next;
    }
  }
}

TypeChecker::Type TypeChecker::ParameterCapability(const Type& type, SourceLocation location) {
  auto current = type;
  std::size_t steps = 0;
  while (current.kind == Type::Kind::Parameter) {
    const auto* parameter = FindTypeParameter(current);
    const auto* bound = parameter == nullptr ? nullptr : ResolveParameterBound(*parameter);
    if (bound == nullptr || ++steps > type_parameters_.size()) {
      const auto name =
          parameter == nullptr ? std::string(current.name) : parameter->declaration->name;
      AddDiagnostic(location, "generic parameter '" + name +
                                  "' needs an 'extends' bound that provides this operation");
      return InvalidType();
    }
    if (current.known_non_null) {
      current = NonNullable(*bound);
    } else {
      current = current.nullable ? NullableType(*bound) : *bound;
    }
  }
  return current;
}

bool TypeChecker::IsPotentiallyNullable(const Type& type) const {
  auto current = type;
  absl::flat_hash_set<TypeParameterId> visited;
  while (true) {
    if (current.kind == Type::Kind::Parameter && current.known_non_null) {
      return false;
    }
    if (current.nullable || current.kind == Type::Kind::Null ||
        current.kind == Type::Kind::Dynamic) {
      return true;
    }
    const auto* parameter = FindTypeParameter(current);
    if (parameter == nullptr) {
      return false;
    }
    if (!parameter->bound || !visited.insert(parameter->declaration->identity).second) {
      return true;
    }
    current = *parameter->bound;
  }
}

void TypeChecker::QueueBoundCheck(std::string owner,
                                  std::span<const TypeParameter> parameters,
                                  std::vector<Type> arguments,
                                  SourceLocation location) {
  const bool bounded = std::ranges::any_of(
      parameters, [](const auto& parameter) { return parameter.bound.has_value(); });
  if (!bounded || arguments.size() != parameters.size()) {
    return;
  }
  bound_checks_.push_back({
      .owner = std::move(owner),
      .parameters = std::vector<TypeParameter>(parameters.begin(), parameters.end()),
      .arguments = std::move(arguments),
      .location = location,
  });
}

bool TypeChecker::CheckTypeArguments(const BoundCheck& check) {
  TypeSubstitutions substitutions;
  for (std::size_t index = 0; index < check.parameters.size(); ++index) {
    substitutions.emplace(check.parameters.at(index).identity,
                          DescribeType(check.arguments.at(index)));
  }
  bool satisfied = true;
  for (std::size_t index = 0; index < check.parameters.size(); ++index) {
    const auto& parameter = check.parameters.at(index);
    if (!parameter.bound) {
      continue;
    }
    const auto bound =
        ResolveType(SubstituteTypeName(*parameter.bound, substitutions), VoidTypePolicy::Allowed);
    const auto& argument = check.arguments.at(index);
    if (!IsValid(bound) || !IsValid(argument)) {
      satisfied = false;
      continue;
    }
    if (!IsSubtype(argument, bound)) {
      AddDiagnostic(check.location, "type argument '" + TypeNameFor(argument) +
                                        "' does not satisfy the bound '" + TypeNameFor(bound) +
                                        "' of generic parameter '" + parameter.name + "' in '" +
                                        check.owner + "'");
      satisfied = false;
    }
  }
  return satisfied;
}

void TypeChecker::RegisterApplications() {
  for (auto& application : specializer_->TakeApplications()) {
    const auto key = SpecializationName(application.name, application.arguments);
    if (!checked_applications_.insert(key).second) {
      continue;
    }
    std::span<const TypeParameter> parameters;
    if (const auto* function = specializer_->FunctionTemplate(application.name)) {
      parameters = function->type_parameters;
    } else if (const auto* owner = specializer_->ClassTemplate(application.name)) {
      parameters = owner->type_parameters;
    } else if (const auto* interface = specializer_->InterfaceTemplate(application.name)) {
      parameters = interface->type_parameters;
    } else if (const auto* alias = specializer_->AliasTemplate(application.name)) {
      parameters = alias->type_parameters;
    }
    if (parameters.empty() || parameters.size() != application.arguments.size() ||
        std::ranges::none_of(parameters,
                             [](const auto& parameter) { return parameter.bound.has_value(); })) {
      continue;
    }
    pending_applications_.push_back({
        .owner = application.name,
        .parameters = std::vector<TypeParameter>(parameters.begin(), parameters.end()),
        .arguments = std::move(application.arguments),
        .location = application.location,
    });
  }
}

bool TypeChecker::DrainBoundChecks() {
  RegisterApplications();
  bool progressed = false;
  while (!pending_applications_.empty() || !bound_checks_.empty()) {
    progressed = true;
    while (!pending_applications_.empty()) {
      auto application = std::move(pending_applications_.back());
      pending_applications_.pop_back();
      std::vector<Type> arguments;
      arguments.reserve(application.arguments.size());
      for (const auto& argument : application.arguments) {
        arguments.push_back(ResolveType(argument, VoidTypePolicy::Allowed));
      }
      QueueBoundCheck(std::move(application.owner), application.parameters, std::move(arguments),
                      application.location);
    }
    while (!bound_checks_.empty()) {
      auto check = std::move(bound_checks_.back());
      bound_checks_.pop_back();
      static_cast<void>(CheckTypeArguments(check));
    }
    RegisterApplications();
  }
  return progressed;
}

void TypeChecker::RegisterInterface(const Interface& source, bool abstract) {
  InterfaceDescriptor interface;
  interface.type = {
      .kind = Type::Kind::Interface,
      .name = InternTypeName(source.name),
      .is_abstract = abstract,
  };
  types_.try_emplace(source.name, interface.type);
  // 抽象应用只供定义检查读取签名，不占用运行时接口编号。
  if (!abstract) {
    interface.index = next_interface_index_++;
  }
  for (const auto& method : source.methods) {
    if (!interface.methods.emplace(method.name, ResolveSignature(method)).second) {
      AddDiagnostic(method.location, "duplicate interface method '" + method.name + "'");
    }
  }
  for (const auto& method : source.method_templates) {
    if (interface.methods.contains(method.name) ||
        !interface.method_templates
             .emplace(method.name, MethodTemplate{.owner = source.name, .declaration = &method})
             .second) {
      AddDiagnostic(method.location, "duplicate interface method '" + method.name + "'");
    }
  }
  interfaces_.insert_or_assign(source.name, std::move(interface));
}

void TypeChecker::CheckTemplateFunction(const Function& function, const ClassInfo* owner) {
  const auto errors = ErrorCount(diagnostics_);
  const auto captured = FindCapturedMutableDeclarations(function);
  captured_declarations_.insert(captured.begin(), captured.end());
  const auto signature = ResolveSignature(function);
  CheckFunctionBody(function, signature, owner);
  if (ErrorCount(diagnostics_) != errors) {
    failed_templates_.insert(&function);
  }
}

void TypeChecker::CheckTemplateClass(const Class& source) {
  const auto errors = ErrorCount(diagnostics_);
  ValidateTypeParameters(source.type_parameters);
  for (const auto& method : source.method_templates) {
    ValidateTypeParameters(method.type_parameters);
  }
  const auto self =
      ResolveType(SelfApplication(source.name, source.type_parameters, source.location),
                  VoidTypePolicy::Forbidden);
  const auto found = classes_.find(self.name);
  if (IsValid(self) && found != classes_.end()) {
    const auto& info = found->second;
    const auto captured = FindCapturedMutableDeclarations(source);
    captured_declarations_.insert(captured.begin(), captured.end());
    CheckClassBody(source, info);
    for (const auto& method : source.method_templates) {
      CheckTemplateFunction(method, &info);
    }
  }
  if (ErrorCount(diagnostics_) != errors) {
    failed_templates_.insert(&source);
  }
}

void TypeChecker::CheckDefinitions(Program& program) {
  // 模板事实与具体声明事实分开记录；具体特化仍由各自的函数体检查产生执行事实。
  auto concrete = std::exchange(facts_, {});
  auto concrete_builtins = std::exchange(builtin_function_ids_, {});
  checking_definitions_ = true;
  auto& templates = program.templates;
  for (const auto& alias : templates.aliases) {
    ValidateTypeParameters(alias.type_parameters);
    static_cast<void>(ResolveType(alias.target, VoidTypePolicy::Allowed));
  }
  for (const auto& interface : templates.interfaces) {
    ValidateTypeParameters(interface.type_parameters);
    for (const auto& method : interface.method_templates) {
      ValidateTypeParameters(method.type_parameters);
      static_cast<void>(ResolveSignature(method));
    }
    static_cast<void>(
        ResolveType(SelfApplication(interface.name, interface.type_parameters, interface.location),
                    VoidTypePolicy::Forbidden));
  }
  for (const auto& function : templates.functions) {
    ValidateTypeParameters(function.type_parameters);
    specializer_->SetExpansionOwner(function.name);
    CheckTemplateFunction(function, nullptr);
  }
  for (const auto& owner : templates.classes) {
    specializer_->SetExpansionOwner(owner.name);
    CheckTemplateClass(owner);
  }
  for (const auto& owner : program.classes) {
    const auto origin = program.origins.find(owner.name);
    const auto info = classes_.find(owner.name);
    // 具体特化中的方法模板是模板方法替换所属参数后的副本，其定义已随模板检查。
    if (owner.method_templates.empty() || info == classes_.end() ||
        (origin != program.origins.end() && !origin->second.arguments.empty())) {
      continue;
    }
    for (const auto& method : owner.method_templates) {
      ValidateTypeParameters(method.type_parameters);
      specializer_->SetExpansionOwner(owner.name);
      CheckTemplateFunction(method, &info->second);
    }
  }
  for (const auto& interface : program.interfaces) {
    const auto origin = program.origins.find(interface.name);
    if (origin != program.origins.end() && !origin->second.arguments.empty()) {
      continue;
    }
    for (const auto& method : interface.method_templates) {
      ValidateTypeParameters(method.type_parameters);
      static_cast<void>(ResolveSignature(method));
    }
  }
  definitions_ = std::exchange(facts_, {});
  facts_ = std::move(concrete);
  builtin_function_ids_ = std::move(concrete_builtins);
  checking_definitions_ = false;
}

void TypeChecker::CheckPendingSpecializations() {
  // 新特化只排入工作队列，函数体按声明作用域检查，不递归借用调用方局部变量。
  while (checked_functions_ < specializer_->function_count() ||
         checked_classes_ < specializer_->class_count() ||
         checked_methods_ < specializer_->method_count()) {
    while (checked_functions_ < specializer_->function_count()) {
      const auto index = checked_functions_++;
      const auto& function = specializer_->function(index);
      if (failed_templates_.contains(specializer_->FunctionSource(index))) {
        continue;
      }
      specializer_->SetExpansionOwner(function.name);
      CheckFunction(function, nullptr);
    }
    while (checked_classes_ < specializer_->class_count()) {
      const auto index = checked_classes_++;
      if (specializer_->IsAbstractClass(index) ||
          failed_templates_.contains(specializer_->ClassSource(index))) {
        continue;
      }
      const auto& source = specializer_->class_declaration(index);
      specializer_->SetExpansionOwner(source.name);
      CheckClassBody(source, classes_.at(source.name));
    }
    while (checked_methods_ < specializer_->method_count()) {
      const auto index = checked_methods_++;
      if (specializer_->method_owner_kind(index) == DeclarationKind::Interface ||
          failed_templates_.contains(specializer_->method_source(index))) {
        continue;
      }
      const auto& method = specializer_->method(index);
      const auto owner = std::string(specializer_->method_owner(index));
      specializer_->SetExpansionOwner(owner + '.' + method.name);
      CheckFunction(method, &classes_.at(owner));
    }
  }
}

TypeChecker::Type TypeChecker::ResolveClassInstantiation(const TypeName& type_name) {
  std::vector<TypeName> arguments;
  for (const auto& argument : type_name.arguments) {
    const auto type = ResolveType(argument, VoidTypePolicy::Allowed);
    if (!IsValid(type)) {
      return InvalidType();
    }
    arguments.push_back(DescribeType(type));
  }
  const auto name = specializer_->Specialize(type_name.name, arguments, type_name.location);
  RegisterSpecializations();
  const auto found = types_.find(name);
  return found == types_.end() ? InvalidType() : found->second;
}

void TypeChecker::SetConstructorParameters(const Class& owner, InferenceSignature& source) {
  if (owner.constructor_kind == ConstructorKind::Explicit) {
    source.parameters = owner.constructor->parameters;
  } else {
    for (const auto& field : owner.fields) {
      if (owner.IsConstructorField(field)) {
        source.parameters.push_back(
            {.type = field.type, .name = field.name, .location = field.location});
        if (field.initializer) {
          source.optional_parameters.insert(field.name);
        }
      }
    }
  }
}

void TypeChecker::RegisterSpecializations() {
  for (auto& diagnostic : specializer_->TakeDiagnostics()) {
    diagnostics_.push_back(std::move(diagnostic));
  }
  const auto class_begin = registered_classes_;
  const auto class_end = specializer_->class_count();
  registered_classes_ = class_end;
  const auto interface_begin = registered_interfaces_;
  const auto interface_end = specializer_->interface_count();
  registered_interfaces_ = interface_end;
  for (auto index = interface_begin; index < interface_end; ++index) {
    const auto& source = specializer_->interface_declaration(index);
    RegisterMethodTemplates(source.method_templates);
    RegisterInterface(source, specializer_->IsAbstractInterface(index));
  }
  for (auto index = class_begin; index < class_end; ++index) {
    const auto& source = specializer_->class_declaration(index);
    RegisterMethodTemplates(source.method_templates);
    const Type type{
        .kind = Type::Kind::Class,
        .name = InternTypeName(source.name),
        .is_abstract = specializer_->IsAbstractClass(index),
    };
    types_.try_emplace(source.name, type);
    ClassInfo info;
    info.type = type;
    classes_.try_emplace(source.name, std::move(info));
  }
  for (auto index = class_begin; index < class_end; ++index) {
    const auto& source = specializer_->class_declaration(index);
    ResolveClass(source, classes_.at(source.name));
    const auto captured = FindCapturedMutableDeclarations(source);
    captured_declarations_.insert(captured.begin(), captured.end());
  }
  for (auto index = class_begin; index < class_end; ++index) {
    const auto& source = specializer_->class_declaration(index);
    if (source.base_class) {
      MergeBaseClass(source.name, merged_classes_, visiting_classes_);
    }
    CheckClassImplementation(source);
  }
  while (registered_functions_ < specializer_->function_count()) {
    const auto& function = specializer_->function(registered_functions_++);
    functions_.try_emplace(function.name, ResolveSignature(function));
    const auto captured = FindCapturedMutableDeclarations(function);
    captured_declarations_.insert(captured.begin(), captured.end());
  }
  RegisterMethodApplications();
}

}  // namespace vela::frontend
