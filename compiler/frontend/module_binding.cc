#include "compiler/frontend/module_binding.h"
#include "base/declaration.h"
#include "compiler/frontend/source_file.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include "compiler/frontend/ast.h"
#include "compiler/frontend/builtins.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/parser.h"
#include "compiler/frontend/standard_environment.h"

namespace vela::frontend {
namespace {

struct Symbol final {
  std::string target;
  ExportKind export_kind = ExportKind::Local;
};

struct ModuleScope final {
  absl::flat_hash_map<std::string, Symbol> declarations;
  absl::flat_hash_map<std::string, std::string> names;
  absl::flat_hash_map<std::string, std::uint32_t> prefixes;
};

class ModuleBinding final {
 public:
  ModuleBinding(std::vector<SourceModule> modules, std::vector<SourceFile> sources)
      : modules_(std::move(modules)), sources_(std::move(sources)), scopes_(modules_.size()) {}

  ParseResult Run();

 private:
  void Error(SourceLocation location, std::string message);
  template <class Declaration>
  void Declare(const Declaration& declaration, DeclarationKind kind, std::size_t index);
  void CollectDeclarations();
  void BindImports();
  void AddImport(const Program::Import& imported, std::uint32_t dependency, ModuleScope& scope);
  void BindProgram(Program& program);
  void BindClass(Class& declaration);
  void BindFunction(Function& function);
  void BindType(TypeName& type);
  void BindExpression(Expression* expression);
  void BindStatement(Statement* statement);
  void BindScoped(Statement* statement);
  [[nodiscard]] bool IsLocal(const std::string& name) const;
  [[nodiscard]] bool IsTypeParameter(const std::string& name) const;
  std::string Resolve(const std::string& name, SourceLocation location);
  std::string ExportedName(std::uint32_t dependency,
                           const std::string& name,
                           SourceLocation location);

