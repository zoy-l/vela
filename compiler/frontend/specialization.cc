#include "compiler/frontend/specialization.h"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <memory>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/builtins.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/standard_environment.h"
#include "compiler/frontend/type_limits.h"

namespace vela::frontend {
namespace {

// 特化数量有界，既防止递增递归泛型无限展开，也对应当前字节码的索引容量。

bool IsBuiltinGenericExpression(const Expression& expression) {
  if (expression.text == "Set" || expression.text == "Map" || expression.text == "Expected" ||
      expression.text == "Future" || expression.text == "Completer") {
    return true;
  }
  return expression.kind == Expression::Kind::Member && expression.object != nullptr &&
         expression.object->kind == Expression::Kind::Name && expression.object->text == "Future";
}

Function CloneFunction(const Function& source);

std::unique_ptr<Statement> CloneStatement(const Statement* source);

std::unique_ptr<Expression> CloneExpression(const Expression* source) {
  if (source == nullptr) {
    return nullptr;
  }
  auto result = std::make_unique<Expression>();
  result->kind = source->kind;
  result->location = source->location;
  result->text = source->text;
  result->null_safe = source->null_safe;
  result->postfix = source->postfix;
  result->integer_value = source->integer_value;
  result->double_value = source->double_value;
  result->boolean_value = source->boolean_value;
  result->type_arguments = source->type_arguments;
  result->type = source->type;
  for (const auto& arm : source->arms) {
    result->arms.push_back({
        .kind = arm.kind,
        .location = arm.location,
        .binding = arm.binding,
        .pattern = CloneExpression(arm.pattern.get()),
        .value = CloneExpression(arm.value.get()),
    });
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
        .location = argument.location,
        .value = CloneExpression(argument.value.get()),
    });
  }
  if (source->function) {
    result->function = std::make_unique<Function>(CloneFunction(*source->function));
  }
  return result;
}

std::unique_ptr<Statement> CloneStatement(const Statement* source) {
  if (source == nullptr) {
    return nullptr;
  }
  auto result = std::make_unique<Statement>();
  result->kind = source->kind;
  result->location = source->location;
  result->type = source->type;
  result->echo = source->echo;
  result->binding = source->binding;
  result->name = source->name;
  result->expression = CloneExpression(source->expression.get());
  result->condition = CloneExpression(source->condition.get());
  result->increment = CloneExpression(source->increment.get());
  result->then_branch = CloneStatement(source->then_branch.get());
  result->else_branch = CloneStatement(source->else_branch.get());
  result->initializer = CloneStatement(source->initializer.get());
  result->finally_branch = CloneStatement(source->finally_branch.get());
  for (const auto& clause : source->catches) {
    result->catches.push_back({
        .location = clause.location,
        .name = clause.name,
        .type = clause.type,
        .body = CloneStatement(clause.body.get()),
    });
  }
  for (const auto& child : source->statements) {
    result->statements.push_back(CloneStatement(child.get()));
  }
  for (const auto& branch : source->cases) {
    result->cases.push_back({
        .location = branch.location,
        .label = CloneExpression(branch.label.get()),
        .body = CloneStatement(branch.body.get()),
    });
  }
  return result;
}

Function CloneFunction(const Function& source) {
  Function result;
  result.name = source.name;
  result.return_type = source.return_type;
  result.parameters = source.parameters;
  result.body = CloneStatement(source.body.get());
  result.location = source.location;
  result.type_parameters = source.type_parameters;
  result.visibility = source.visibility;
  result.storage = source.storage;
  result.export_kind = source.export_kind;
  result.native_symbol = source.native_symbol;
  result.intrinsic = source.intrinsic;
  result.role = source.role;
  result.is_async = source.is_async;
  return result;
}

Field CloneField(const Field& source) {
  return {
      .type = source.type,
      .name = source.name,
      .location = source.location,
      .visibility = source.visibility,
      .storage = source.storage,
      .role = source.role,
      .initializer = CloneExpression(source.initializer.get()),
  };
}

std::string TypeKey(const TypeName& type);

bool IsTypeWithinLimit(const TypeName& type, std::size_t depth = 1) {
  if (depth > kMaxTypeNesting) {
    return false;
  }
  return std::ranges::all_of(type.arguments, [&](const TypeName& child) -> bool {
    return IsTypeWithinLimit(child, depth + 1);
  });
}

