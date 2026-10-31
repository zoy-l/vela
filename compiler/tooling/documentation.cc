#include "compiler/tooling/documentation.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <ostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/source_location.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/checker.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/parser.h"
#include "compiler/frontend/source_loader.h"
#include "compiler/frontend/token.h"

namespace vela::tooling {
namespace {

void TypeParameters(std::ostream& output, const std::vector<frontend::TypeParameter>& parameters) {
  if (parameters.empty()) {
    return;
  }
  output << '<';
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    const auto& parameter = parameters.at(index);
    if (index != 0) {
      output << ", ";
    }
    output << parameter.name;
    if (parameter.bound) {
      output << " extends " << parameter.bound->ToString();
    }
  }
  output << '>';
}

void Parameters(std::ostream& output,
                const std::vector<frontend::Parameter>& parameters,
                std::span<const std::string> optional = {}) {
  output << '(';
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    if (index != 0) {
      output << ", ";
    }
    const auto& parameter = parameters.at(index);
    output << parameter.name << ": " << parameter.type.ToString();
    if (std::ranges::find(optional, parameter.name) != optional.end()) {
      output << " = null";
    }
  }
  output << ')';
}

void Signature(std::ostream& output, const frontend::Function& function) {
  output << function.name << FunctionSignature(function);
}

struct Declaration final {
  enum class Kind : std::uint8_t { Function, Class, Interface, Enum, Alias, Global };
  Kind kind = Kind::Function;
  std::size_t index = 0;
  SourceLocation location;
  std::string_view name;
};

template <class T>
void AddDeclarations(std::vector<Declaration>& output,
                     const std::vector<T>& input,
                     Declaration::Kind kind) {
  for (std::size_t index = 0; index < input.size(); ++index) {
    const auto& declaration = input.at(index);
    if (declaration.export_kind == frontend::ExportKind::Exported) {
      output.push_back({
          .kind = kind,
          .index = index,
          .location = declaration.location,
          .name = declaration.name,
      });
    }
  }
}

class Documentation final {
 public:
  Documentation(const frontend::Program& program, const frontend::SourceSnapshot& source)
      : program_(&program), source_(&source) {}
  std::string Generate();

 private:
  void Comment(SourceLocation location);
  void Class(const frontend::Class& declaration);
  void ClassDeclaration(const frontend::Class& declaration);
  void Constructor(const frontend::Class& declaration);
  void FieldDeclaration(const frontend::Field& field);
  void Initializer(const frontend::Expression& expression);
  void Method(const frontend::Function& method);
  void Emit(Declaration declaration);

  const frontend::Program* program_;
  const frontend::SourceSnapshot* source_;
  std::ostringstream output_;
};

void Documentation::Comment(SourceLocation location) {
  const auto comment = DocumentationComment(source_->text, source_->comments, location);
  if (!comment.empty()) {
    output_ << comment << "\n\n";
  }
}

void Documentation::Method(const frontend::Function& method) {
  output_ << "### `" << method.name << "`\n\n";
  Comment(method.location);
  output_ << "```vela\n";
  if (method.storage == frontend::MemberStorage::Static) {
    output_ << "static ";
  }
  Signature(output_, method);
  output_ << ";\n```\n\n";
}

void Documentation::ClassDeclaration(const frontend::Class& declaration) {
  const auto* constructor = declaration.constructor.get();
  output_ << "class " << declaration.name;
  TypeParameters(output_, declaration.type_parameters);
  if (!declaration.interfaces.empty()) {
    output_ << " implements ";
    for (std::size_t index = 0; index < declaration.interfaces.size(); ++index) {
      if (index != 0) {
        output_ << ", ";
      }
      output_ << declaration.interfaces.at(index).ToString();
    }
  }
  output_ << " {\n";
  for (const auto& field : declaration.fields) {
    if (field.visibility == frontend::Visibility::Private) {
      continue;
    }
    FieldDeclaration(field);
  }
  if (constructor != nullptr) {
    output_ << "  ";
    if (constructor->visibility != frontend::Visibility::Public) {
      output_ << "private ";
    }
    output_ << "constructor";
    if (declaration.constructor_kind == frontend::ConstructorKind::Explicit) {
      Parameters(output_, constructor->parameters);
    }
    output_ << " { … }\n";
  }
  output_ << "}\n```\n\n";
}

