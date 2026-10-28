#include "compiler/tooling/documentation.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <ostream>
#include <ranges>
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

namespace vela::tooling {
namespace {

using frontend::TypeName;

std::string TypeText(const TypeName& type) {
  switch (type.kind) {
    case TypeName::Kind::Named:
      return type.name;
    case TypeName::Kind::Array:
      return TypeText(type.arguments.front()) + "[]";
    case TypeName::Kind::Nullable:
      return TypeText(type.arguments.front()) + "?";
    case TypeName::Kind::Generic: {
      std::string text = type.name + '<';
      for (const auto& argument : type.arguments) {
        if (text.back() != '<') {
          text += ", ";
        }
        text += TypeText(argument);
      }
      return text + '>';
    }
    case TypeName::Kind::Function: {
      std::string text = "(";
      for (std::size_t index = 0; index + 1 < type.arguments.size(); ++index) {
        if (index != 0) {
          text += ", ";
        }
        text += TypeText(type.arguments.at(index));
      }
      return text + ") => " + TypeText(type.arguments.back());
    }
  }
  std::unreachable();
}

void TypeParameters(std::ostream& output, const std::vector<std::string>& parameters) {
  if (parameters.empty()) {
    return;
  }
  output << '<';
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    if (index != 0) {
      output << ", ";
    }
    output << parameters.at(index);
  }
  output << '>';
}

void Parameters(std::ostream& output, const std::vector<frontend::Parameter>& parameters) {
  output << '(';
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    if (index != 0) {
      output << ", ";
    }
    const auto& parameter = parameters.at(index);
    output << parameter.name << ": " << TypeText(parameter.type);
  }
  output << ')';
}

void Signature(std::ostream& output, const frontend::Function& function) {
  output << function.name;
  TypeParameters(output, function.type_parameters);
  Parameters(output, function.parameters);
  output << ": " << TypeText(function.return_type);
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
  Documentation(const frontend::Program& program, std::string_view source) : program_(&program) {
    std::size_t start = 0;
    while (start < source.size()) {
      const auto newline = source.find('\n', start);
      lines_.push_back(source.substr(
          start, newline == std::string_view::npos ? source.size() - start : newline - start));
      if (newline == std::string_view::npos) {
        break;
      }
      start = newline + 1;
    }
  }
  std::string Generate();

 private:
  void Comment(SourceLocation location);
  void Class(const frontend::Class& declaration);
  void Constructor(const frontend::Class& declaration);
  void FieldDeclaration(const frontend::Field& field);
  void Method(const frontend::Function& method);
  void Emit(Declaration declaration);

  const frontend::Program* program_;
  std::vector<std::string_view> lines_;
  std::ostringstream output_;
};

void Documentation::Comment(SourceLocation location) {
  std::vector<std::string_view> comments;
  auto line = static_cast<std::size_t>(location.line - 1);
  while (line != 0) {
    auto text = lines_.at(--line);
    const auto start = text.find_first_not_of(" \t");
    if (start == std::string_view::npos || !text.substr(start).starts_with("///")) {
      break;
    }
    text.remove_prefix(start + 3);
    if (text.starts_with(' ')) {
      text.remove_prefix(1);
    }
    if (text.ends_with('\r')) {
      text.remove_suffix(1);
    }
    comments.push_back(text);
  }
  if (comments.empty()) {
    return;
  }
  for (const auto text : std::views::reverse(comments)) {
    output_ << text << '\n';
  }
  output_ << '\n';
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

void Documentation::Class(const frontend::Class& declaration) {
  const auto* constructor = declaration.constructor.get();
  const bool public_constructor =
      constructor == nullptr || constructor->visibility == frontend::Visibility::Public;
  output_ << "class " << declaration.name;
  TypeParameters(output_, declaration.type_parameters);
  if (!declaration.interfaces.empty()) {
    output_ << " implements ";
    for (std::size_t index = 0; index < declaration.interfaces.size(); ++index) {
      if (index != 0) {
        output_ << ", ";
      }
      output_ << TypeText(declaration.interfaces.at(index));
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
    if (!public_constructor) {
      output_ << "private ";
    }
    output_ << "constructor";
    if (declaration.constructor_kind == frontend::ConstructorKind::Explicit) {
      Parameters(output_, constructor->parameters);
    }
    output_ << " { … }\n";
  }
  output_ << "}\n```\n\n";
  Constructor(declaration);
  for (const auto& field : declaration.fields) {
    if (field.visibility == frontend::Visibility::Private) {
      continue;
    }
    output_ << "### `" << field.name << "`\n\n";
    Comment(field.location);
    output_ << "Type: `" << TypeText(field.type) << "`";
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
  output_ << field.name << ": " << TypeText(field.type);
  if (field.initializer) {
    output_ << " = …";
  }
  output_ << ";\n";
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
    output_ << field.name << ": " << TypeText(field.type);
    if (field.initializer) {
      output_ << " = …";
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
      output_ << "interface " << interface.name << " {\n";
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
      output_ << " = " << TypeText(alias.target) << ";\n";
      break;
    }
    case Declaration::Kind::Global: {
      const auto& global = program.globals.at(declaration.index);
      output_ << (global.binding == frontend::BindingKind::Constant ? "const " : "let ")
              << global.name;
      if (!global.type.name.empty()) {
        output_ << ": " << TypeText(global.type);
      }
      output_ << " = …;\n";
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

std::expected<std::string, frontend::Diagnostics> GenerateModuleDocumentation(
    std::string_view source) {
  const auto parsed = frontend::ParseSource(source);
  if (!parsed.diagnostics.empty()) {
    return std::unexpected(parsed.diagnostics);
  }
  return Documentation(parsed.program, source).Generate();
}

std::expected<std::string, frontend::Diagnostics> GenerateDocumentation(std::string_view entry) {
  auto loaded = frontend::LoadProgram(entry);
  if (!loaded.diagnostics.empty()) {
    return std::unexpected(std::move(loaded.diagnostics));
  }
  auto checked = frontend::CheckProgram(loaded.program);
  if (!checked.Succeeded()) {
    return std::unexpected(std::move(checked.diagnostics));
  }
  const auto& source = loaded.program.sources.front();
  return GenerateModuleDocumentation(source.text);
}

}  // namespace vela::tooling