std::string SpecializationName(const std::string& base, const std::vector<TypeName>& arguments) {
  std::string result = base + '<';
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    if (index != 0) {
      result += ", ";
    }
    result += TypeKey(arguments.at(index));
  }
  return result + '>';
}

std::string TypeKey(const TypeName& type) {
  if (type.kind == TypeName::Kind::Generic) {
    return SpecializationName(type.name, type.arguments);
  }
  if (type.kind == TypeName::Kind::Array || type.kind == TypeName::Kind::Nullable) {
    const auto& child = type.arguments.front();
    const auto name = TypeKey(child);
    return (child.kind == TypeName::Kind::Function ? "(" + name + ")" : name) +
           (type.kind == TypeName::Kind::Array ? "[]" : "?");
  }
  if (type.kind == TypeName::Kind::Function) {
    std::string result = "(";
    for (std::size_t index = 0; index + 1 < type.arguments.size(); ++index) {
      result += (index == 0 ? "" : ", ") + TypeKey(type.arguments.at(index));
    }
    return result + ") => " + TypeKey(type.arguments.back());
  }
  return type.name;
}

class Specializer final {
 public:
  explicit Specializer(Program& program) : program_(program) {}
  ~Specializer() = default;
  Specializer(const Specializer&) = delete;
  Specializer& operator=(const Specializer&) = delete;
  Specializer(Specializer&&) = delete;
  Specializer& operator=(Specializer&&) = delete;

  Diagnostics Run();

 private:
  using Substitutions = absl::flat_hash_map<std::string, TypeName>;
  void CollectDeclarations();
  void CollectTemplates();
  void Error(SourceLocation location, std::string message);
  Substitutions Bind(const std::vector<std::string>& parameters,
                     const std::vector<TypeName>& arguments,
                     SourceLocation location);
  void Normalize(TypeName& type, const Substitutions& substitutions);
  void NormalizeType(TypeName& type, const Substitutions& substitutions);
  void Normalize(Expression* expression, const Substitutions& substitutions);
  void Normalize(Statement* statement, const Substitutions& substitutions);
  void Normalize(Function& function, const Substitutions& substitutions);
  void Normalize(Class& owner, const Substitutions& substitutions);
  void NormalizeScoped(Statement* statement, const Substitutions& substitutions);
  [[nodiscard]] bool IsShadowed(const std::string& name) const;
  std::string Instantiate(const std::string& name,
                          const std::vector<TypeName>& arguments,
                          SourceLocation location);

  Program& program_;
  Diagnostics diagnostics_;
  absl::flat_hash_map<std::string, Function> function_templates_;
  absl::flat_hash_map<std::string, Class> class_templates_;
  absl::flat_hash_map<std::string, Program::TypeAlias> aliases_;
  absl::flat_hash_set<std::string> instantiated_;
  absl::flat_hash_set<std::string> active_aliases_;
  std::deque<Function> functions_;
  std::deque<Class> classes_;
  std::deque<Substitutions> function_substitutions_;
  std::deque<Substitutions> class_substitutions_;
  std::vector<absl::flat_hash_set<std::string>> scopes_;
  std::size_t type_depth_ = 0;
};

void Specializer::Error(SourceLocation location, std::string message) {
  diagnostics_.push_back({
      .location = location,
      .message = std::move(message),
      .source_path = location.source_id < program_.sources.size()
                         ? program_.sources.at(location.source_id).path
                         : std::string(),
  });
}

Specializer::Substitutions Specializer::Bind(const std::vector<std::string>& parameters,
                                             const std::vector<TypeName>& arguments,
                                             SourceLocation location) {
  Substitutions result;
  if (parameters.size() != arguments.size()) {
    Error(location, "generic argument count does not match type parameter count");
    return result;
  }
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    if (!result.emplace(parameters.at(index), arguments.at(index)).second) {
      Error(location, "duplicate generic parameter '" + parameters.at(index) + "'");
    }
  }
  return result;
}

