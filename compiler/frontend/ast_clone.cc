#include "compiler/frontend/ast_clone.h"

#include <memory>
#include <optional>
#include <utility>

namespace vela::frontend {
namespace {

class AstCloner final {
 public:
  explicit AstCloner(std::optional<SourceLocation> origin = std::nullopt,
                     AstCloneTransform* transform = nullptr)
      : origin_(origin), transform_(transform) {}
  Function CloneFunction(const Function& source);
  Class CloneClass(const Class& source);
  Program CloneProgram(const Program& source);

 private:
  SourceLocation Relocate(SourceLocation location) const {
    if (origin_) {
      location.source_id = origin_->source_id;
      location.line += origin_->line - 1;
    }
    return location;
  }
  TypeName CloneType(const TypeName& source) const {
    if (transform_ != nullptr && !source.name.empty()) {
      return transform_->TransformType(source);
    }
    TypeName result{.name = source.name,
                    .location = Relocate(source.location),
                    .kind = source.kind,
                    .arguments = {}};
    result.arguments.reserve(source.arguments.size());
    for (const auto& argument : source.arguments) {
      result.arguments.push_back(CloneType(argument));
    }
    return result;
  }
  std::unique_ptr<Expression> CloneExpression(const Expression* source);
  std::unique_ptr<Statement> CloneStatement(const Statement* source);
  std::unique_ptr<Statement> CloneScopedStatement(const Statement* source);
  Field CloneField(const Field& source);
  std::optional<SourceLocation> origin_;
  AstCloneTransform* transform_;
};

std::unique_ptr<Expression> AstCloner::CloneExpression(const Expression* source) {
  if (source == nullptr) {
    return nullptr;
  }
  auto result = std::make_unique<Expression>();
  result->kind = source->kind;
  result->location = Relocate(source->location);
  result->text = source->text;
  result->null_safe = source->null_safe;
  result->postfix = source->postfix;
  result->integer_value = source->integer_value;
  result->double_value = source->double_value;
  result->boolean_value = source->boolean_value;
  result->type_arguments.reserve(source->type_arguments.size());
  result->arms.reserve(source->arms.size());
  result->elements.reserve(source->elements.size());
  result->arguments.reserve(source->arguments.size());
  result->type = CloneType(source->type);
  for (const auto& type : source->type_arguments) {
    result->type_arguments.push_back(CloneType(type));
  }
  if (transform_ != nullptr && !result->type_arguments.empty()) {
    transform_->TransformApplication(*result, *source);
  }
  result->left = CloneExpression(source->left.get());
  result->right = CloneExpression(source->right.get());
  result->operand = CloneExpression(source->operand.get());
  result->callee = CloneExpression(source->callee.get());
  result->object = CloneExpression(source->object.get());
  result->index = CloneExpression(source->index.get());
  for (const auto& element : source->elements) {
    result->elements.push_back(CloneExpression(element.get()));
  }
  for (const auto& argument : source->arguments) {
    result->arguments.push_back({
        .name = argument.name,
        .location = Relocate(argument.location),
        .value = CloneExpression(argument.value.get()),
    });
  }
  for (const auto& arm : source->arms) {
    if (transform_ != nullptr) {
      transform_->BeginScope();
      if (arm.kind == MatchArm::Kind::Binding) {
        transform_->Declare(arm.binding);
      }
    }
    result->arms.push_back({
        .kind = arm.kind,
        .location = Relocate(arm.location),
        .binding = arm.binding,
        .pattern = CloneExpression(arm.pattern.get()),
        .value = CloneExpression(arm.value.get()),
    });
    if (transform_ != nullptr) {
      transform_->EndScope();
    }
  }
  if (source->function) {
    result->function = std::make_unique<Function>(CloneFunction(*source->function));
  }
  return result;
}

std::unique_ptr<Statement> AstCloner::CloneScopedStatement(const Statement* source) {
  if (source == nullptr) {
    return nullptr;
  }
  if (transform_ != nullptr) {
    transform_->BeginScope();
  }
  auto result = CloneStatement(source);
  if (transform_ != nullptr) {
    transform_->EndScope();
  }
  return result;
}

std::unique_ptr<Statement> AstCloner::CloneStatement(const Statement* source) {
  if (source == nullptr) {
    return nullptr;
  }
  const bool has_scope = source->kind == Statement::Kind::Block ||
                         source->kind == Statement::Kind::For ||
                         source->kind == Statement::Kind::ForEach;
  if (transform_ != nullptr && has_scope) {
    transform_->BeginScope();
  }
  auto result = std::make_unique<Statement>();
  result->kind = source->kind;
  result->location = Relocate(source->location);
  result->type = CloneType(source->type);
  result->echo = source->echo;
  result->binding = source->binding;
  result->name = source->name;
  result->second_name = source->second_name;
  result->destructuring = source->destructuring;
  result->initializer = CloneStatement(source->initializer.get());
  result->expression = CloneExpression(source->expression.get());
  result->condition = CloneExpression(source->condition.get());
  result->iterable = CloneExpression(source->iterable.get());
  if (transform_ != nullptr && source->kind == Statement::Kind::ForEach) {
    transform_->Declare(source->name);
    if (source->destructuring) {
      transform_->Declare(source->second_name);
    }
  }
  result->then_branch = CloneScopedStatement(source->then_branch.get());
  result->else_branch = CloneScopedStatement(source->else_branch.get());
  result->finally_branch = CloneScopedStatement(source->finally_branch.get());
  result->catches.reserve(source->catches.size());
  result->statements.reserve(source->statements.size());
  result->cases.reserve(source->cases.size());
  for (const auto& clause : source->catches) {
    if (transform_ != nullptr) {
      transform_->BeginScope();
    }
    auto type = CloneType(clause.type);
    if (transform_ != nullptr && !clause.name.empty()) {
      transform_->Declare(clause.name);
    }
    result->catches.push_back({
        .location = Relocate(clause.location),
        .name = clause.name,
        .type = std::move(type),
        .body = CloneStatement(clause.body.get()),
    });
    if (transform_ != nullptr) {
      transform_->EndScope();
    }
  }
  result->increment = CloneExpression(source->increment.get());
  for (const auto& child : source->statements) {
    result->statements.push_back(CloneStatement(child.get()));
  }
  for (const auto& branch : source->cases) {
    result->cases.push_back({
        .location = Relocate(branch.location),
        .label = CloneExpression(branch.label.get()),
        .body = CloneStatement(branch.body.get()),
    });
  }
  if (transform_ != nullptr) {
    if (source->kind == Statement::Kind::VariableDeclaration) {
      transform_->Declare(source->name);
    }
    if (has_scope) {
      transform_->EndScope();
    }
  }
  return result;
}

Function AstCloner::CloneFunction(const Function& source) {
  if (transform_ != nullptr) {
    transform_->BeginScope();
  }
  Function result;
  result.name = source.name;
  result.return_type = CloneType(source.return_type);
  result.parameters.reserve(source.parameters.size());
  for (const auto& parameter : source.parameters) {
    result.parameters.push_back({.type = CloneType(parameter.type),
                                 .name = parameter.name,
                                 .location = Relocate(parameter.location)});
    if (transform_ != nullptr) {
      transform_->Declare(parameter.name);
    }
  }
  result.body = CloneStatement(source.body.get());
  result.location = Relocate(source.location);
  result.type_parameters = source.type_parameters;
  result.visibility = source.visibility;
  result.storage = source.storage;
  result.export_kind = source.export_kind;
  result.native_symbol = source.native_symbol;
  result.intrinsic = source.intrinsic;
  result.role = source.role;
  result.is_async = source.is_async;
  result.is_override = source.is_override;
  result.is_getter = source.is_getter;
  result.is_setter = source.is_setter;
  if (transform_ != nullptr) {
    transform_->EndScope();
  }
  return result;
}

Field AstCloner::CloneField(const Field& source) {
  return {
      .type = CloneType(source.type),
      .name = source.name,
      .location = Relocate(source.location),
      .visibility = source.visibility,
      .storage = source.storage,
      .initializer = CloneExpression(source.initializer.get()),
  };
}

Program AstCloner::CloneProgram(const Program& source) {
  Program result;
  result.functions.reserve(source.functions.size());
  result.classes.reserve(source.classes.size());
  result.globals.reserve(source.globals.size());
  result.imports = source.imports;
  for (auto& imported : result.imports) {
    imported.location = Relocate(imported.location);
  }
  result.sources = source.sources;
  result.file_resolutions = source.file_resolutions;
  result.standard_names = source.standard_names;
  result.enums = source.enums;
  for (auto& declaration : result.enums) {
    declaration.location = Relocate(declaration.location);
    for (auto& member : declaration.members) {
      member.location = Relocate(member.location);
    }
  }
  result.aliases = source.aliases;
  for (auto& alias : result.aliases) {
    alias.location = Relocate(alias.location);
    alias.target = CloneType(alias.target);
  }
  for (const auto& function : source.functions) {
    result.functions.push_back(CloneFunction(function));
  }
  result.interfaces.reserve(source.interfaces.size());
  for (const auto& declaration : source.interfaces) {
    Interface copy;
    copy.name = declaration.name;
    copy.location = Relocate(declaration.location);
    copy.export_kind = declaration.export_kind;
    copy.methods.reserve(declaration.methods.size());
    for (const auto& method : declaration.methods) {
      copy.methods.push_back(CloneFunction(method));
    }
    result.interfaces.push_back(std::move(copy));
  }
  for (const auto& owner : source.classes) {
    result.classes.push_back(CloneClass(owner));
  }
  for (const auto& global : source.globals) {
    result.globals.push_back({.name = global.name,
                              .type = CloneType(global.type),
                              .initializer = CloneExpression(global.initializer.get()),
                              .location = Relocate(global.location),
                              .binding = global.binding,
                              .export_kind = global.export_kind});
  }
  return result;
}

Class AstCloner::CloneClass(const Class& source) {
  Class result;
  result.name = source.name;
  result.location = Relocate(source.location);
  result.type_parameters = source.type_parameters;
  result.constructor_kind = source.constructor_kind;
  result.export_kind = source.export_kind;
  result.interfaces.reserve(source.interfaces.size());
  result.fields.reserve(source.fields.size());
  result.methods.reserve(source.methods.size());
  if (source.base_class) {
    result.base_class = CloneType(*source.base_class);
  }
  for (const auto& type : source.interfaces) {
    result.interfaces.push_back(CloneType(type));
  }
  for (const auto& field : source.fields) {
    result.fields.push_back(CloneField(field));
  }
  for (const auto& method : source.methods) {
    result.methods.push_back(CloneFunction(method));
  }
  if (source.constructor) {
    result.constructor = std::make_unique<Function>(CloneFunction(*source.constructor));
  }
  return result;
}

}  // namespace

Function CloneFunction(const Function& source, AstCloneTransform& transform) {
  return AstCloner(std::nullopt, &transform).CloneFunction(source);
}
Class CloneClass(const Class& source, AstCloneTransform& transform) {
  return AstCloner(std::nullopt, &transform).CloneClass(source);
}
Program CloneProgram(const Program& source, SourceLocation origin) {
  return AstCloner(origin).CloneProgram(source);
}

}  // namespace vela::frontend
