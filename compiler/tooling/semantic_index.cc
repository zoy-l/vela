#include "compiler/tooling/semantic_index.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "base/path.h"
#include "base/source_location.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/lexer.h"
#include "compiler/frontend/parser.h"
#include "compiler/frontend/source_loader.h"
#include "compiler/frontend/specialization.h"
#include "compiler/frontend/standard_environment.h"
#include "compiler/frontend/token.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/tooling/source_text.h"

namespace vela::tooling {

std::string DisplaySymbolName(std::string_view name) {
  std::string text(name);
  for (auto begin = text.find("$module"); begin != std::string::npos;
       begin = text.find("$module", begin)) {
    const auto end = text.find('.', begin);
    if (end == std::string::npos) {
      break;
    }
    text.erase(begin, end - begin + 1);
  }
  return text;
}

namespace {

using frontend::Expression;
using frontend::Statement;
using frontend::TokenKind;
using frontend::TypeName;

std::string TypeText(const TypeName& type) {
  switch (type.kind) {
    case TypeName::Kind::Named:
      return DisplaySymbolName(type.name);
    case TypeName::Kind::Array:
    case TypeName::Kind::Nullable: {
      const auto& element = type.arguments.front();
      auto text = TypeText(element);
      if (element.kind == TypeName::Kind::Function) {
        text = '(' + text + ')';
      }
      return text + (type.kind == TypeName::Kind::Array ? "[]" : "?");
    }
    case TypeName::Kind::Generic: {
      auto text = DisplaySymbolName(type.name) + '<';
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
        text += (index == 0 ? "" : ", ") + TypeText(type.arguments.at(index));
      }
      return text + ") => " + TypeText(type.arguments.back());
    }
  }
  std::unreachable();
}

std::string Signature(const frontend::Function& function) {
  std::string text;
  for (const auto& parameter : function.type_parameters) {
    text += (text.empty() ? "<" : ", ") + parameter;
  }
  text += text.empty() ? "(" : ">(";
  for (const auto& parameter : function.parameters) {
    if (text.back() != '(') {
      text += ", ";
    }
    text += parameter.name + ": " + TypeText(parameter.type);
  }
  return text + "): " + TypeText(function.return_type);
}

std::string GenericName(std::string_view source_name, std::span<const std::string> parameters) {
  auto name = DisplaySymbolName(source_name);
  if (!parameters.empty()) {
    name += '<';
    for (const auto& parameter : parameters) {
      name += (name.back() == '<' ? "" : ", ") + parameter;
    }
    name += '>';
  }
  return name;
}

std::optional<TokenKind> Closing(TokenKind kind) {
  switch (kind) {
    case TokenKind::LeftBrace:
      return TokenKind::RightBrace;
    case TokenKind::LeftBracket:
      return TokenKind::RightBracket;
    case TokenKind::LeftParen:
      return TokenKind::RightParen;
    default:
      return std::nullopt;
  }
}

bool IsIdentifierByte(char value) {
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
         (value >= '0' && value <= '9') || value == '_';
}

void CloseCompletionDelimiters(std::string& text, std::span<const frontend::Token> tokens) {
  std::vector<TokenKind> closing;
  for (const auto& token : tokens) {
    if (const auto close = Closing(token.kind)) {
      closing.push_back(*close);
    } else if (!closing.empty() && token.kind == closing.back()) {
      closing.pop_back();
    }
  }
  for (auto item = closing.rbegin(); item != closing.rend(); ++item) {
    switch (*item) {
      case TokenKind::RightBrace:
        text += '}';
        break;
      case TokenKind::RightBracket:
        text += ']';
        break;
      case TokenKind::RightParen:
        text += ')';
        break;
      default:
        std::unreachable();
    }
  }
}

}  // namespace

std::string CompletionSource(std::string_view source, std::size_t offset) {
  auto lexed = frontend::Lexer(source, 0, frontend::CommentRetention::Preserve).Tokenize();
  // 注释与字面量中不制造表达式；插值内部的表达式仍由生产 lexer 分别标记。
  for (const auto& comment : lexed.comments) {
    if (offset >= comment.offset && offset <= comment.offset + comment.length) {
      return std::string(source);
    }
  }
  for (const auto& token : lexed.tokens) {
    if (offset > token.offset && offset < token.offset + token.length &&
        (token.kind == TokenKind::StringLiteral || token.kind == TokenKind::StringHead ||
         token.kind == TokenKind::StringMiddle || token.kind == TokenKind::StringTail)) {
      return std::string(source);
    }
  }
  std::size_t end = offset;
  while (end < source.size() && IsIdentifierByte(source.at(end))) {
    ++end;
  }
  std::string text(source);
  // 保留已经输入的前缀，光标位置仍落在同一个 token 内。
  text.insert(end, "__vela_completion__");
  lexed = frontend::Lexer(text).Tokenize();
  CloseCompletionDelimiters(text, lexed.tokens);
  return text;
}

