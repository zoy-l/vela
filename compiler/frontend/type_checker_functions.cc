#include "compiler/frontend/type_checker.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "compiler/frontend/ast.h"

namespace vela::frontend {

auto TypeChecker::FunctionType(const FunctionInfo& signature) -> Type {
  std::string name = "(";
  FunctionInfo positional;
  positional.return_type = signature.return_type;
  for (std::size_t index = 0; index < signature.parameters.size(); ++index) {
    const auto& parameter = signature.parameters.at(index).type;
    name += (index == 0 ? "" : ", ") + parameter.name;
    const auto key = std::to_string(index);
    positional.parameter_indices.emplace(key, static_cast<std::uint32_t>(index));
    positional.parameters.push_back({.name = key, .type = parameter});
  }
  name += ") => " + signature.return_type.name;
  Type result{.kind = Type::Kind::Function, .name = name};
  function_types_.try_emplace(name, std::move(positional));
  types_.try_emplace(name, result);
  return result;
}

void TypeChecker::CaptureLocal(const Expression& expression, const LocalBinding& binding) {
  bindings_.insert_or_assign(&expression, binding.id);
  for (const auto& context : lambda_scopes_) {
    if (binding.scope >= context.scope) {
      continue;
    }
    auto& captures = lambdas_.at(context.index).captures;
    if (std::ranges::none_of(captures, [&](const Capture& capture) -> bool {
          return capture.binding == binding.id;
        })) {
      captures.push_back({
          .name = expression.text,
          .type = binding.type,
          .binding = binding.id,
          .kind = binding.kind,
      });
    }
  }
}

auto TypeChecker::CheckLambda(const Expression& expression) -> Type {
  if (const auto found = lambda_indices_.find(&expression); found != lambda_indices_.end()) {
    return FunctionType(lambdas_.at(found->second).signature);
  }
  const auto& function = *expression.function;
  const auto signature = ResolveSignature(function);
  const auto result = FunctionType(signature);
  const auto index = lambdas_.size();
  lambda_indices_.emplace(&expression, index);
  lambdas_.push_back({.expression = &expression, .signature = signature, .captures = {}});
  lambda_scopes_.push_back({.index = index, .scope = scopes_.size()});
  auto previous_facts = std::move(refinements_);
  refinements_.clear();
  const auto previous_loop = std::exchange(loop_depth_, 0);
  const auto previous_switch = std::exchange(switch_depth_, 0);
  BeginScope();
  for (const auto& parameter : function.parameters) {
    if (DeclareLocal(parameter.name,
                     signature.parameters.at(signature.parameter_indices.at(parameter.name)).type,
                     parameter.location, BindingKind::Mutable, &parameter)) {
      bindings_.emplace(&parameter, LookupLocal(parameter.name)->id);
    }
  }
  const auto flow = CheckSequence(*function.body, signature.return_type);
  control_flow_.insert_or_assign(function.body.get(), flow);
  if (signature.return_type.kind != Type::Kind::Void && flow.can_fall_through) {
    AddDiagnostic(function.location, "non-void lambda may fall through without returning");
  }
  EndScope();
  loop_depth_ = previous_loop;
  switch_depth_ = previous_switch;
  refinements_ = std::move(previous_facts);
  lambda_scopes_.pop_back();
  return result;
}

}  // namespace vela::frontend
