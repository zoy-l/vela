#include "compiler/frontend/type_checker.h"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "base/declaration.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/capture_analysis.h"
#include "compiler/frontend/control_flow.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/specialization.h"
#include "compiler/frontend/type_names.h"

namespace vela::frontend {

const MethodTemplate* TypeChecker::InferenceMethod(const Expression& member) {
  const ClassInfo* owner = StaticOwner(member);
  if (owner == nullptr && member.object->kind == Expression::Kind::Name &&
      member.object->text == "super") {
    owner = owner_ != nullptr && owner_->base_class ? &classes_.at(*owner_->base_class) : nullptr;
  }
  if (owner != nullptr) {
    const auto method = owner->method_templates.find(member.text);
    return method == owner->method_templates.end() ? nullptr : &method->second;
  }
  Type receiver;
  if (const auto found = expression_types_.find(member.object.get());
      found != expression_types_.end()) {
    receiver = found->second;
  } else if (member.object->kind == Expression::Kind::Name) {
    if (const auto* binding = LookupLocal(member.object->text)) {
      const auto* refined = refinements_.Find(binding->id);
      receiver = refined == nullptr ? binding->type : *refined;
    }
  }
  if (receiver.kind == Type::Kind::Parameter) {
    receiver = ParameterCapability(receiver, member.object->location);
  }
  receiver = NonNullable(receiver);
  if (receiver.kind == Type::Kind::Class) {
    const auto& methods = classes_.at(receiver.name).method_templates;
    const auto method = methods.find(member.text);
    return method == methods.end() ? nullptr : &method->second;
  }
  if (receiver.kind == Type::Kind::Interface) {
    const auto& methods = interfaces_.at(receiver.name).method_templates;
    const auto method = methods.find(member.text);
    return method == methods.end() ? nullptr : &method->second;
  }
  return nullptr;
}

void TypeChecker::PrepareInferenceReceiver(const Expression& expression) {
  if (expression.kind == Expression::Kind::Match) {
    if (!prepared_matches_.contains(&expression)) {
      static_cast<void>(CheckMatch(expression, nullptr, MatchPhase::Prepare));
    }
    return;
  }
  if (expression.kind == Expression::Kind::Binary && expression.text == "??") {
    PrepareInferenceReceiver(*expression.left);
    InvalidateAssignments(*expression.left);
    PrepareInferenceReceiver(*expression.right);
    return;
  }
  if (expression.kind == Expression::Kind::ArrayLiteral) {
    for (const auto& element : expression.elements) {
      PrepareInferenceReceiver(*element);
      InvalidateAssignments(*element);
    }
    return;
  }
  if (expression.kind == Expression::Kind::Conditional) {
    PrepareInferenceExpression(*expression.operand);
    const auto true_branch = refinements_.BeginBranch();
    RefineCondition(*expression.operand, ConditionTruth::AlwaysTrue);
    PrepareInferenceReceiver(*expression.left);
    auto true_facts = refinements_.Capture(true_branch);
    const auto false_branch = refinements_.BeginBranch();
    RefineCondition(*expression.operand, ConditionTruth::AlwaysFalse);
    PrepareInferenceReceiver(*expression.right);
    const auto false_facts = refinements_.Capture(false_branch);
    refinements_.Intersect(true_facts, false_facts);
    refinements_.Apply(true_facts);
    return;
  }
  const auto& member = expression.kind == Expression::Kind::Call ? *expression.callee : expression;
  if (member.kind != Expression::Kind::Member || StaticOwner(member) != nullptr ||
      (member.object->kind == Expression::Kind::Name && member.object->text == "super") ||
      prepared_expressions_.contains(member.object.get()) ||
      !CanCheckExpression(*member.object, nullptr)) {
    return;
  }
  const auto& receiver = *member.object;
  if (receiver.kind == Expression::Kind::Name && LookupLocal(receiver.text) == nullptr &&
      (receiver.text == kFutureTypeName || receiver.text == kTimerTypeName)) {
    return;
  }
  PrepareInferenceExpression(*member.object);
}

TypeChecker::Type TypeChecker::PrepareInferenceExpression(const Expression& expression) {
  const auto branch = refinements_.BeginBranch();
  const auto type = CheckExpression(expression, nullptr);
  auto changes = refinements_.Capture(branch);
  refinements_.Apply(changes);
  // 保留接收者检查及流效果；上下文就绪后只继续方法具体化，不重新检查源码接收者。
  prepared_expressions_.emplace(
      &expression, PreparedExpression{.type = type, .refinements = std::move(changes)});
  return type;
}

std::optional<std::size_t> TypeChecker::CallbackParameterCount(const Expression& expression) {
  if (expression.kind == Expression::Kind::Lambda) {
    return expression.function->parameters.size();
  }
  if (const auto signature = GenericSignature(expression)) {
    return signature->parameters.size();
  }
  if (expression.kind == Expression::Kind::Conditional ||
      (expression.kind == Expression::Kind::Binary && expression.text == "??")) {
    const auto left = CallbackParameterCount(*expression.left);
    const auto right = CallbackParameterCount(*expression.right);
    if (!left) {
      return right;
    }
    return !right || left == right ? left : std::nullopt;
  }
  std::optional<std::size_t> count;
  if (expression.kind == Expression::Kind::Match) {
    for (const auto& arm : expression.arms) {
      const auto current = CallbackParameterCount(*arm.value);
      if (current && count && current != count) {
        return std::nullopt;
      }
      if (current) {
        count = current;
      }
    }
  }
  return count;
}

TypeChecker::InferenceSignature TypeChecker::FunctionInferenceSignature(const Function& function) {
  InferenceSignature source;
  source.type_parameters = function.type_parameters;
  source.parameters = function.parameters;
  source.return_type = function.return_type;
  source.optional_parameters.insert(function.optional_parameters.begin(),
                                    function.optional_parameters.end());
  NormalizeInferenceSignature(source);
  return source;
}

void TypeChecker::NormalizeInferenceSignature(InferenceSignature& source) {
  const auto normalize = [this](TypeName type) {
    return specializer_->DescribeType(specializer_->ExpandAlias(std::move(type)));
  };
  source.return_type = normalize(std::move(source.return_type));
  for (auto& parameter : source.parameters) {
    parameter.type = normalize(std::move(parameter.type));
  }
}

std::expected<MemberReference, Diagnostic> TypeChecker::MaterializeMethod(
    const MethodTemplate& method,
    std::span<const TypeName> arguments,
    SourceLocation location) {
  const auto& type = types_.at(method.owner);
  const auto created = specializer_->SpecializeMethod({
      .owner = method.owner,
      .kind =
          type.kind == Type::Kind::Interface ? DeclarationKind::Interface : DeclarationKind::Class,
      .declaration = method.declaration,
      .arguments = std::vector<TypeName>(arguments.begin(), arguments.end()),
      .location = location,
  });
  if (!created) {
    return std::unexpected(created.error());
  }
  RegisterSpecializations();
  return MemberReference{.owner = method.owner, .member = specializer_->method(*created).name};
}

TypeChecker::Type TypeChecker::CheckGenericMethod(const MethodTemplate& method,
                                                  const Expression& member,
                                                  const Expression* call,
                                                  const Type* expected,
                                                  MethodAccess access) {
  const auto& declaration = *method.declaration;
  CheckAccess(declaration.visibility, method.owner, member.location);
  if ((access == MethodAccess::Static) != (declaration.storage == MemberStorage::Static)) {
    AddDiagnostic(member.location, "static method must be accessed through its class");
    return InvalidType();
  }
  ValidateTypeParameters(declaration.type_parameters);
  const auto source = FunctionInferenceSignature(declaration);
  FunctionInfo signature;
  std::vector<TypeName> arguments;
  std::vector<std::optional<Type>> checked;
  if (call != nullptr) {
    auto inferred = InferInvocation(source, *call, expected, member.type_arguments);
    if (!inferred) {
      return InvalidType();
    }
    arguments = std::move(inferred->type_arguments);
    signature = std::move(inferred->signature);
    checked = std::move(inferred->arguments);
  } else {
    auto inferred = InferFunctionValue(source, member, expected);
    if (!inferred) {
      return InvalidType();
    }
    arguments = std::move(*inferred);
    signature = ResolveAppliedSignature(source, arguments, member);
  }
  signature.member_owner = method.owner;
  signature.storage = declaration.storage;
  signature.visibility = declaration.visibility;
  const bool abstract = checking_definitions_ || types_.at(method.owner).is_abstract ||
                        std::ranges::any_of(arguments, HasTypeParameters);
  if (!abstract) {
    const auto target = MaterializeMethod(method, arguments, member.location);
    if (!target) {
      AddDiagnostic(target.error().location, target.error().message);
      return InvalidType();
    }
    const auto* node = call == nullptr ? &member : call;
    if (types_.at(method.owner).kind == Type::Kind::Interface) {
      interface_members_.insert_or_assign(node, *target);
    } else if (access == MethodAccess::Static) {
      static_members_.insert_or_assign(node, *target);
    } else if (access == MethodAccess::Super) {
      super_members_.insert_or_assign(&member, *target);
    } else {
      instance_members_.insert_or_assign(node, *target);
    }
  }
  const auto function_type = FunctionType(signature);
  expression_types_.insert_or_assign(&member, function_type);
  return call != nullptr ? CheckFunctionCall(signature, *call, checked) : function_type;
}

void TypeChecker::RegisterMethodApplications() {
  while (registered_methods_ < specializer_->method_count()) {
    const auto index = registered_methods_++;
    const auto& method = specializer_->method(index);
    const auto owner = std::string(specializer_->method_owner(index));
    auto signature = ResolveSignature(method);
    signature.member_owner = owner;
    if (specializer_->method_owner_kind(index) == DeclarationKind::Interface) {
      auto& interface = interfaces_.at(owner);
      interface.methods.try_emplace(method.name, std::move(signature));
      interface.method_applications.emplace(method.name, &method);
      const auto implementers = interface_implementers_.find(owner);
      if (implementers != interface_implementers_.end()) {
        for (const auto& implementation : implementers->second) {
          QueueInterfaceBinding(classes_.at(implementation), owner, method);
        }
      }
    } else {
      classes_.at(owner).methods.try_emplace(method.name, std::move(signature));
      const auto captured = FindCapturedMutableDeclarations(method);
      captured_declarations_.insert(captured.begin(), captured.end());
    }
  }
}

void TypeChecker::QueueInterfaceBinding(const ClassInfo& owner,
                                        const std::string& interface,
                                        const Function& method) {
  const auto& application = *method.application;
  if (!owner.interface_method_templates.contains({interface, application.name}) ||
      !queued_interface_bindings_.emplace(owner.type.name, interface, method.name).second) {
    return;
  }
  interface_bindings_.push_back({
      .owner = std::string(owner.type.name),
      .interface = interface,
      .method = method.name,
      .location = method.location,
  });
}

void TypeChecker::DrainInterfaceBindings() {
  while (!interface_bindings_.empty()) {
    auto binding = std::move(interface_bindings_.back());
    interface_bindings_.pop_back();
    auto& owner = classes_.at(binding.owner);
    const auto& method = *interfaces_.at(binding.interface).method_applications.at(binding.method);
    const auto& application = *method.application;
    const auto implementation =
        owner.interface_method_templates.at({binding.interface, application.name});
    const auto target = MaterializeMethod(implementation, application.arguments, binding.location);
    if (!target) {
      AddDiagnostic(target.error().location, target.error().message);
      continue;
    }
    owner.interface_targets.insert_or_assign({binding.interface, binding.method}, *target);
  }
}

void TypeChecker::RestoreMethodSpecializations(std::span<const GenericSpecialization> retained) {
  for (const auto& request : retained) {
    if (!request.method) {
      continue;
    }
    const auto& application = *request.method;
    const auto type = ResolveType(
        {
            .name = request.name,
            .location = {},
            .kind = request.arguments.empty() ? TypeName::Kind::Named : TypeName::Kind::Generic,
            .arguments = request.arguments,
            .parameter = {},
        },
        false);
    if (!IsValid(type)) {
      continue;
    }
    const auto& name = application.name;
    const auto* templates = type.kind == Type::Kind::Class
                                ? &classes_.at(type.name).method_templates
                                : &interfaces_.at(type.name).method_templates;
    const auto method = templates->find(name);
    if (method == templates->end()) {
      continue;
    }
    if (application.arguments.size() != method->second.declaration->type_parameters.size()) {
      AddDiagnostic({},
                    "retained generic method argument count does not match type parameter count");
      continue;
    }
    std::vector<Type> arguments;
    arguments.reserve(application.arguments.size());
    for (const auto& argument : application.arguments) {
      arguments.push_back(ResolveType(argument, true));
    }
    if (!CheckTypeArguments({
            .owner = method->second.owner + '.' + name,
            .parameters = method->second.declaration->type_parameters,
            .arguments = std::move(arguments),
            .location = {},
        })) {
      continue;
    }
    const auto materialized = MaterializeMethod(method->second, application.arguments, {});
    if (!materialized) {
      AddDiagnostic(materialized.error().location, materialized.error().message);
    }
  }
}

}  // namespace vela::frontend