SemanticIndex::SemanticIndex(std::string_view path,
                             std::span<const frontend::SourceInput> overlays) {
  auto loaded = frontend::LoadProgram(path, overlays, frontend::SourceLoadMode::Editor);
  program_ = std::move(loaded.program);
  for (const auto& resolution : program_.file_resolutions) {
    resolutions_.emplace(resolution.requested_path, resolution.canonical_path);
  }
  sources_.reserve(program_.sources.size());
  for (std::uint32_t index = 0; index < program_.sources.size(); ++index) {
    const auto& file = program_.sources.at(index);
    auto text = SourceText::Create(file.text);
    if (!text) {
      // 无效 UTF-8 已由 loader/lexer 报错，不能构建协议位置索引。
      sources_.clear();
      return;
    }
    auto lexed = frontend::Lexer(file.text, index, frontend::CommentRetention::Preserve).Tokenize();
    Source source{
        .path = file.path,
        .text = std::move(*text),
        .tokens = std::move(lexed.tokens),
        .comments = std::move(lexed.comments),
        .closing = {},
        .imports = {},
    };
    source.closing.resize(source.tokens.size());
    std::vector<std::size_t> opening;
    for (std::size_t token_index = 0; token_index < source.tokens.size(); ++token_index) {
      source.closing.at(token_index) = token_index;
      const auto kind = source.tokens.at(token_index).kind;
      if (Closing(kind)) {
        opening.push_back(token_index);
      } else if (!opening.empty() && Closing(source.tokens.at(opening.back()).kind) == kind) {
        source.closing.at(opening.back()) = token_index;
        opening.pop_back();
      }
    }
    source.imports = frontend::ParseSource(file.text, index).program.imports;
    source_ids_.emplace(file.path, index);
    sources_.push_back(std::move(source));
  }
  if (sources_.empty()) {
    return;
  }
  // 先保留模板声明；specializer 会从执行用 AST 中移除未实例化模板。
  CollectDeclarations();
  // 特化会展开别名并移除模板，先记录源码类型引用，保留其声明身份。
  VisitProgram();
  const auto diagnostics = frontend::SpecializeGenerics(program_);
  if (diagnostics.empty()) {
    model_ = frontend::TypeChecker().Check(program_).model;
    CollectDeclarations();
    phase_ = IndexPhase::Checked;
    VisitProgram();
  }
  CollectImports();
}

std::size_t SemanticIndex::TokenAt(SourceLocation location) const {
  const auto& source = sources_.at(location.source_id);
  const auto offset = source.text
                          .OffsetAt({.line = location.line - 1, .character = location.column - 1},
                                    PositionEncoding::Utf8)
                          .value();
  const auto& tokens = source.tokens;
  const auto found = std::ranges::lower_bound(tokens, offset, {}, &frontend::Token::offset);
  return static_cast<std::size_t>(found - tokens.begin());
}

SemanticIndex::Key SemanticIndex::KeyAt(SourceLocation location) const {
  const auto& source = sources_.at(location.source_id);
  return {location.source_id, source.tokens.at(TokenAt(location)).offset};
}