std::string Specializer::Instantiate(const std::string& name,
                                     const std::vector<TypeName>& arguments,
                                     SourceLocation location) {
  const auto key = SpecializationName(name, arguments);
  if (instantiated_.contains(key)) {
    return key;
  }
  if (instantiated_.size() >= kMaxGenericSpecializations) {
    Error(location, "generic specialization exceeds the limit of 256; check expanding recursion");
    return key;
  }
  instantiated_.insert(key);
  if (const auto found = function_templates_.find(name); found != function_templates_.end()) {
    const auto substitutions = Bind(found->second.type_parameters, arguments, location);
    auto function = CloneFunction(found->second);
    function.name = key;
    function.type_parameters.clear();
    // 特化体加入工作队列，不沿泛型调用链递归展开 C++ 栈。
    functions_.push_back(std::move(function));
    function_substitutions_.push_back(substitutions);
  } else if (const auto owner = class_templates_.find(name); owner != class_templates_.end()) {
    const auto substitutions = Bind(owner->second.type_parameters, arguments, location);
    Class concrete;
    concrete.name = key;
    for (const auto& field : owner->second.fields) {
      concrete.fields.push_back(CloneField(field));
    }
    concrete.location = owner->second.location;
    concrete.constructor_kind = owner->second.constructor_kind;
    concrete.export_kind = owner->second.export_kind;
    concrete.interfaces = owner->second.interfaces;
    if (owner->second.constructor) {
      concrete.constructor = std::make_unique<Function>(CloneFunction(*owner->second.constructor));
    }
    for (const auto& method : owner->second.methods) {
      concrete.methods.push_back(CloneFunction(method));
    }
    classes_.push_back(std::move(concrete));
    class_substitutions_.push_back(substitutions);
  } else {
    Error(location, "unknown generic declaration '" + name + "'");
  }
  return key;
}

bool Specializer::IsShadowed(const std::string& name) const {
  return std::ranges::any_of(scopes_ | std::views::reverse,
                             [&](const auto& scope) -> bool { return scope.contains(name); });
}

void Specializer::Normalize(TypeName& type, const Substitutions& substitutions) {
  if (type_depth_ >= kMaxTypeNesting) {
    Error(type.location,
          "type nesting exceeds the limit of 128; check expanding generic recursion");
    return;
  }
  ++type_depth_;
  NormalizeType(type, substitutions);
  if (type_depth_ == 1 && diagnostics_.empty() && !IsTypeWithinLimit(type)) {
    Error(type.location,
          "type nesting exceeds the limit of 128; check expanding generic recursion");
  }
  --type_depth_;
}

void Specializer::NormalizeType(TypeName& type, const Substitutions& substitutions) {
  if (type.kind == TypeName::Kind::Named) {
    if (const auto parameter = substitutions.find(type.name); parameter != substitutions.end()) {
      type = parameter->second;
      return;
    }
  }
  for (auto& argument : type.arguments) {
    Normalize(argument, substitutions);
  }
  const auto alias = aliases_.find(type.name);
  if ((type.kind == TypeName::Kind::Named || type.kind == TypeName::Kind::Generic) &&
      alias != aliases_.end()) {
    const auto key = SpecializationName(type.name, type.arguments);
    if (!active_aliases_.insert(key).second) {
      Error(type.location, "cyclic or expanding type alias '" + type.name + "'");
      return;
    }
    const auto bound = Bind(alias->second.type_parameters, type.arguments, type.location);
    type = alias->second.target;
    Normalize(type, bound);
    active_aliases_.erase(key);
    return;
  }
  if (type.kind == TypeName::Kind::Generic) {
    if (type.name == "Set" || type.name == "Map" || type.name == "Expected" ||
        type.name == "Future" || type.name == "Completer") {
      return;
    }
    type.name = Instantiate(type.name, type.arguments, type.location);
    type.kind = TypeName::Kind::Named;
    type.arguments.clear();
  } else if (type.kind == TypeName::Kind::Array || type.kind == TypeName::Kind::Nullable ||
             type.kind == TypeName::Kind::Function) {
    type.name = TypeKey(type);
  }
}

