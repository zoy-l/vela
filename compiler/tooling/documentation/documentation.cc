#include "compiler/tooling/documentation/documentation.h"
#include "compiler/frontend/syntax/names.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "base/source_location.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/syntax/ast_clone.h"
#include "compiler/frontend/syntax/parser.h"
#include "compiler/frontend/syntax/token.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/frontend/typing/type_checker.h"
#include "compiler/tooling/semantic/semantic_index.h"
#include "compiler/tooling/semantic/semantic_syntax.h"

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

template <class Parameters, class Render>
void RenderParameters(std::ostream& output, const Parameters& parameters, const Render& render) {
  output << '(';
  std::string_view separator;
  for (const auto& parameter : parameters) {
    output << separator;
    const auto default_value = render(parameter);
    if (!default_value.empty()) {
      output << " = " << default_value;
    }
    separator = ", ";
  }
  output << ')';
}

void Parameters(std::ostream& output,
                const std::vector<frontend::Parameter>& parameters,
                std::span<const std::string> optional = {}) {
  RenderParameters(output, parameters, [&](const auto& parameter) -> std::string_view {
    if (parameter.binding != frontend::Parameter::Binding::Local) {
      const bool is_field = parameter.binding == frontend::Parameter::Binding::Field;
      output << (is_field ? "this." : "super.")
             << (is_field ? parameter.member_name : parameter.name);
      return {};
    }
    output << parameter.name << ": " << parameter.type.ToString();
    const auto* initial = parameter.initializer.get();
    if (initial != nullptr) {
      return initial->kind == frontend::Expression::Kind::NullLiteral ? "null" : "…";
    }
    return std::ranges::find(optional, parameter.name) != optional.end() ? "null" : "";
  });
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
  std::span<const frontend::Attribute> attributes;
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
          .attributes = DeclarationAttributes(declaration),
      });
    }
  }
}

class Documentation final {
 public:
  Documentation(const frontend::Program& program,
                const frontend::SourceSnapshot& source,
                const frontend::SemanticModel& model,
                std::span<const frontend::Class> class_templates)
      : program_(&program), source_(&source), model_(&model) {
    class_templates_.reserve(class_templates.size());
    for (const auto& declaration : class_templates) {
      class_templates_.emplace(declaration.name, &declaration);
    }
  }
  std::string Generate();

 private:
  void Comment(SourceLocation location);
  void Attributes(std::span<const frontend::Attribute> attributes, std::string_view indent = "");
  void Class(const frontend::Class& declaration);
  void ClassDeclaration(const frontend::Class& declaration);
  void Constructor(const frontend::Class& declaration);
  void FieldDeclaration(const frontend::Field& field);
  void Initializer(const frontend::Expression& expression);
  [[nodiscard]] std::string_view SourceText(const frontend::Token& first,
                                            const frontend::Token& last) const;
  void Method(const frontend::Function& method);
  void Emit(Declaration declaration);

  const frontend::Program* program_;
  const frontend::SourceSnapshot* source_;
  const frontend::SemanticModel* model_;
  absl::flat_hash_map<std::string_view, const frontend::Class*> class_templates_;
  std::ostringstream output_;
};

void Documentation::Comment(SourceLocation location) {
  const auto comment = DocumentationComment(source_->text, source_->comments, location);
  if (!comment.empty()) {
    output_ << comment << "\n\n";
  }
}

void Documentation::Attributes(std::span<const frontend::Attribute> attributes,
                               std::string_view indent) {
  const auto& tokens = source_->tokens;
  for (const auto& attribute : attributes) {
    const auto begin =
        tokens.begin() + static_cast<std::ptrdiff_t>(TokenIndexAt(tokens, attribute.location));
    const auto end = std::find_if(begin + 1, tokens.end(), [](const auto& token) {
      return token.kind == frontend::TokenKind::RightBracket &&
             token.role == frontend::TokenRole::AttributeDelimiter;
    });
    output_ << indent << SourceText(*begin, *end) << '\n';
  }
}

void Documentation::Method(const frontend::Function& method) {
  output_ << "### `" << method.name << "`\n\n";
  Comment(DocumentationLocation(method));
  output_ << "```vela\n";
  Attributes(method.attributes);
  if (method.storage == frontend::MemberStorage::Static) {
    output_ << "static ";
  }
  Signature(output_, method);
  output_ << ";\n```\n\n";
}