SemanticIndex::Scope SemanticIndex::Extent(const Statement& statement) const {
  const auto source_id = statement.location.source_id;
  const auto& source = sources_.at(source_id);
  const auto start = TokenAt(statement.location);
  auto end = start;
  if (statement.kind == Statement::Kind::Block &&
      source.tokens.at(start).kind == TokenKind::LeftBrace) {
    end = source.closing.at(start);
  } else if (statement.kind == Statement::Kind::If || statement.kind == Statement::Kind::While ||
             statement.kind == Statement::Kind::For || statement.kind == Statement::Kind::Try) {
    auto extent = Scope{
        .source = source_id,
        .begin = source.tokens.at(start).offset,
        .end = source.tokens.at(start).offset,
    };
    for (const auto* child : {
             statement.then_branch.get(),
             statement.else_branch.get(),
             statement.finally_branch.get(),
         }) {
      if (child != nullptr) {
        extent.end = Extent(*child).end;
      }
    }
    for (const auto& clause : statement.catches) {
      extent.end = std::max(extent.end, Extent(*clause.body).end);
    }
    return extent;
  } else if (statement.kind == Statement::Kind::Block && !statement.statements.empty()) {
    return {
        .source = source_id,
        .begin = source.tokens.at(start).offset,
        .end = Extent(*statement.statements.back()).end,
    };
  } else {
    for (; end + 1 < source.tokens.size(); ++end) {
      const auto kind = source.tokens.at(end).kind;
      if (kind == TokenKind::Semicolon || kind == TokenKind::RightBrace) {
        break;
      }
      if (kind == TokenKind::LeftBrace && statement.kind == Statement::Kind::Switch) {
        end = source.closing.at(end);
        break;
      }
      end = source.closing.at(end);
    }
  }
  const auto& last = source.tokens.at(end);
  return {
      .source = source_id,
      .begin = source.tokens.at(start).offset,
      .end = last.offset + last.length,
  };
}

std::size_t SemanticIndex::AddSymbol(SourceLocation location, Completion completion) {
  const auto& source = sources_.at(location.source_id);
  auto token = TokenAt(location);
  // match 绑定的位置指向 let，catch 指向关键字；普通声明已经指向名称。
  if (source.tokens.at(token).kind == TokenKind::Let) {
    ++token;
  } else if (source.tokens.at(token).kind == TokenKind::Catch) {
    token += 2;
  }
  const auto& name = source.tokens.at(token);
  const Key key{location.source_id, name.offset};
  const auto [found, inserted] = declarations_.try_emplace(key, symbols_.size());
  if (inserted) {
    completion.label = name.lexeme;
    completion.documentation = DocumentationAt(location);
    symbols_.push_back({
        .completion = std::move(completion),
        .owner = {},
        .key = key,
        .length = name.length,
    });
    references_.try_emplace(key, found->second);
  }
  return found->second;
}

void SemanticIndex::CollectDeclarations() {
  const auto declare = [this](const auto& declaration, CompletionKind kind,
                              const std::string& detail) -> std::size_t {
    const auto symbol_id =
        AddSymbol(declaration.location,
                  {.label = declaration.name, .detail = detail, .kind = kind, .documentation = {}});
    symbols_.at(symbol_id).export_kind = declaration.export_kind;
    names_.insert_or_assign(declaration.name, symbol_id);
    return symbol_id;
  };
  const auto member = [this](std::string_view owner, const auto& declaration, CompletionKind kind,
                             const std::string& detail) -> std::size_t {
    const auto symbol_id =
        AddSymbol(declaration.location,
                  {.label = declaration.name, .detail = detail, .kind = kind, .documentation = {}});
    auto& symbol = symbols_.at(symbol_id);
    if (symbol.owner.empty()) {
      symbol.owner = DisplaySymbolName(owner);
    }
    symbol.visibility = declaration.visibility;
    symbol.storage = declaration.storage;
    members_.try_emplace(owner).first->second.insert_or_assign(declaration.name, symbol_id);
    return symbol_id;
  };
  for (const auto& declaration : program_.functions) {
    const auto symbol = declare(declaration, CompletionKind::Function, Signature(declaration));
    CollectParameters(symbol, declaration);
  }
  for (const auto& declaration : program_.globals) {
    const auto checked = model_.globals.find(declaration.name);
    declare(declaration,
            declaration.binding == frontend::BindingKind::Constant ? CompletionKind::Constant
                                                                   : CompletionKind::Variable,
            TypeText(declaration.type));
    if (checked != model_.globals.end()) {
      symbols_.at(names_.at(declaration.name)).completion.detail =
          DisplaySymbolName(checked->second.type.name);
    }
  }
  for (const auto& declaration : program_.aliases) {
    declare(declaration, CompletionKind::Type,
            "type " + GenericName(declaration.name, declaration.type_parameters) + " = " +
                TypeText(declaration.target));
  }
  for (const auto& declaration : program_.classes) {
    const auto class_symbol = declare(declaration, CompletionKind::Class, ClassDetail(declaration));
    for (const auto& field : declaration.fields) {
      const auto field_symbol =
          member(declaration.name, field, CompletionKind::Field, TypeText(field.type));
      if (declaration.constructor_kind == frontend::ConstructorKind::Generated &&
          field.role == frontend::FieldRole::Parameter) {
        parameters_.try_emplace(class_symbol)
            .first->second.insert_or_assign(field.name, field_symbol);
      }
    }
    for (const auto& method : declaration.methods) {
      const auto method_symbol =
          member(declaration.name, method, CompletionKind::Method, Signature(method));
      CollectParameters(method_symbol, method);
    }
    if (declaration.constructor &&
        declaration.constructor_kind == frontend::ConstructorKind::Explicit) {
      CollectParameters(class_symbol, *declaration.constructor);
    }
  }
  for (const auto& declaration : program_.interfaces) {
    declare(declaration, CompletionKind::Interface, "interface");
    for (const auto& method : declaration.methods) {
      const auto method_symbol =
          member(declaration.name, method, CompletionKind::Method, Signature(method));
      CollectParameters(method_symbol, method);
    }
  }
  for (const auto& declaration : program_.enums) {
    declare(declaration, CompletionKind::Enum, "enum");
    CollectEnumMembers(declaration);
  }
}