void Specializer::Normalize(Expression* expression, const Substitutions& substitutions) {
  if (expression == nullptr) {
    return;
  }
  if (!expression->type.name.empty()) {
    Normalize(expression->type, substitutions);
  }
  for (auto& argument : expression->type_arguments) {
    Normalize(argument, substitutions);
  }
  if (!expression->type_arguments.empty() && IsShadowed(expression->text)) {
    Error(expression->location, "type arguments cannot be applied to a local binding");
  } else if (!expression->type_arguments.empty() && aliases_.contains(expression->text)) {
    TypeName type{
        .name = expression->text,
        .location = expression->location,
        .kind = TypeName::Kind::Generic,
        .arguments = expression->type_arguments,
    };
    Normalize(type, substitutions);
    if (type.kind == TypeName::Kind::Named ||
        (type.kind == TypeName::Kind::Generic && (type.name == "Set" || type.name == "Map"))) {
      expression->text = type.name;
      expression->type_arguments = type.arguments;
    } else {
      Error(expression->location, "type alias does not name a constructible type");
    }
  } else if (!expression->type_arguments.empty() &&
             (function_templates_.contains(expression->text) ||
              class_templates_.contains(expression->text))) {
    expression->text =
        Instantiate(expression->text, expression->type_arguments, expression->location);
    expression->type_arguments.clear();
  } else if (!expression->type_arguments.empty() && !IsBuiltinGenericExpression(*expression)) {
    Error(expression->location, "type arguments require a generic declaration");
  }
  for (auto* child : {
           expression->left.get(),
           expression->right.get(),
           expression->operand.get(),
           expression->callee.get(),
           expression->object.get(),
           expression->index.get(),
       }) {
    Normalize(child, substitutions);
  }
  for (auto& element : expression->elements) {
    Normalize(element.get(), substitutions);
  }
  for (auto& argument : expression->arguments) {
    Normalize(argument.value.get(), substitutions);
  }
  for (auto& arm : expression->arms) {
    scopes_.emplace_back();
    if (arm.kind == MatchArm::Kind::Binding) {
      scopes_.back().insert(arm.binding);
    }
    Normalize(arm.pattern.get(), substitutions);
    Normalize(arm.value.get(), substitutions);
    scopes_.pop_back();
  }
  if (expression->function) {
    Normalize(*expression->function, substitutions);
  }
}

void Specializer::Normalize(Statement* statement, const Substitutions& substitutions) {
  if (statement == nullptr) {
    return;
  }
  const bool has_scope =
      statement->kind == Statement::Kind::Block || statement->kind == Statement::Kind::For;
  if (has_scope) {
    scopes_.emplace_back();
  }
  if (!statement->type.name.empty()) {
    Normalize(statement->type, substitutions);
  }
  Normalize(statement->initializer.get(), substitutions);
  Normalize(statement->expression.get(), substitutions);
  Normalize(statement->condition.get(), substitutions);
  NormalizeScoped(statement->then_branch.get(), substitutions);
  NormalizeScoped(statement->else_branch.get(), substitutions);
  NormalizeScoped(statement->finally_branch.get(), substitutions);
  for (auto& clause : statement->catches) {
    scopes_.emplace_back();
    if (!clause.name.empty()) {
      Normalize(clause.type, substitutions);
      scopes_.back().insert(clause.name);
    }
    Normalize(clause.body.get(), substitutions);
    scopes_.pop_back();
  }
  Normalize(statement->increment.get(), substitutions);
  for (auto& child : statement->statements) {
    Normalize(child.get(), substitutions);
  }
  for (auto& branch : statement->cases) {
    Normalize(branch.label.get(), substitutions);
    Normalize(branch.body.get(), substitutions);
  }
  if (statement->kind == Statement::Kind::VariableDeclaration && !scopes_.empty()) {
    scopes_.back().insert(statement->name);
  }
  if (has_scope) {
    scopes_.pop_back();
  }
}

void Specializer::NormalizeScoped(Statement* statement, const Substitutions& substitutions) {
  if (statement != nullptr) {
    scopes_.emplace_back();
    Normalize(statement, substitutions);
    scopes_.pop_back();
  }
}

void Specializer::Normalize(Function& function, const Substitutions& substitutions) {
  scopes_.emplace_back();
  Normalize(function.return_type, substitutions);
  for (auto& parameter : function.parameters) {
    Normalize(parameter.type, substitutions);
    scopes_.back().insert(parameter.name);
  }
  Normalize(function.body.get(), substitutions);
  scopes_.pop_back();
}