void Documentation::Class(const frontend::Class& declaration) {
  ClassDeclaration(declaration);
  Constructor(declaration);
  const auto* constructor = declaration.constructor.get();
  const bool public_constructor =
      constructor == nullptr || constructor->visibility == frontend::Visibility::Public;
  for (const auto& field : declaration.fields) {
    if (field.visibility == frontend::Visibility::Private) {
      continue;
    }
    output_ << "### `" << field.name << "`\n\n";
    Comment(field.location);
    output_ << "Type: `" << field.type.ToString() << "`";
    if (public_constructor && declaration.IsConstructorField(field)) {
      output_ << "; named constructor parameter";
    }
    if (field.initializer) {
      output_ << "; has a default initializer";
    }
    output_ << ".\n\n";
  }
  for (const auto& method : declaration.methods) {
    if (method.visibility == frontend::Visibility::Public) {
      Method(method);
    }
  }
}

void Documentation::FieldDeclaration(const frontend::Field& field) {
  output_ << "  ";
  if (field.storage == frontend::MemberStorage::Static) {
    output_ << "static ";
  }
  output_ << field.name << ": " << field.type.ToString();
  if (field.initializer) {
    Initializer(*field.initializer);
  }
  output_ << ";\n";
}

void Documentation::Initializer(const frontend::Expression& expression) {
  output_ << " = ";
  if (expression.kind != frontend::Expression::Kind::MapLiteral) {
    output_ << "…";
    return;
  }
  const auto& tokens = source_->tokens;
  const auto start = std::ranges::find(tokens, expression.location, &frontend::Token::location);
  auto end = start + 1;
  std::size_t depth = 1;
  while (depth != 0) {
    if (end->kind == frontend::TokenKind::LeftBrace) {
      ++depth;
    } else if (end->kind == frontend::TokenKind::RightBrace) {
      --depth;
    }
    ++end;
  }
  const auto& last = *(end - 1);
  output_ << std::string_view(source_->text)
                 .substr(start->offset, last.offset + last.length - start->offset);
}

void Documentation::Constructor(const frontend::Class& declaration) {
  if (declaration.constructor &&
      declaration.constructor->visibility == frontend::Visibility::Private) {
    return;
  }
  if (declaration.constructor_kind == frontend::ConstructorKind::Explicit) {
    output_ << "### Constructor\n\n```vela\n" << declaration.name;
    Parameters(output_, declaration.constructor->parameters);
    output_ << ";\n```\n\n";
    return;
  }
  output_ << "### Constructor\n\n```vela\n" << declaration.name << '(';
  std::size_t parameter_index = 0;
  for (const auto& field : declaration.fields) {
    if (!declaration.IsConstructorField(field)) {
      continue;
    }
    if (parameter_index++ != 0) {
      output_ << ", ";
    }
    output_ << field.name << ": " << field.type.ToString();
    if (field.initializer) {
      Initializer(*field.initializer);
    }
  }
  output_ << ");\n```\n\n";
}

void Documentation::Emit(Declaration declaration) {
  const auto& program = *program_;
  output_ << "## `" << declaration.name << "`\n\n";
  Comment(declaration.location);
  output_ << "```vela\n";
  switch (declaration.kind) {
    case Declaration::Kind::Function: {
      const auto& function = program.functions.at(declaration.index);
      if (function.is_async) {
        output_ << "async ";
      }
      output_ << (function.native_symbol.empty() ? "func " : "external func ");
      Signature(output_, function);
      output_ << ";\n";
      break;
    }
    case Declaration::Kind::Class:
      Class(program.classes.at(declaration.index));
      return;
    case Declaration::Kind::Interface: {
      const auto& interface = program.interfaces.at(declaration.index);
      output_ << "interface " << interface.name;
      TypeParameters(output_, interface.type_parameters);
      output_ << " {\n";
      for (const auto& method : interface.methods) {
        output_ << "  ";
        Signature(output_, method);
        output_ << ";\n";
      }
      output_ << "}\n```\n\n";
      for (const auto& method : interface.methods) {
        Method(method);
      }
      return;
    }
    case Declaration::Kind::Enum: {
      const auto& enumeration = program.enums.at(declaration.index);
      output_ << "enum " << enumeration.name << " {\n";
      for (const auto& member : enumeration.members) {
        output_ << "  " << member.name << ",\n";
      }
      output_ << "}\n";
      break;
    }
    case Declaration::Kind::Alias: {
      const auto& alias = program.aliases.at(declaration.index);
      output_ << "type " << alias.name;
      TypeParameters(output_, alias.type_parameters);
      output_ << " = " << alias.target.ToString() << ";\n";
      break;
    }
    case Declaration::Kind::Global: {
      const auto& global = program.globals.at(declaration.index);
      output_ << (global.binding == frontend::BindingKind::Constant ? "const " : "let ")
              << global.name;
      if (!global.type.name.empty()) {
        output_ << ": " << global.type.ToString();
      }
      Initializer(*global.initializer);
      output_ << ";\n";
      break;
    }
  }
  output_ << "```\n\n";
}