void SemanticIndex::CollectEnumMembers(const frontend::Enum& declaration) {
  auto& members = members_.try_emplace(declaration.name).first->second;
  const auto owner = DisplaySymbolName(declaration.name);
  std::size_t ordinal = 0;
  for (const auto& value : declaration.members) {
    const auto symbol_id = AddSymbol(
        value.location, {
                            .label = value.name,
                            .detail = owner + '.' + value.name + " = " + std::to_string(ordinal++),
                            .kind = CompletionKind::EnumMember,
                            .documentation = {},
                        });
    symbols_.at(symbol_id).storage = frontend::MemberStorage::Static;
    members.insert_or_assign(value.name, symbol_id);
  }
}

void SemanticIndex::CollectParameters(std::size_t symbol, const frontend::Function& function) {
  auto& declared = parameters_.try_emplace(symbol).first->second;
  for (const auto& parameter : function.parameters) {
    const auto parameter_id = AddSymbol(parameter.location, {
                                                                .label = parameter.name,
                                                                .detail = TypeText(parameter.type),
                                                                .kind = CompletionKind::Variable,
                                                                .documentation = {},
                                                            });
    declared.insert_or_assign(parameter.name, parameter_id);
    symbols_.at(parameter_id).role = Symbol::Role::Parameter;
  }
}

void SemanticIndex::AddReference(SourceLocation location, std::string_view name) {
  const auto found = names_.find(name);
  if (found == names_.end()) {
    return;
  }
  references_.try_emplace(KeyAt(location), found->second);
}

void SemanticIndex::VisitType(const TypeName& type) {
  if (type.name.empty()) {
    return;
  }
  if (context_type_parameters_.contains(type.name)) {
    type_parameters_.try_emplace(KeyAt(type.location), 0U);
  }
  if (type.kind == TypeName::Kind::Named || type.kind == TypeName::Kind::Generic) {
    auto token = TokenAt(type.location);
    const auto& tokens = sources_.at(type.location.source_id).tokens;
    // qualified type 的 location 指向导入前缀，定义属于最后一个标识符。
    while (token + 2 < tokens.size() && tokens.at(token + 1).kind == TokenKind::Dot &&
           tokens.at(token + 2).kind == TokenKind::Identifier) {
      token += 2;
    }
    AddReference(tokens.at(token).location, type.name);
  }
  for (const auto& argument : type.arguments) {
    VisitType(argument);
  }
}

void SemanticIndex::DeclareLocal(const void* node,
                                 SourceLocation location,
                                 std::string_view name,
                                 std::string detail,
                                 Scope scope,
                                 std::size_t available) {
  if (phase_ == IndexPhase::SourceTypes) {
    return;
  }
  const auto symbol = AddSymbol(location, {
                                              .label = std::string(name),
                                              .detail = std::move(detail),
                                              .kind = CompletionKind::Variable,
                                              .documentation = {},
                                          });
  locals_.push_back({.symbol = symbol, .scope = scope, .available = available});
  if (const auto binding = model_.bindings.find(node); binding != model_.bindings.end()) {
    bindings_.insert_or_assign(binding->second, symbol);
  }
}