void Specializer::Normalize(Class& owner, const Substitutions& substitutions) {
  for (auto& interface : owner.interfaces) {
    Normalize(interface, substitutions);
  }
  for (auto& field : owner.fields) {
    Normalize(field.type, substitutions);
    Normalize(field.initializer.get(), substitutions);
  }
  for (auto& method : owner.methods) {
    Normalize(method, substitutions);
  }
  if (owner.constructor) {
    Normalize(*owner.constructor, substitutions);
  }
}

void Specializer::CollectDeclarations() {
  absl::flat_hash_set<std::string> names;
  const auto declare = [&](const auto& declaration, const std::string& kind) -> void {
    if (!names.insert(declaration.name).second) {
      Error(declaration.location, "duplicate " + kind + " '" + declaration.name + "'");
    }
  };
  const auto parameters = [&](const auto& declaration) -> void {
    absl::flat_hash_set<std::string> unique;
    for (const auto& parameter : declaration.type_parameters) {
      if (!unique.insert(parameter).second) {
        Error(declaration.location, "duplicate generic parameter '" + parameter + "'");
      }
    }
  };
  for (const auto& declaration : program_.functions) {
    declare(declaration, "function");
    parameters(declaration);
  }
  for (const auto& declaration : program_.classes) {
    declare(declaration, "type");
    parameters(declaration);
  }
  for (const auto& declaration : program_.enums) {
    declare(declaration, "type");
  }
  for (const auto& declaration : program_.interfaces) {
    declare(declaration, "interface");
  }
  for (const auto& declaration : program_.aliases) {
    declare(declaration, "type alias");
    parameters(declaration);
  }
  for (const auto& declaration : program_.globals) {
    declare(declaration, "global");
  }
}

void Specializer::CollectTemplates() {
  for (auto& alias : program_.aliases) {
    if (IsBuiltinName(alias.name)) {
      Error(alias.location, "type alias name is reserved for a builtin");
    }
    if (!aliases_.emplace(alias.name, alias).second) {
      Error(alias.location, "duplicate type alias '" + alias.name + "'");
    }
  }
  for (auto& function : program_.functions) {
    if (IsBuiltinName(function.name)) {
      Error(function.location, "function name is reserved for a builtin");
    }
    if (function.type_parameters.empty()) {
      functions_.push_back(std::move(function));
      function_substitutions_.emplace_back();
    } else {
      const auto name = function.name;
      function_templates_.emplace(name, std::move(function));
    }
  }
  for (auto& owner : program_.classes) {
    if (IsBuiltinName(owner.name) && !IsStandardTypeNamespace(program_, owner)) {
      Error(owner.location, "type name is reserved for a builtin");
    }
    if (owner.type_parameters.empty()) {
      classes_.push_back(std::move(owner));
      class_substitutions_.emplace_back();
    } else {
      const auto name = owner.name;
      class_templates_.emplace(name, std::move(owner));
    }
  }
  program_.functions.clear();
  program_.classes.clear();
}

Diagnostics Specializer::Run() {
  CollectDeclarations();
  CollectTemplates();
  std::erase_if(program_.aliases,
                [](const auto& alias) -> bool { return !alias.type_parameters.empty(); });
  for (auto& alias : program_.aliases) {
    Normalize(alias.target, {});
  }
  for (auto& interface : program_.interfaces) {
    for (auto& method : interface.methods) {
      Normalize(method, {});
    }
  }
  for (auto& global : program_.globals) {
    if (!global.type.name.empty()) {
      Normalize(global.type, {});
    }
    Normalize(global.initializer.get(), {});
  }
  std::size_t function_index = 0;
  std::size_t class_index = 0;
  while (diagnostics_.empty() &&
         (function_index < functions_.size() || class_index < classes_.size())) {
    if (function_index < functions_.size()) {
      Normalize(functions_.at(function_index), function_substitutions_.at(function_index));
      ++function_index;
    } else {
      Normalize(classes_.at(class_index), class_substitutions_.at(class_index));
      ++class_index;
    }
  }
  for (auto& function : functions_) {
    program_.functions.push_back(std::move(function));
  }
  for (auto& owner : classes_) {
    program_.classes.push_back(std::move(owner));
  }
  return std::move(diagnostics_);
}

}  // namespace

Diagnostics SpecializeGenerics(Program& program) {
  return Specializer(program).Run();
}

}  // namespace vela::frontend