std::string Documentation::Generate() {
  std::vector<Declaration> declarations;
  const auto& program = *program_;
  AddDeclarations(declarations, program.functions, Declaration::Kind::Function);
  AddDeclarations(declarations, program.classes, Declaration::Kind::Class);
  AddDeclarations(declarations, program.interfaces, Declaration::Kind::Interface);
  AddDeclarations(declarations, program.enums, Declaration::Kind::Enum);
  AddDeclarations(declarations, program.aliases, Declaration::Kind::Alias);
  AddDeclarations(declarations, program.globals, Declaration::Kind::Global);
  std::ranges::sort(declarations, {},
                    [](const auto& declaration) -> std::pair<std::uint32_t, std::uint32_t> {
                      return std::pair(declaration.location.line, declaration.location.column);
                    });
  output_ << "# Module API\n\n";
  if (declarations.empty()) {
    output_ << "This module has no exported declarations.\n";
  }
  for (const auto& declaration : declarations) {
    Emit(declaration);
  }
  return std::move(output_).str();
}

}  // namespace

std::string FunctionSignature(const frontend::Function& function) {
  std::ostringstream output;
  TypeParameters(output, function.type_parameters);
  Parameters(output, function.parameters, function.optional_parameters);
  output << ": " << function.return_type.ToString();
  return std::move(output).str();
}

std::string DocumentationComment(std::string_view source,
                                 std::span<const frontend::SourceComment> comments,
                                 SourceLocation location) {
  auto line = location.line - 1;
  auto previous = std::ranges::lower_bound(
      comments, location.line, {}, [](const auto& comment) { return comment.location.line; });
  std::vector<std::string_view> lines;
  while (previous != comments.begin() && line > 0) {
    const auto& comment = *--previous;
    auto content = source.substr(comment.offset, comment.length);
    if (comment.location.line != line || !content.starts_with("///")) {
      break;
    }
    const auto newline =
        comment.offset == 0 ? std::string_view::npos : source.rfind('\n', comment.offset - 1);
    const auto start = newline == std::string_view::npos ? 0 : newline + 1;
    if (source.substr(start, comment.offset - start).find_first_not_of(" \t") !=
        std::string_view::npos) {
      break;
    }
    content.remove_prefix(3);
    if (content.starts_with(' ')) {
      content.remove_prefix(1);
    }
    if (content.ends_with('\r')) {
      content.remove_suffix(1);
    }
    lines.push_back(content);
    --line;
  }
  std::string result;
  for (auto item = lines.rbegin(); item != lines.rend(); ++item) {
    if (item != lines.rbegin()) {
      result += '\n';
    }
    result += *item;
  }
  return result;
}

std::expected<std::string, frontend::Diagnostics> GenerateModuleDocumentation(
    std::string_view source) {
  const auto parsed = frontend::ParseSourceSnapshot(std::string(source));
  if (!parsed.diagnostics.empty()) {
    return std::unexpected(parsed.diagnostics);
  }
  return Documentation(parsed.program, *parsed.source).Generate();
}

DocumentationResult GenerateDocumentation(std::string_view entry,
                                          frontend::DiagnosticOptions options) {
  auto graph = frontend::LoadModuleGraph(entry);
  if (!graph.diagnostics.empty()) {
    return {.markdown = {}, .diagnostics = std::move(graph.diagnostics)};
  }
  auto documentation =
      Documentation(graph.modules.front().program, *graph.sources.front().syntax).Generate();
  auto loaded = frontend::BindModuleGraph(std::move(graph));
  if (!loaded.diagnostics.empty()) {
    return {.markdown = {}, .diagnostics = std::move(loaded.diagnostics)};
  }
  auto checked = frontend::CheckProgram(loaded.program, options);
  if (!checked.Succeeded()) {
    return {.markdown = {}, .diagnostics = std::move(checked.diagnostics)};
  }
  return {.markdown = std::move(documentation), .diagnostics = std::move(checked.diagnostics)};
}

}  // namespace vela::tooling