void SemanticIndex::VisitFunction(const frontend::Function& function,
                                  const Scope& scope,
                                  const frontend::FunctionDescriptor* signature) {
  auto previous_parameters = context_type_parameters_;
  MarkTypeParameters(function.location, function.type_parameters);
  VisitType(function.return_type);
  auto body = function.body ? Extent(*function.body) : scope;
  body.depth = scope.depth + 1;
  for (const auto& parameter : function.parameters) {
    VisitType(parameter.type);
    if (function.body) {
      auto detail = TypeText(parameter.type);
      if (signature != nullptr) {
        const auto parameter_index = signature->parameter_indices.find(parameter.name);
        if (parameter_index != signature->parameter_indices.end()) {
          detail = DisplaySymbolName(signature->parameters.at(parameter_index->second).type.name);
        }
      }
      DeclareLocal(&parameter, parameter.location, parameter.name, std::move(detail), body,
                   body.begin);
      if (phase_ == IndexPhase::Checked) {
        symbols_.at(declarations_.at(KeyAt(parameter.location))).role = Symbol::Role::Parameter;
      }
    }
  }
  VisitStatement(function.body.get(), body);
  context_type_parameters_ = std::move(previous_parameters);
}

void SemanticIndex::VisitStatement(const Statement* statement, Scope scope) {
  if (statement == nullptr) {
    return;
  }
  if (statement->kind == Statement::Kind::Block || statement->kind == Statement::Kind::For) {
    auto nested = Extent(*statement);
    nested.depth = scope.depth + 1;
    scope = nested;
  }
  VisitType(statement->type);
  VisitStatement(statement->initializer.get(), scope);
  VisitExpression(statement->expression.get(), scope);
  if (statement->kind == Statement::Kind::VariableDeclaration) {
    const auto type = model_.local_types.find(statement);
    const auto detail = type == model_.local_types.end() ? TypeText(statement->type)
                                                         : DisplaySymbolName(type->second.name);
    DeclareLocal(statement, statement->location, statement->name, detail, scope,
                 Extent(*statement).end);
    if (phase_ == IndexPhase::Checked && statement->binding == frontend::BindingKind::Constant) {
      symbols_.at(declarations_.at(KeyAt(statement->location))).completion.kind =
          CompletionKind::Constant;
    }
  }
  VisitExpression(statement->condition.get(), scope);
  VisitExpression(statement->increment.get(), scope);
  for (const auto* branch : {
           statement->then_branch.get(),
           statement->else_branch.get(),
           statement->finally_branch.get(),
       }) {
    if (branch != nullptr) {
      auto nested = Extent(*branch);
      nested.depth = scope.depth + 1;
      VisitStatement(branch, nested);
    }
  }
  for (const auto& child : statement->statements) {
    VisitStatement(child.get(), scope);
  }
  for (const auto& branch : statement->cases) {
    VisitExpression(branch.label.get(), scope);
    VisitStatement(branch.body.get(), scope);
  }
  for (const auto& clause : statement->catches) {
    auto nested = Extent(*clause.body);
    nested.depth = scope.depth + 1;
    VisitType(clause.type);
    if (!clause.name.empty()) {
      DeclareLocal(&clause, clause.location, clause.name, TypeText(clause.type), nested,
                   nested.begin);
    }
    VisitStatement(clause.body.get(), nested);
  }
}

SemanticIndex::Receiver SemanticIndex::ReceiverFor(const Expression& expression) const {
  Receiver receiver;
  receiver.context_class = context_class_;
  const auto type = model_.expression_types.find(&expression);
  if (type != model_.expression_types.end()) {
    receiver.type = type->second;
  } else if (expression.kind == Expression::Kind::Name && !model_.bindings.contains(&expression)) {
    if (expression.text == "Future") {
      receiver.type = {.kind = frontend::TypeDescriptor::Kind::Future, .name = "Future"};
      receiver.storage = frontend::MemberStorage::Static;
      return receiver;
    }
    if (const auto named = model_.types.find(expression.text); named != model_.types.end()) {
      receiver.type = named->second;
      receiver.storage = frontend::MemberStorage::Static;
    }
  }
  return receiver;
}

