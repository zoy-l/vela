#include "compiler/frontend/specialization.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include "base/declaration.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/ast_clone.h"
#include "compiler/frontend/builtins.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/standard_environment.h"
#include "compiler/frontend/type_limits.h"

namespace vela::frontend {
namespace {

bool IsBuiltinGenericExpression(const Expression& expression, const Expression* object) {
  if (expression.text == "Set" || expression.text == "Map" || expression.text == "Future" ||
      expression.text == "Completer") {
    return true;
  }
  return expression.kind == Expression::Kind::Member && object != nullptr &&
         object->kind == Expression::Kind::Name && object->text == "Future";
}

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
    result += arguments.at(index).ToString();
  }
  return result + '>';
}

class Specializer final : public AstCloneTransform {
 public:
  explicit Specializer(Program& program) : program_(program) {}
  ~Specializer() override = default;
  Specializer(const Specializer&) = delete;
  Specializer& operator=(const Specializer&) = delete;
  Specializer(Specializer&&) = delete;
  Specializer& operator=(Specializer&&) = delete;

  Diagnostics Run(std::span<const GenericSpecialization> retained);

 private:
  using Substitutions = absl::flat_hash_map<std::string, TypeName>;
  template <class T>
  struct Pending final {
    T declaration;
    Substitutions substitutions;
    const T* source = nullptr;
    std::size_t expansion_depth = 0;
  };
  struct Work final {
    enum class Kind : std::uint8_t { Function, Class };
    Kind kind;
    std::size_t index;
  };
  void CollectDeclarations();
  void CollectAliases();
  void CollectFunctionTemplates();
  void CollectClassTemplates();
  void DescribeClassTemplate(const Class& owner);
  void NormalizeAliases();
  void Error(SourceLocation location, std::string message);
  Substitutions Bind(const std::vector<std::string>& parameters,
                     const std::vector<TypeName>& arguments,
                     SourceLocation location);
  void Normalize(TypeName& type, const Substitutions& substitutions);
  void NormalizeType(TypeName& type, const Substitutions& substitutions);
  void NormalizeTypeHead(TypeName& type);
  TypeName CloneNormalizedType(const TypeName& source, const Substitutions& substitutions);
  void NormalizeExpressionHead(Expression& expression,
                               const Substitutions& substitutions,
                               const Expression* object);
  TypeName TransformType(const TypeName& source) override;
  void TransformApplication(Expression& target, const Expression& source) override;
  void BeginScope() override;
  void EndScope() override;
  void Declare(const std::string& name) override;
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
  std::deque<Pending<Function>> functions_;
  std::deque<Pending<Class>> classes_;
  std::vector<Work> work_;
  std::vector<absl::flat_hash_set<std::string>> scopes_;
  std::size_t type_depth_ = 0;
  std::size_t expansion_depth_ = 0;
  const Substitutions* clone_substitutions_ = nullptr;
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
  // 限制单条依赖链的展开深度，不限制项目中彼此独立的具体特化数量。
  if (expansion_depth_ >= kMaxGenericExpansionDepth) {
    Error(location, "generic expansion depth exceeds the limit of 128; check expanding recursion");
    return key;
  }
  instantiated_.insert(key);
  if (const auto origin = program_.origins.find(name); origin != program_.origins.end()) {
    auto concrete_origin = origin->second;
    concrete_origin.arguments = arguments;
    program_.origins.emplace(key, std::move(concrete_origin));
  }
  // 模板表在 CollectTemplates 后不再插入或改写，队列借用的节点地址保持稳定。
  if (const auto found = function_templates_.find(name); found != function_templates_.end()) {
    auto substitutions = Bind(found->second.type_parameters, arguments, location);
    Function function;
    function.name = key;
    // 只排入模板引用；取出队列项时融合克隆与规范化，不沿泛型调用链递归展开 C++ 栈。
    functions_.push_back({
        .declaration = std::move(function),
        .substitutions = std::move(substitutions),
        .source = &found->second,
        .expansion_depth = expansion_depth_ + 1,
    });
    work_.push_back({.kind = Work::Kind::Function, .index = functions_.size() - 1});
  } else if (const auto owner = class_templates_.find(name); owner != class_templates_.end()) {
    auto substitutions = Bind(owner->second.type_parameters, arguments, location);
    Class concrete;
    concrete.name = key;
    classes_.push_back({
        .declaration = std::move(concrete),
        .substitutions = std::move(substitutions),
        .source = &owner->second,
        .expansion_depth = expansion_depth_ + 1,
    });
    work_.push_back({.kind = Work::Kind::Class, .index = classes_.size() - 1});
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
  NormalizeTypeHead(type);
}

void Specializer::NormalizeTypeHead(TypeName& type) {
  const auto alias = aliases_.find(type.name);
  if ((type.kind == TypeName::Kind::Named || type.kind == TypeName::Kind::Generic) &&
      alias != aliases_.end()) {
    const auto key = SpecializationName(type.name, type.arguments);
    if (!active_aliases_.insert(key).second) {
      Error(type.location, "cyclic or expanding type alias '" + type.name + "'");
      return;
    }
    const auto bound = Bind(alias->second.type_parameters, type.arguments, type.location);
    type = CloneNormalizedType(alias->second.target, bound);
    active_aliases_.erase(key);
    return;
  }
  if (type.kind == TypeName::Kind::Generic) {
    if (type.name == "Set" || type.name == "Map" || type.name == "Future" ||
        type.name == "Completer") {
      return;
    }
    type.name = Instantiate(type.name, type.arguments, type.location);
    type.kind = TypeName::Kind::Named;
    type.arguments.clear();
  } else if (type.kind == TypeName::Kind::Array || type.kind == TypeName::Kind::Nullable ||
             type.kind == TypeName::Kind::Function) {
    type.name = type.ToString();
  }
}

TypeName Specializer::CloneNormalizedType(const TypeName& source,
                                          const Substitutions& substitutions) {
  if (type_depth_ >= kMaxTypeNesting) {
    Error(source.location,
          "type nesting exceeds the limit of 128; check expanding generic recursion");
    return source;
  }
  ++type_depth_;
  TypeName result;
  const auto parameter =
      source.kind == TypeName::Kind::Named ? substitutions.find(source.name) : substitutions.end();
  if (parameter != substitutions.end()) {
    result = parameter->second;
  } else {
    result.name = source.name;
    result.location = source.location;
    result.kind = source.kind;
    result.arguments.reserve(source.arguments.size());
    for (const auto& argument : source.arguments) {
      result.arguments.push_back(CloneNormalizedType(argument, substitutions));
    }
    NormalizeTypeHead(result);
  }
  if (type_depth_ == 1 && diagnostics_.empty() && !IsTypeWithinLimit(result)) {
    Error(result.location,
          "type nesting exceeds the limit of 128; check expanding generic recursion");
  }
  --type_depth_;
  return result;
}

TypeName Specializer::TransformType(const TypeName& source) {
  return CloneNormalizedType(source, *clone_substitutions_);
}

void Specializer::TransformApplication(Expression& target, const Expression& source) {
  NormalizeExpressionHead(target, *clone_substitutions_, source.object.get());
}

void Specializer::BeginScope() {
  scopes_.emplace_back();
}

void Specializer::EndScope() {
  scopes_.pop_back();
}

void Specializer::Declare(const std::string& name) {
  if (!scopes_.empty()) {
    scopes_.back().insert(name);
  }
}

void Specializer::NormalizeExpressionHead(Expression& expression,
                                          const Substitutions& substitutions,
                                          const Expression* object) {
  if (!expression.type_arguments.empty() && IsShadowed(expression.text)) {
    Error(expression.location, "type arguments cannot be applied to a local binding");
  } else if (!expression.type_arguments.empty() && aliases_.contains(expression.text)) {
    TypeName type{
        .name = expression.text,
        .location = expression.location,
        .kind = TypeName::Kind::Generic,
        .arguments = expression.type_arguments,
    };
    Normalize(type, substitutions);
    if (type.kind == TypeName::Kind::Named ||
        (type.kind == TypeName::Kind::Generic && (type.name == "Set" || type.name == "Map"))) {
      expression.text = type.name;
      expression.type_arguments = type.arguments;
    } else {
      Error(expression.location, "type alias does not name a constructible type");
    }
  } else if (!expression.type_arguments.empty() && (function_templates_.contains(expression.text) ||
                                                    class_templates_.contains(expression.text))) {
    expression.text = Instantiate(expression.text, expression.type_arguments, expression.location);
    expression.type_arguments.clear();
  } else if (!expression.type_arguments.empty() &&
             !IsBuiltinGenericExpression(expression, object)) {
    Error(expression.location, "type arguments require a generic declaration");
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
  NormalizeExpressionHead(*expression, substitutions, expression->object.get());
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
  const bool has_scope = statement->kind == Statement::Kind::Block ||
                         statement->kind == Statement::Kind::For ||
                         statement->kind == Statement::Kind::ForEach;
  if (has_scope) {
    scopes_.emplace_back();
  }
  if (!statement->type.name.empty()) {
    Normalize(statement->type, substitutions);
  }
  Normalize(statement->initializer.get(), substitutions);
  Normalize(statement->expression.get(), substitutions);
  Normalize(statement->condition.get(), substitutions);
  Normalize(statement->iterable.get(), substitutions);
  if (statement->kind == Statement::Kind::ForEach) {
    scopes_.back().insert(statement->name);
    if (statement->destructuring) {
      scopes_.back().insert(statement->second_name);
    }
  }
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
  if (owner.base_class) {
    Normalize(*owner.base_class, substitutions);
  }
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

void Specializer::CollectAliases() {
  for (auto& alias : program_.aliases) {
    if (IsBuiltinName(alias.name)) {
      Error(alias.location, "type alias name is reserved for a builtin");
    }
    if (!aliases_.emplace(alias.name, alias).second) {
      Error(alias.location, "duplicate type alias '" + alias.name + "'");
    }
  }
}

void Specializer::CollectFunctionTemplates() {
  for (auto& function : program_.functions) {
    if (IsBuiltinName(function.name)) {
      Error(function.location, "function name is reserved for a builtin");
    }
    if (function.type_parameters.empty()) {
      functions_.push_back({.declaration = std::move(function), .substitutions = {}});
      work_.push_back({.kind = Work::Kind::Function, .index = functions_.size() - 1});
    } else {
      const auto name = function.name;
      function_templates_.emplace(name, std::move(function));
    }
  }
  program_.functions.clear();
}

void Specializer::DescribeClassTemplate(const Class& owner) {
  const auto origin = program_.origins.find(owner.name);
  if (origin == program_.origins.end()) {
    return;
  }
  auto& description = origin->second;
  description.members.push_back({.kind = DeclarationKind::Constructor, .name = "$constructor"});
  for (const auto& field : owner.fields) {
    description.members.push_back({
        .kind = field.storage == MemberStorage::Static ? DeclarationKind::StaticField
                                                       : DeclarationKind::Field,
        .name = field.name,
        .has_initializer = field.initializer != nullptr,
    });
  }
  for (const auto& method : owner.methods) {
    auto kind = DeclarationKind::Method;
    if (method.is_getter) {
      kind = DeclarationKind::Getter;
    } else if (method.is_setter) {
      kind = DeclarationKind::Setter;
    } else if (method.storage == MemberStorage::Static) {
      kind = DeclarationKind::StaticMethod;
    }
    description.members.push_back({.kind = kind, .name = method.name});
  }
}

void Specializer::CollectClassTemplates() {
  for (auto& owner : program_.classes) {
    if (IsBuiltinName(owner.name) && !IsStandardTypeNamespace(program_, owner)) {
      Error(owner.location, "type name is reserved for a builtin");
    }
    if (owner.type_parameters.empty()) {
      classes_.push_back({.declaration = std::move(owner), .substitutions = {}});
      work_.push_back({.kind = Work::Kind::Class, .index = classes_.size() - 1});
    } else {
      const auto name = owner.name;
      DescribeClassTemplate(owner);
      class_templates_.emplace(name, std::move(owner));
    }
  }
  program_.classes.clear();
}

void Specializer::NormalizeAliases() {
  std::erase_if(program_.aliases,
                [](const auto& alias) -> bool { return !alias.type_parameters.empty(); });
  for (auto& alias : program_.aliases) {
    Normalize(alias.target, {});
  }
}

Diagnostics Specializer::Run(std::span<const GenericSpecialization> retained) {
  CollectDeclarations();
  CollectAliases();
  CollectFunctionTemplates();
  CollectClassTemplates();
  for (const auto& request : retained) {
    auto arguments = request.arguments;
    for (auto& argument : arguments) {
      Normalize(argument, {});
    }
    (void)Instantiate(request.name, arguments, {});
  }
  NormalizeAliases();
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
  // 深度优先处理依赖，保证指数分叉的展开递归先触达深度边界，而非耗尽整个宽度。
  while (diagnostics_.empty() && !work_.empty()) {
    const auto work = work_.back();
    work_.pop_back();
    if (work.kind == Work::Kind::Function) {
      auto& pending = functions_.at(work.index);
      expansion_depth_ = pending.expansion_depth;
      if (pending.source != nullptr) {
        const auto name = pending.declaration.name;
        clone_substitutions_ = &pending.substitutions;
        pending.declaration = CloneFunction(*pending.source, *this);
        clone_substitutions_ = nullptr;
        pending.declaration.name = name;
        pending.declaration.type_parameters.clear();
      } else {
        Normalize(pending.declaration, pending.substitutions);
      }
    } else {
      auto& pending = classes_.at(work.index);
      expansion_depth_ = pending.expansion_depth;
      if (pending.source != nullptr) {
        const auto name = pending.declaration.name;
        clone_substitutions_ = &pending.substitutions;
        pending.declaration = CloneClass(*pending.source, *this);
        clone_substitutions_ = nullptr;
        pending.declaration.name = name;
        pending.declaration.type_parameters.clear();
      } else {
        Normalize(pending.declaration, pending.substitutions);
      }
    }
  }
  for (auto& function : functions_) {
    program_.functions.push_back(std::move(function.declaration));
  }
  for (auto& owner : classes_) {
    program_.classes.push_back(std::move(owner.declaration));
  }
  return std::move(diagnostics_);
}

}  // namespace

Diagnostics SpecializeGenerics(Program& program, std::span<const GenericSpecialization> retained) {
  return Specializer(program).Run(retained);
}

}  // namespace vela::frontend