void Documentation::ClassDeclaration(const frontend::Class& declaration) {
  const auto* constructor = declaration.constructor.get();
  const auto& native_key = declaration.native_key;
  if (!native_key.empty()) {
    output_ << "external ";
  }
  output_ << "class " << declaration.name;
  if (!native_key.empty()) {
    output_ << " = '" << native_key << '\'';
  }
  TypeParameters(output_, declaration.type_parameters);
  if (declaration.base_class) {
    output_ << " extends " << declaration.base_class->ToString();
  }
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
    Attributes(constructor->attributes, "  ");
    output_ << "  ";
    if (constructor->visibility == frontend::Visibility::Private) {
      output_ << frontend::kPrivateNamePrefix;
    }
    output_ << "constructor";
    if (declaration.constructor_kind == frontend::ConstructorKind::Explicit) {
      Parameters(output_, constructor->parameters);
    }
    output_ << (native_key.empty() ? " { … }\n" : ";\n");
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
    Comment(DocumentationLocation(field));
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
  Attributes(field.attributes, "  ");
  output_ << "  ";
  if (field.storage == frontend::MemberStorage::Static) {
    output_ << "static ";
  }
  if (field.binding == frontend::BindingKind::Constant) {
    output_ << "const ";
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
  const auto start =
      tokens.begin() + static_cast<std::ptrdiff_t>(TokenIndexAt(tokens, expression.location));
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
  output_ << SourceText(*start, last);
}

std::string_view Documentation::SourceText(const frontend::Token& first,
                                           const frontend::Token& last) const {
  return std::string_view(source_->text)
      .substr(first.offset, last.offset + last.length - first.offset);
}

void Documentation::Constructor(const frontend::Class& declaration) {
  if (declaration.constructor_kind == frontend::ConstructorKind::Unavailable) {
    return;
  }
  if (declaration.constructor &&
      declaration.constructor->visibility == frontend::Visibility::Private) {
    return;
  }
  output_ << "### Constructor\n\n";
  if (declaration.base_class && !declaration.constructor &&
      std::ranges::none_of(declaration.fields, [&](const auto& field) {
        return declaration.IsConstructorField(field);
      })) {
    output_ << "Constructor signature inherited from `" << declaration.base_class->ToString()
            << "`.\n\n";
  }
  // 泛型参数身份由绑定阶段赋予；原始声明仅保留用户书写的类型和导入别名。
  const auto& bound =
      declaration.type_parameters.empty() ? declaration : *class_templates_.at(declaration.name);
  const auto& signature = model_->FindClassDescriptor(bound)->constructor;
  output_ << "```vela\n" << declaration.name;
  RenderParameters(output_, signature.parameters, [&](const auto& parameter) -> std::string_view {
    output_ << parameter.name << ": " << DisplayType(*model_, parameter.type);
    return signature.optional_parameters.contains(parameter.name) ? "…" : "";
  });
  output_ << ";\n```\n\n";
}

void Documentation::Emit(Declaration declaration) {
  const auto& program = *program_;
  output_ << "## `" << declaration.name << "`\n\n";
  Comment(DocumentationLocation(declaration));
  output_ << "```vela\n";
  Attributes(declaration.attributes);
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
        Attributes(method.attributes, "  ");
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
        Attributes(member.attributes, "  ");
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
      if (global.type.kind != frontend::TypeName::Kind::Inferred) {
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

DocumentationResult GenerateDocumentation(std::string_view entry,
                                          frontend::DiagnosticOptions options,
                                          const frontend::ModulePaths& imports) {
  auto graph = frontend::LoadModuleGraph(entry, imports);
  if (!graph.diagnostics.empty()) {
    return {.markdown = {}, .diagnostics = std::move(graph.diagnostics)};
  }
  const auto original = frontend::CloneProgramDeclaration(graph.modules.front().program);
  const auto source = graph.sources.front().syntax;
  auto loaded = frontend::BindModuleGraph(std::move(graph));
  if (!loaded.diagnostics.empty()) {
    return {.markdown = {}, .diagnostics = std::move(loaded.diagnostics)};
  }
  auto& program = loaded.program;
  auto checked = frontend::TypeChecker(options).Check(program);
  if (!checked.Succeeded()) {
    return {.markdown = {}, .diagnostics = std::move(checked.diagnostics)};
  }
  auto documentation =
      Documentation(original, *source, checked.model, program.templates.classes).Generate();
  return {.markdown = std::move(documentation), .diagnostics = std::move(checked.diagnostics)};
}

}  // namespace vela::tooling