void SemanticIndex::IndexExpression(const Expression& expression) {
  if (phase_ == IndexPhase::SourceTypes) {
    return;
  }
  if (expression.kind == Expression::Kind::Name) {
    const auto binding = model_.bindings.find(&expression);
    if (binding != model_.bindings.end()) {
      if (const auto symbol = bindings_.find(binding->second); symbol != bindings_.end()) {
        references_.insert_or_assign(KeyAt(expression.location), symbol->second);
      }
    } else {
      AddReference(expression.location, expression.text);
    }
  } else if (expression.kind == Expression::Kind::Member) {
    auto receiver = ReceiverFor(*expression.object);
    const auto key = KeyAt(expression.location);
    const auto found = members_.find(receiver.type.name);
    if (found != members_.end()) {
      const auto member = found->second.find(expression.text);
      if (member != found->second.end()) {
        references_.insert_or_assign(key, member->second);
      }
    }
    receivers_.insert_or_assign(key, std::move(receiver));
  }
}

void SemanticIndex::VisitExpression(const Expression* expression, const Scope& scope) {
  if (expression == nullptr) {
    return;
  }
  IndexExpression(*expression);
  VisitType(expression->type);
  for (const auto& argument : expression->type_arguments) {
    VisitType(argument);
  }
  for (const auto* child : {
           expression->left.get(),
           expression->right.get(),
           expression->operand.get(),
           expression->callee.get(),
           expression->object.get(),
           expression->index.get(),
       }) {
    VisitExpression(child, scope);
  }
  for (const auto& element : expression->elements) {
    VisitExpression(element.get(), scope);
  }
  const auto target = expression->callee ? references_.find(KeyAt(expression->callee->location))
                                         : references_.end();
  if (phase_ == IndexPhase::Checked && expression->kind == Expression::Kind::Call) {
    calls_.insert_or_assign(KeyAt(expression->callee->location), expression);
  }
  const auto parameters =
      target != references_.end() ? parameters_.find(target->second) : parameters_.end();
  for (const auto& argument : expression->arguments) {
    if (parameters != parameters_.end() && !argument.name.empty()) {
      const auto parameter = parameters->second.find(argument.name);
      if (parameter != parameters->second.end()) {
        references_.insert_or_assign(KeyAt(argument.location), parameter->second);
      }
    }
    VisitExpression(argument.value.get(), scope);
  }
  for (std::size_t arm_index = 0; arm_index < expression->arms.size(); ++arm_index) {
    const auto& arm = expression->arms.at(arm_index);
    VisitExpression(arm.pattern.get(), scope);
    auto nested = scope;
    nested.begin = KeyAt(arm.location).second;
    const auto& source = sources_.at(arm.location.source_id);
    const auto match_start = TokenAt(expression->location);
    const auto match_body = source.closing.at(match_start + 1) + 1;
    nested.end = arm_index + 1 < expression->arms.size()
                     ? KeyAt(expression->arms.at(arm_index + 1).location).second
                     : source.tokens.at(source.closing.at(match_body)).offset;
    ++nested.depth;
    if (arm.kind == frontend::MatchArm::Kind::Binding) {
      DeclareLocal(&arm, arm.location, arm.binding, "match binding", nested, nested.begin);
    }
    VisitExpression(arm.value.get(), nested);
  }
  if (expression->function) {
    const auto lambda = model_.lambda_indices.find(expression);
    const auto* signature = lambda == model_.lambda_indices.end()
                                ? nullptr
                                : &model_.lambdas.at(lambda->second).signature;
    VisitFunction(*expression->function, scope, signature);
  }
}

void SemanticIndex::VisitProgram() {
  const Scope root{.end = sources_.front().text.text().size()};
  for (const auto& function : program_.functions) {
    VisitFunction(function, root);
  }
  for (const auto& declaration : program_.classes) {
    context_class_ = declaration.name;
    MarkTypeParameters(declaration.location, declaration.type_parameters);
    for (const auto& type : declaration.interfaces) {
      VisitType(type);
    }
    for (const auto& field : declaration.fields) {
      VisitType(field.type);
      VisitExpression(field.initializer.get(), root);
    }
    for (const auto& method : declaration.methods) {
      VisitFunction(method, root);
    }
    if (declaration.constructor) {
      VisitFunction(*declaration.constructor, root);
    }
    context_class_.clear();
    context_type_parameters_.clear();
  }
  for (const auto& declaration : program_.interfaces) {
    for (const auto& method : declaration.methods) {
      VisitFunction(method, root);
    }
  }
  for (const auto& alias : program_.aliases) {
    MarkTypeParameters(alias.location, alias.type_parameters);
    VisitType(alias.target);
    context_type_parameters_.clear();
  }
  for (const auto& global : program_.globals) {
    VisitType(global.type);
    VisitExpression(global.initializer.get(), root);
  }
}