  std::vector<SourceModule> modules_;
  std::vector<SourceFile> sources_;
  std::vector<ModuleScope> scopes_;
  Diagnostics diagnostics_;
  std::uint32_t current_ = 0;
  std::vector<absl::flat_hash_set<std::string>> locals_;
  std::vector<absl::flat_hash_set<std::string>> type_parameters_;
};

void ModuleBinding::Error(SourceLocation location, std::string message) {
  diagnostics_.push_back({
      .location = location,
      .message = std::move(message),
      .source_path = sources_.at(location.source_id).path,
  });
}

template <class Declaration>
void ModuleBinding::Declare(const Declaration& declaration,
                            DeclarationKind kind,
                            std::size_t index) {
  auto& scope = scopes_.at(index);
  auto& module = modules_.at(index);
  if (IsBuiltinName(declaration.name) || IsStandardName(declaration.name)) {
    Error(declaration.location, "declaration name is reserved for a builtin");
  }
  const auto binding_id = module.binding_id;
  const auto target = index == 0 ? declaration.name
                                 : "$module" + std::to_string(binding_id) + "." + declaration.name;
  if (!scope.declarations
           .emplace(declaration.name,
                    Symbol{.target = target, .export_kind = declaration.export_kind})
           .second) {
    const auto* category = kind == DeclarationKind::Function ? "function" : "module declaration";
    Error(declaration.location,
          "duplicate " + std::string(category) + " '" + declaration.name + "'");
  }
  scope.names.try_emplace(declaration.name, target);
  std::vector<std::string> parameters;
  if constexpr (requires { declaration.type_parameters; }) {
    parameters = declaration.type_parameters;
  }
  module.program.origins.emplace(target, DeclarationOrigin{
                                             .module_uri = sources_.at(index).path,
                                             .name = declaration.name,
                                             .kind = kind,
                                             .arguments = {},
                                             .type_parameters = std::move(parameters),
                                             .members = {},
                                         });
}

void ModuleBinding::CollectDeclarations() {
  for (std::size_t index = 0; index < modules_.size(); ++index) {
    const auto& program = modules_.at(index).program;
    for (const auto& declaration : program.functions) {
      Declare(declaration, DeclarationKind::Function, index);
    }
    for (const auto& declaration : program.classes) {
      Declare(declaration, DeclarationKind::Class, index);
    }
    for (const auto& declaration : program.enums) {
      Declare(declaration, DeclarationKind::Enum, index);
    }
    for (const auto& declaration : program.interfaces) {
      Declare(declaration, DeclarationKind::Interface, index);
    }
    for (const auto& declaration : program.aliases) {
      Declare(declaration, DeclarationKind::Alias, index);
    }
    for (const auto& declaration : program.globals) {
      Declare(declaration, DeclarationKind::Global, index);
    }
  }
}

void ModuleBinding::AddImport(const Program::Import& imported,
                              std::uint32_t dependency,
                              ModuleScope& scope) {
  if (!imported.alias.empty()) {
    if (IsBuiltinName(imported.alias) || IsStandardName(imported.alias) ||
        scope.names.contains(imported.alias) ||
        !scope.prefixes.emplace(imported.alias, dependency).second) {
      Error(imported.location, "duplicate or reserved import prefix '" + imported.alias + "'");
    }
    return;
  }
  for (const auto& [name, symbol] : scopes_.at(dependency).declarations) {
    if (symbol.export_kind != ExportKind::Exported) {
      continue;
    }
    const auto [found, inserted] = scope.names.try_emplace(name, symbol.target);
    if (scope.prefixes.contains(name) || (!inserted && found->second != symbol.target)) {
      Error(imported.location, "conflicting imported name '" + name + "'; use an import prefix");
    }
  }
}

void ModuleBinding::BindImports() {
  for (std::size_t index = 0; index < modules_.size(); ++index) {
    const auto& module = modules_.at(index);
    auto& scope = scopes_.at(index);
    for (std::size_t imported = 0; imported < module.program.imports.size(); ++imported) {
      AddImport(module.program.imports.at(imported), module.dependencies.at(imported), scope);
    }
  }
}

bool ModuleBinding::IsLocal(const std::string& name) const {
  return std::ranges::any_of(locals_,
                             [&name](const auto& scope) -> bool { return scope.contains(name); });
}

bool ModuleBinding::IsTypeParameter(const std::string& name) const {
  return std::ranges::any_of(type_parameters_,
                             [&name](const auto& scope) -> bool { return scope.contains(name); });
}

std::string ModuleBinding::ExportedName(std::uint32_t dependency,
                                        const std::string& name,
                                        SourceLocation location) {
  const auto& declarations = scopes_.at(dependency).declarations;
  const auto found = declarations.find(name);
  if (found == declarations.end() || found->second.export_kind != ExportKind::Exported) {
    Error(location, "module does not export '" + name + "'");
    return name;
  }
  return found->second.target;
}

std::string ModuleBinding::Resolve(const std::string& name, SourceLocation location) {
  const auto& scope = scopes_.at(current_);
  const auto separator = name.find('.');
  if (separator != std::string::npos) {
    const auto prefix = scope.prefixes.find(name.substr(0, separator));
    if (prefix != scope.prefixes.end()) {
      return ExportedName(prefix->second, name.substr(separator + 1), location);
    }
  }
  const auto found = scope.names.find(name);
  if (found != scope.names.end()) {
    return found->second;
  }
  if (current_ != 0 && scopes_.front().declarations.contains(name)) {
    // 入口声明保留宿主可见名称；合并 AST 不能令其他模块越过各自的 import 作用域。
    Error(location, "name '" + name + "' is not declared or imported by this module");
  }
  // 未绑定的未限定名字交由类型检查器按表达式/类型上下文诊断，避免模块绑定抢占其语义。
  return name;
}

void ModuleBinding::BindType(TypeName& type) {
  if (type.name.empty()) {
    return;
  }
  for (auto& argument : type.arguments) {
    BindType(argument);
  }
  if ((type.kind == TypeName::Kind::Named || type.kind == TypeName::Kind::Generic) &&
      !IsTypeParameter(type.name.substr(0, type.name.find('.')))) {
    type.name = Resolve(type.name, type.location);
  }
}

void ModuleBinding::BindExpression(Expression* expression) {
  if (expression == nullptr) {
    return;
  }
  if (expression->kind == Expression::Kind::Member &&
      expression->object->kind == Expression::Kind::Name && !IsLocal(expression->object->text)) {
    const auto& prefixes = scopes_.at(current_).prefixes;
    const auto prefix = prefixes.find(expression->object->text);
    if (prefix != prefixes.end()) {
      expression->text = ExportedName(prefix->second, expression->text, expression->location);
      expression->kind = Expression::Kind::Name;
      expression->object.reset();
    }
  } else if (expression->kind == Expression::Kind::Name && !IsLocal(expression->text)) {
    if (scopes_.at(current_).prefixes.contains(expression->text)) {
      Error(expression->location, "an import prefix must be followed by an exported member");
    } else {
      expression->text = Resolve(expression->text, expression->location);
    }
  }
  BindType(expression->type);
  for (auto& argument : expression->type_arguments) {
    BindType(argument);
  }
  for (auto* child : {
           expression->left.get(),
           expression->right.get(),
           expression->operand.get(),
           expression->callee.get(),
           expression->object.get(),
           expression->index.get(),
       }) {
    BindExpression(child);
  }
  for (auto& element : expression->elements) {
    BindExpression(element.get());
  }
  for (auto& argument : expression->arguments) {
    BindExpression(argument.value.get());
  }
  for (auto& arm : expression->arms) {
    BindExpression(arm.pattern.get());
    locals_.emplace_back();
    if (arm.kind == MatchArm::Kind::Binding) {
      locals_.back().insert(arm.binding);
    }
    BindExpression(arm.value.get());
    locals_.pop_back();
  }
  if (expression->function) {
    BindFunction(*expression->function);
  }
}

void ModuleBinding::BindScoped(Statement* statement) {
  locals_.emplace_back();
  BindStatement(statement);
  locals_.pop_back();
}

void ModuleBinding::BindStatement(Statement* statement) {
  if (statement == nullptr) {
    return;
  }
  const bool scope = statement->kind == Statement::Kind::Block ||
                     statement->kind == Statement::Kind::For ||
                     statement->kind == Statement::Kind::ForEach;
  if (scope) {
    locals_.emplace_back();
  }
  BindType(statement->type);
  BindStatement(statement->initializer.get());
  BindExpression(statement->expression.get());
  BindExpression(statement->condition.get());
  BindExpression(statement->iterable.get());
  if (statement->kind == Statement::Kind::ForEach) {
    locals_.back().insert(statement->name);
    if (statement->destructuring) {
      locals_.back().insert(statement->second_name);
    }
  }
  BindScoped(statement->then_branch.get());
  BindScoped(statement->else_branch.get());
  BindScoped(statement->finally_branch.get());
  for (auto& clause : statement->catches) {
    locals_.emplace_back();
    if (!clause.name.empty()) {
      BindType(clause.type);
      locals_.back().insert(clause.name);
    }
    BindStatement(clause.body.get());
    locals_.pop_back();
  }
  BindExpression(statement->increment.get());
  for (auto& child : statement->statements) {
    BindStatement(child.get());
  }
  for (auto& branch : statement->cases) {
    BindExpression(branch.label.get());
    BindScoped(branch.body.get());
  }
  if (statement->kind == Statement::Kind::VariableDeclaration) {
    locals_.back().insert(statement->name);
  }
  if (scope) {
    locals_.pop_back();
  }
}

void ModuleBinding::BindFunction(Function& function) {
  type_parameters_.emplace_back(function.type_parameters.begin(), function.type_parameters.end());
  locals_.emplace_back();
  BindType(function.return_type);
  for (auto& parameter : function.parameters) {
    BindType(parameter.type);
    locals_.back().insert(parameter.name);
  }
  BindStatement(function.body.get());
  locals_.pop_back();
  type_parameters_.pop_back();
}

void ModuleBinding::BindClass(Class& declaration) {
  type_parameters_.emplace_back(declaration.type_parameters.begin(),
                                declaration.type_parameters.end());
  if (declaration.base_class) {
    BindType(*declaration.base_class);
  }
  for (auto& interface : declaration.interfaces) {
    BindType(interface);
  }
  for (auto& field : declaration.fields) {
    BindType(field.type);
    BindExpression(field.initializer.get());
  }
  for (auto& method : declaration.methods) {
    BindFunction(method);
  }
  if (declaration.constructor) {
    BindFunction(*declaration.constructor);
  }
  type_parameters_.pop_back();
}

void ModuleBinding::BindProgram(Program& program) {
  const auto rename = [&](auto& declaration) -> void {
    declaration.name = scopes_.at(current_).declarations.at(declaration.name).target;
  };
  for (auto& function : program.functions) {
    BindFunction(function);
    rename(function);
  }
  for (auto& declaration : program.classes) {
    BindClass(declaration);
    rename(declaration);
  }
  for (auto& declaration : program.enums) {
    rename(declaration);
  }
  for (auto& declaration : program.interfaces) {
    for (auto& method : declaration.methods) {
      BindFunction(method);
    }
    rename(declaration);
  }
  for (auto& alias : program.aliases) {
    type_parameters_.emplace_back(alias.type_parameters.begin(), alias.type_parameters.end());
    BindType(alias.target);
    type_parameters_.pop_back();
    rename(alias);
  }
  for (auto& global : program.globals) {
    BindType(global.type);
    BindExpression(global.initializer.get());
    rename(global);
  }
}

ParseResult ModuleBinding::Run() {
  CollectDeclarations();
  if (diagnostics_.empty()) {
    BindImports();
  }
  Program combined;
  const auto can_bind = diagnostics_.empty();
  for (auto& module : modules_) {
    // 声明与导入表已经建立；函数体中的未知名字不能阻止后续依赖绑定其声明身份。
    if (can_bind) {
      BindProgram(module.program);
    }
    AppendProgram(combined, std::move(module.program));
    ++current_;
  }
  combined.imports.clear();
  combined.sources = std::move(sources_);
  return {.program = std::move(combined), .diagnostics = std::move(diagnostics_)};
}

}  // namespace

ParseResult BindModules(std::vector<SourceModule> modules, std::vector<SourceFile> sources) {
  return ModuleBinding(std::move(modules), std::move(sources)).Run();
}

}  // namespace vela::frontend
