#include "base/builtin.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/type_checker.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/builtins.h"

namespace vela::frontend {
namespace {

std::unique_ptr<Expression> Name(std::string text, SourceLocation location) {
  auto expression = std::make_unique<Expression>();
  expression->kind = Expression::Kind::Name;
  expression->text = std::move(text);
  expression->location = location;
  return expression;
}

TypeName NamedType(const TypeDescriptor& type, SourceLocation location) {
  return {.name = type.name, .location = location, .arguments = {}};
}

}  // namespace

std::optional<TypeChecker::FunctionInfo> TypeChecker::InferBuiltinMethod(const Type& receiver,
                                                                         std::string_view name) {
  if (const auto builtin = FindBuiltin(receiver.kind, name)) {
    return BuiltinSignature(*builtin);
  }
  if (receiver.kind == Type::Kind::Array) {
    if (name == "join") {
      return BuiltinSignature(Builtin::ArrayJoin);
    }
    const auto methods = ArrayMethods();
    const auto found =
        std::ranges::find_if(methods, [name](const auto& method) { return method.first == name; });
    if (found != methods.end()) {
      return ArrayMethodSignature(found->second, array_elements_.at(receiver.name));
    }
  }
  if (receiver.kind == Type::Kind::Set || receiver.kind == Type::Kind::Map) {
    const auto method = ResolveCollectionMethod(receiver, name);
    return method ? std::optional(method->signature) : std::nullopt;
  }
  return InferAsyncMethod(receiver, name);
}

std::optional<TypeChecker::FunctionInfo> TypeChecker::InferAsyncMethod(const Type& receiver,
                                                                       std::string_view name) {
  FunctionInfo signature;
  signature.return_type = {.kind = Type::Kind::Void, .name = "void"};
  if ((receiver.kind == Type::Kind::Timer || receiver.kind == Type::Kind::CancellationToken) &&
      name == "cancel") {
    return signature;
  }
  if (receiver.kind == Type::Kind::Completer && (name == "complete" || name == "completeError")) {
    const auto element =
        name == "completeError" ? types_.at("Error") : generic_arguments_.at(receiver.name).front();
    if (element.kind != Type::Kind::Void) {
      signature.parameters.push_back(
          {.name = name == "complete" ? "value" : "error", .type = element});
      signature.parameter_indices.emplace(signature.parameters.front().name, 0);
    }
    return signature;
  }
  return std::nullopt;
}

TypeChecker::Type TypeChecker::CheckBuiltinMethodValue(const Expression& expression,
                                                       const Type& receiver,
                                                       const Type* expected) {
  std::optional<FunctionInfo> signature;
  if (expected != nullptr && expected->kind == Type::Kind::Function) {
    signature = function_types_.at(NonNullable(*expected).name);
  } else {
    signature = InferBuiltinMethod(receiver, expression.text);
    const bool rest =
        signature && std::ranges::any_of(signature->parameters,
                                         [](const auto& parameter) { return parameter.rest; });
    const bool algorithm =
        receiver.kind == Type::Kind::Array && FindArrayAlgorithm(expression.text).has_value();
    if (rest || algorithm) {
      AddDiagnostic(expression.location,
                    "this builtin method value requires a function type context");
      return InvalidType();
    }
  }
  if (!signature) {
    AddDiagnostic(expression.location, "unknown builtin method '" + expression.text + "'");
    return InvalidType();
  }
  // 函数类型已有固定的位置参数契约；可选/可变实参在取值时特化为具体调用形状。
  signature->optional_parameters.clear();
  return GenerateBuiltinMethod(expression, receiver, *signature);
}

TypeChecker::Type TypeChecker::GenerateBuiltinMethod(const Expression& expression,
                                                     const Type& receiver,
                                                     const FunctionInfo& signature) {
  auto type = FunctionType(signature);
  const auto key = "$builtin." + receiver.name + "." + expression.text + "." + type.name;
  if (const auto found = builtin_function_ids_.find(key); found != builtin_function_ids_.end()) {
    builtin_method_values_.insert_or_assign(&expression, found->second);
    const auto& generated = generated_functions_.at(found->second);
    const auto* returned = generated.declaration->body->statements.front()->expression.get();
    DiagnoseDynamicConversion(expression, signature.return_type, expression_types_.at(returned));
    for (const auto& argument : returned->arguments) {
      const auto conversion = checked_conversions_.find(argument.value.get());
      if (conversion != checked_conversions_.end()) {
        DiagnoseDynamicConversion(expression, conversion->second,
                                  expression_types_.at(argument.value.get()));
      }
    }
    return type;
  }

  auto function = std::make_unique<Function>();
  function->name = key;
  function->location = expression.location;
  function->return_type = NamedType(signature.return_type, expression.location);
  function->parameters.push_back({
      .type = NamedType(receiver, expression.location),
      .name = "$receiver",
      .location = expression.location,
  });
  FunctionInfo implementation;
  implementation.return_type = signature.return_type;
  implementation.parameters.push_back({.name = "$receiver", .type = receiver});
  implementation.parameter_indices.emplace("$receiver", 0);

  auto call = std::make_unique<Expression>();
  call->kind = Expression::Kind::Call;
  call->location = expression.location;
  call->callee = std::make_unique<Expression>();
  call->callee->kind = Expression::Kind::Member;
  call->callee->location = expression.location;
  call->callee->text = expression.text;
  call->callee->object = Name("$receiver", expression.location);
  for (std::size_t index = 0; index < signature.parameters.size(); ++index) {
    const auto name = "$argument" + std::to_string(index);
    const auto& parameter = signature.parameters.at(index).type;
    function->parameters.push_back({
        .type = NamedType(parameter, expression.location),
        .name = name,
        .location = expression.location,
    });
    implementation.parameter_indices.emplace(name, static_cast<std::uint32_t>(index + 1));
    implementation.parameters.push_back({.name = name, .type = parameter});
    call->arguments.push_back(
        {.name = {}, .location = expression.location, .value = Name(name, expression.location)});
  }
  const auto* invocation = call.get();
  function->body = std::make_unique<Statement>();
  function->body->kind = Statement::Kind::Block;
  function->body->location = expression.location;
  auto returned = std::make_unique<Statement>();
  returned->location = expression.location;
  returned->kind = signature.return_type.kind == Type::Kind::Void ? Statement::Kind::Expression
                                                                  : Statement::Kind::Return;
  returned->expression = std::move(call);
  function->body->statements.push_back(std::move(returned));
  if (signature.return_type.kind == Type::Kind::Void) {
    auto end = std::make_unique<Statement>();
    end->kind = Statement::Kind::Return;
    end->location = expression.location;
    function->body->statements.push_back(std::move(end));
  }

  const auto index = generated_functions_.size();
  builtin_function_ids_.emplace(key, index);
  builtin_method_values_.insert_or_assign(&expression, index);
  generated_functions_.push_back(
      {.declaration = std::move(function), .signature = std::move(implementation)});
  const auto diagnostics = diagnostics_.size();
  CheckGeneratedFunction(index);
  const auto actual = expression_types_.find(invocation);
  if (signature.return_type.kind == Type::Kind::Void && actual != expression_types_.end() &&
      IsValid(actual->second) && actual->second.kind != Type::Kind::Void) {
    AddDiagnostic(expression.location, "builtin method result does not match the function type");
  }
  return HasErrors(std::span(diagnostics_).subspan(diagnostics)) ? InvalidType() : type;
}

}  // namespace vela::frontend