std::optional<std::uint32_t> SemanticIndex::ImportSource(
    const frontend::Program::Import& imported) const {
  auto path = PathText(
      frontend::ResolveImportPath(Utf8Path(sources_.front().path), Utf8Path(imported.path)));
  if (const auto resolution = resolutions_.find(path); resolution != resolutions_.end()) {
    path = resolution->second;
  }
  if (const auto source = source_ids_.find(path); source != source_ids_.end()) {
    return source->second;
  }
  return std::nullopt;
}

void SemanticIndex::CollectImports() {
  exports_.resize(sources_.size());
  for (const auto& [name, symbol] : names_) {
    const auto& declaration = symbols_.at(symbol);
    if (declaration.key.first == 0 ||
        sources_.at(declaration.key.first).path.starts_with(frontend::kStandardSourcePrefix)) {
      visible_.push_back(symbol);
    }
    if (declaration.export_kind == frontend::ExportKind::Exported) {
      exports_.at(declaration.key.first).push_back(symbol);
    }
  }
  for (const auto& imported : sources_.front().imports) {
    const auto source = ImportSource(imported);
    if (!source) {
      continue;
    }
    const auto path_token = TokenAt(imported.location);
    const auto& tokens = sources_.front().tokens;
    import_targets_.insert_or_assign(tokens.at(path_token).offset, *source);
    if (!imported.alias.empty()) {
      import_targets_.insert_or_assign(tokens.at(path_token + 2).offset, *source);
      prefixes_.insert_or_assign(imported.alias, *source);
    } else {
      const auto& exported = exports_.at(*source);
      visible_.insert(visible_.end(), exported.begin(), exported.end());
    }
  }
}

std::vector<ImportLink> SemanticIndex::ImportLinks(PositionEncoding encoding) const {
  std::vector<ImportLink> result;
  if (sources_.empty()) {
    return result;
  }
  const auto& source = sources_.front();
  result.reserve(source.imports.size());
  for (const auto& imported : source.imports) {
    const auto& token = source.tokens.at(TokenAt(imported.location));
    const auto target = import_targets_.find(token.offset);
    if (target == import_targets_.end()) {
      continue;
    }
    result.push_back({
        .path = sources_.at(target->second).path,
        .range =
            {
                .start = source.text.PositionAt(token.offset + 1, encoding),
                .end = source.text.PositionAt(token.offset + token.length - 1, encoding),
            },
    });
  }
  return result;
}

const frontend::Token* SemanticIndex::QueryToken(std::size_t offset) const {
  if (sources_.empty()) {
    return nullptr;
  }
  const auto& tokens = sources_.front().tokens;
  const auto after = std::ranges::upper_bound(tokens, offset, {}, &frontend::Token::offset);
  if (after == tokens.begin()) {
    return nullptr;
  }
  auto selected = std::prev(after);
  // 编辑器的光标可停在单词末尾，此时当前位置可能已经属于紧接着的 '(' 或 '.'。
  if (selected->offset == offset && selected->kind != TokenKind::Identifier &&
      selected->kind != TokenKind::Print && selected != tokens.begin()) {
    const auto previous = std::prev(selected);
    if ((previous->kind == TokenKind::Identifier || previous->kind == TokenKind::Print) &&
        previous->offset + previous->length == offset) {
      selected = previous;
    }
  }
  const auto& token = *selected;
  if (offset > token.offset + token.length) {
    return nullptr;
  }
  return &token;
}

std::optional<Definition> SemanticIndex::FindDefinition(std::size_t offset,
                                                        PositionEncoding encoding) const {
  const auto* selected = QueryToken(offset);
  if (selected == nullptr) {
    return std::nullopt;
  }
  const auto& token = *selected;
  if (const auto imported = import_targets_.find(token.offset); imported != import_targets_.end()) {
    const auto& source = sources_.at(imported->second);
    if (source.path.starts_with(frontend::kStandardSourcePrefix)) {
      return std::nullopt;
    }
    return Definition{.path = source.path, .range = {}};
  }
  const auto found = references_.find(Key{0, token.offset});
  if (found == references_.end()) {
    return std::nullopt;
  }
  const auto& symbol = symbols_.at(found->second);
  const auto& source = sources_.at(symbol.key.first);
  if (source.path.starts_with(frontend::kStandardSourcePrefix)) {
    return std::nullopt;
  }
  return Definition{
      .path = source.path,
      .range =
          {
              .start = source.text.PositionAt(symbol.key.second, encoding),
              .end = source.text.PositionAt(symbol.key.second + symbol.length, encoding),
          },
  };
}

