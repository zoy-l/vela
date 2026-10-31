#include "compiler/frontend/type_checker.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_set.h>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/capture_analysis.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/specialization.h"
#include "compiler/frontend/type_name.h"

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
      const auto type = ResolveType(*bound, true);
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
    const auto bound = ResolveType(SubstituteTypeName(*parameter.bound, substitutions), true);
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
        arguments.push_back(ResolveType(argument, true));
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

NodeFacts TypeChecker::TakeNodeFacts() {
  return {
      .lambdas = std::exchange(lambdas_, {}),
      .lambda_indices = std::exchange(lambda_indices_, {}),
      .bindings = std::exchange(bindings_, {}),
      .expression_types = std::exchange(expression_types_, {}),
      .generic_applications = std::exchange(generic_applications_, {}),
      .checked_conversions = std::exchange(checked_conversions_, {}),
      .case_values = std::exchange(case_values_, {}),
      .builtin_calls = std::exchange(builtin_calls_, {}),
      .async_calls = std::exchange(async_calls_, {}),
      .generated_functions = std::exchange(generated_functions_, {}),
      .builtin_method_values = std::exchange(builtin_method_values_, {}),
      .array_algorithms = std::exchange(array_algorithms_, {}),
      .local_types = std::exchange(local_types_, {}),
      .control_flow = std::exchange(control_flow_, {}),
      .static_members = std::exchange(static_members_, {}),
      .instance_members = std::exchange(instance_members_, {}),
      .interface_members = std::exchange(interface_members_, {}),
      .super_members = std::exchange(super_members_, {}),
      .type_operands = std::exchange(type_operands_, {}),
      .match_bindings = std::exchange(match_bindings_, {}),
      .catch_types = std::exchange(catch_types_, {}),
      .argument_order = std::exchange(argument_order_, {}),
      .collection_constructions = std::exchange(collection_constructions_, {}),
  };
}

void TypeChecker::RestoreNodeFacts(NodeFacts facts) {
  lambdas_ = std::move(facts.lambdas);
  lambda_indices_ = std::move(facts.lambda_indices);
  bindings_ = std::move(facts.bindings);
  expression_types_ = std::move(facts.expression_types);
  generic_applications_ = std::move(facts.generic_applications);
  checked_conversions_ = std::move(facts.checked_conversions);
  case_values_ = std::move(facts.case_values);
  builtin_calls_ = std::move(facts.builtin_calls);
  async_calls_ = std::move(facts.async_calls);
  generated_functions_ = std::move(facts.generated_functions);
  builtin_method_values_ = std::move(facts.builtin_method_values);
  array_algorithms_ = std::move(facts.array_algorithms);
  local_types_ = std::move(facts.local_types);
  control_flow_ = std::move(facts.control_flow);
  static_members_ = std::move(facts.static_members);
  instance_members_ = std::move(facts.instance_members);
  interface_members_ = std::move(facts.interface_members);
  super_members_ = std::move(facts.super_members);
  type_operands_ = std::move(facts.type_operands);
  match_bindings_ = std::move(facts.match_bindings);
  catch_types_ = std::move(facts.catch_types);
  argument_order_ = std::move(facts.argument_order);
  collection_constructions_ = std::move(facts.collection_constructions);
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
    interface.index =
        interface_ids_ == nullptr ? next_interface_index_++ : interface_ids_->Get(source.name);
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
      ResolveType(SelfApplication(source.name, source.type_parameters, source.location), false);
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
  auto concrete = TakeNodeFacts();
  auto concrete_builtins = std::exchange(builtin_function_ids_, {});
  checking_definitions_ = true;
  auto& templates = program.templates;
  for (const auto& alias : templates.aliases) {
    ValidateTypeParameters(alias.type_parameters);
    static_cast<void>(ResolveType(alias.target, true));
  }
  for (const auto& interface : templates.interfaces) {
    ValidateTypeParameters(interface.type_parameters);
    for (const auto& method : interface.method_templates) {
      ValidateTypeParameters(method.type_parameters);
      static_cast<void>(ResolveSignature(method));
    }
    static_cast<void>(ResolveType(
        SelfApplication(interface.name, interface.type_parameters, interface.location), false));
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
  definitions_ = TakeNodeFacts();
  RestoreNodeFacts(std::move(concrete));
  builtin_function_ids_ = std::move(concrete_builtins);
  checking_definitions_ = false;
}

}  // namespace vela::frontend