std::string SemanticIndex::DocumentationAt(SourceLocation location) const {
  const auto& source = sources_.at(location.source_id);
  const auto& comments = source.comments;
  const auto text = source.text.text();
  auto line = static_cast<std::size_t>(location.line - 1);
  auto previous =
      std::ranges::lower_bound(comments, location.line, {},
                               [](const auto& comment) -> auto { return comment.location.line; });
  std::vector<std::string_view> lines;
  while (previous != comments.begin() && line > 0) {
    const auto& comment = *--previous;
    auto content = text.substr(comment.offset, comment.length);
    if (comment.location.line != line || !content.starts_with("///")) {
      break;
    }
    const auto start = source.text.OffsetAt({.line = line - 1}, PositionEncoding::Utf8).value();
    if (text.substr(start, comment.offset - start).find_first_not_of(" \t") !=
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

std::string SemanticIndex::DefaultValue(const Expression& expression) const {
  const auto& source = sources_.at(expression.location.source_id);
  const auto& token = source.tokens.at(TokenAt(expression.location));
  switch (expression.kind) {
    case Expression::Kind::IntegerLiteral:
      return expression.text;
    case Expression::Kind::DoubleLiteral:
    case Expression::Kind::BooleanLiteral:
    case Expression::Kind::StringLiteral:
    case Expression::Kind::NullLiteral:
      return std::string(source.text.text().substr(token.offset, token.length));
    case Expression::Kind::Unary:
      return token.lexeme + DefaultValue(*expression.operand);
    default:
      return "…";
  }
}

std::string SemanticIndex::ClassDetail(const frontend::Class& declaration) const {
  auto text = "class " + GenericName(declaration.name, declaration.type_parameters) + " {\n  ";
  if (declaration.constructor &&
      declaration.constructor->visibility == frontend::Visibility::Private) {
    text += "private ";
  }
  text += "constructor(";
  if (declaration.constructor_kind == frontend::ConstructorKind::Explicit) {
    for (const auto& parameter : declaration.constructor->parameters) {
      text += (text.back() == '(' ? "" : ", ") + parameter.name + ": " + TypeText(parameter.type);
    }
  } else {
    for (const auto& field : declaration.fields) {
      if (field.role != frontend::FieldRole::Parameter) {
        continue;
      }
      text += (text.back() == '(' ? "" : ", ") + field.name + ": " + TypeText(field.type);
      if (field.initializer) {
        text += " = " + DefaultValue(*field.initializer);
      }
    }
  }
  return text + ");\n}";
}

void SemanticIndex::MarkTypeParameters(SourceLocation location,
                                       std::span<const std::string> parameters) {
  if (parameters.empty()) {
    return;
  }
  context_type_parameters_.insert(parameters.begin(), parameters.end());
  const auto& tokens = sources_.at(location.source_id).tokens;
  auto index = TokenAt(location) + 2;
  for (std::size_t parameter = 0; parameter < parameters.size(); ++parameter) {
    const auto& token = tokens.at(index);
    type_parameters_.insert_or_assign(Key{location.source_id, token.offset}, kSemanticDeclaration);
    index += 2;
  }
}

std::vector<Completion> SemanticIndex::CompleteArguments(
    const ArgumentCompletionQuery& query) const {
  std::vector<Completion> result;
  const auto signature = FindCallParameters(query.callee_offset);
  if (!signature) {
    return result;
  }
  const auto& parameters = signature->parameters;
  for (std::size_t index = query.positional_arguments; index < parameters.size(); ++index) {
    const auto& parameter = parameters.at(index);
    if (parameter.rest || query.supplied_names.contains(parameter.name) ||
        !parameter.name.starts_with(query.prefix)) {
      continue;
    }
    auto detail = parameter.name + ": " + DisplaySymbolName(parameter.type.name);
    if (signature->optional_parameters.contains(parameter.name)) {
      detail += " = …";
    }
    result.push_back({
        .label = parameter.name + ':',
        .detail = std::move(detail),
        .kind = CompletionKind::Field,
        .insert_text = parameter.name + (query.has_colon ? "" : ": "),
        .documentation = {},
    });
  }
  return result;
}

}  // namespace vela::tooling
