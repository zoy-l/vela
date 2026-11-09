#include "compiler/tooling/semantic/semantic_index.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "base/source_location.h"
#include "compiler/frontend/builtins/sdk_modules.h"
#include "compiler/frontend/builtins/standard_environment.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/syntax/parser.h"
#include "compiler/frontend/syntax/token.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/frontend/typing/type_checker.h"
#include "compiler/frontend/typing/type_names.h"
#include "compiler/tooling/documentation/documentation.h"
#include "compiler/tooling/semantic/semantic_syntax.h"
#include "compiler/tooling/source_text.h"

namespace vela::tooling {

std::optional<frontend::TokenKind> ClosingDelimiter(frontend::TokenKind kind) {
  using frontend::TokenKind;
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

void TrackDelimiter(std::vector<frontend::TokenKind>& closing,
                    frontend::TokenKind kind,
                    std::optional<frontend::TokenKind> expected) {
  if (expected) {
    closing.push_back(*expected);
  } else if (!closing.empty() && kind == closing.back()) {
    closing.pop_back();
  }
}

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

void AppendClosingDelimiters(std::string& text,
                             std::span<const frontend::TokenKind> closing,
                             CompletionBrace brace) {
  for (const auto kind : std::views::reverse(closing)) {
    switch (kind) {
      case frontend::TokenKind::RightBrace:
        text += brace == CompletionBrace::TerminateStatement ? ";}" : "}";
        break;
      case frontend::TokenKind::RightBracket:
        text += ']';
        break;
      case frontend::TokenKind::RightParen:
        text += ')';
        break;
      default:
        std::unreachable();
    }
  }
}

std::optional<std::size_t> TokenIndexAt(std::span<const frontend::Token> tokens,
                                        std::size_t offset) {
  const auto after = std::ranges::upper_bound(tokens, offset, {}, &frontend::Token::offset);
  if (after == tokens.begin()) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(after - tokens.begin() - 1);
}

namespace {

using frontend::Expression;
using frontend::Statement;
using frontend::TokenKind;
using frontend::TypeName;

void CloseCompletionDelimiters(std::string& text, std::span<const frontend::Token> tokens) {
  std::vector<TokenKind> closing;
  for (const auto& token : tokens) {
    TrackDelimiter(closing, token.kind, ClosingDelimiter(token.kind));
  }
  AppendClosingDelimiters(text, closing, CompletionBrace::Close);
}

}  // namespace

bool IsInComment(std::span<const frontend::SourceComment> comments, std::size_t offset) {
  return std::ranges::any_of(comments, [offset](const auto& comment) {
    return offset >= comment.offset && offset <= comment.offset + comment.length;
  });
}

bool IsCompletionExcluded(const frontend::SourceSnapshot& snapshot, std::size_t offset) {
  if (IsInComment(snapshot.comments, offset)) {
    return true;
  }
  const auto& tokens = snapshot.tokens;
  const auto index = TokenIndexAt(tokens, offset);
  if (!index) {
    return false;
  }
  const auto& token = tokens[*index];
  return offset > token.offset && offset < token.offset + token.length &&
         frontend::IsStringToken(token.kind);
}

std::string CompletionSource(const frontend::SourceSnapshot& snapshot, std::size_t offset) {
  const auto source = std::string_view(snapshot.text);
  // 注释与字面量中不制造表达式；插值内部的表达式仍由生产 lexer 分别标记。
  if (IsCompletionExcluded(snapshot, offset)) {
    return std::string(source);
  }
  std::size_t end = offset;
  while (end < source.size() && frontend::IsIdentifierPart(source[end])) {
    ++end;
  }
  std::string text(source);
  // 保留已经输入的前缀，光标位置仍落在同一个 token 内。
  text.insert(end, "__vela_completion__");
  CloseCompletionDelimiters(text, snapshot.tokens);
  return text;
}

SemanticIndex::SemanticIndex(std::string_view path, std::span<const frontend::SourceInput> overlays)
    : SemanticIndex(frontend::LoadProgram(path, overlays, frontend::SourceLoadMode::Editor)) {}

SemanticIndex::SemanticIndex(std::string_view path,
                             frontend::ModuleCache& modules,
                             std::span<const frontend::SourceInput> overlays)
    : SemanticIndex(
          frontend::LoadProgram(path, modules, overlays, frontend::SourceLoadMode::Editor)) {}

SemanticIndex::SemanticIndex(frontend::ParseResult loaded, frontend::DiagnosticOptions options)
    : program_(std::move(loaded.program)),
      diagnostics_(std::move(loaded.diagnostics)),
      has_load_errors_(frontend::HasErrors(diagnostics_)) {
  for (const auto& resolution : program_.file_resolutions) {
    resolutions_.emplace(resolution.requested_path, resolution.canonical_path);
  }
  sources_.reserve(program_.sources.size());
  for (std::uint32_t index = 0; index < program_.sources.size(); ++index) {
    const auto& file = program_.sources.at(index);
    const auto& syntax = *file.syntax;
    Source source{
        .path = file.path,
        .text = file.text,
        .lines = SourceLineIndex(syntax.lines),
        .tokens = syntax.tokens,
        .comments = syntax.comments,
        .closing = {},
        .imports = syntax.imports,
    };
    source.closing.resize(source.tokens.size());
    std::vector<std::size_t> opening;
    for (std::size_t token_index = 0; token_index < source.tokens.size(); ++token_index) {
      source.closing[token_index] = token_index;
      const auto kind = source.tokens[token_index].kind;
      if (ClosingDelimiter(kind)) {
        opening.push_back(token_index);
      } else if (!opening.empty() && ClosingDelimiter(source.tokens[opening.back()].kind) == kind) {
        source.closing[opening.back()] = token_index;
        opening.pop_back();
      }
    }
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
  auto checked = frontend::TypeChecker(options).Check(program_);
  extents_.clear();
  if (diagnostics_.empty()) {
    diagnostics_ = std::move(checked.diagnostics);
  }
  // 未完成的表达式也需要已注册签名与局部类型；诊断不使部分语义事实失效。
  // 特化初始化失败时尚未创建类型环境，不能进入受检索引阶段。
  if (!checked.model.types.empty()) {
    model_ = std::move(checked.model);
    CollectDeclarations();
    phase_ = IndexPhase::Checked;
    VisitProgram();
  }
  CollectImports();
}

bool SemanticIndex::DependsOn(std::string_view path) const {
  // 加载失败的图可能缺少尚未解析到的依赖，下一次输入变化必须重新尝试。
  return has_load_errors_ || source_ids_.contains(path) || resolutions_.contains(path) ||
         std::ranges::any_of(resolutions_,
                             [path](const auto& entry) { return entry.second == path; });
}

std::size_t SemanticIndex::TokenAt(SourceLocation location) const {
  const auto& source = sources_[location.source_id];
  const auto& tokens = source.tokens;
  const auto position = std::pair(location.line, location.column);
  const auto found = std::ranges::lower_bound(tokens, position, {}, [](const auto& token) {
    const auto token_location = token.location;
    return std::pair(token_location.line, token_location.column);
  });
  return static_cast<std::size_t>(found - tokens.begin());
}

SemanticIndex::Key SemanticIndex::KeyAt(SourceLocation location) const {
  const auto& source = sources_[location.source_id];
  return {location.source_id, source.tokens[TokenAt(location)].offset};
}

SemanticIndex::Scope SemanticIndex::Extent(const Statement& statement) const {
  if (const auto found = extents_.find(&statement); found != extents_.end()) {
    return found->second;
  }
  const auto extent = ComputeExtent(statement);
  extents_.emplace(&statement, extent);
  return extent;
}

SemanticIndex::Scope SemanticIndex::ComputeExtent(const Statement& statement) const {
  const auto source_id = statement.location.source_id;
  const auto& source = sources_[source_id];
  const auto start = TokenAt(statement.location);
  auto end = start;
  if (statement.kind == Statement::Kind::Block &&
      source.tokens[start].kind == TokenKind::LeftBrace) {
    end = source.closing[start];
  } else if (statement.kind == Statement::Kind::If || statement.kind == Statement::Kind::While ||
             statement.kind == Statement::Kind::For || statement.kind == Statement::Kind::ForEach ||
             statement.kind == Statement::Kind::Try) {
    auto extent = Scope{
        .source = source_id,
        .begin = source.tokens[start].offset,
        .end = source.tokens[start].offset,
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
        .begin = source.tokens[start].offset,
        .end = Extent(*statement.statements.back()).end,
    };
  } else {
    for (; end + 1 < source.tokens.size(); ++end) {
      const auto kind = source.tokens[end].kind;
      if (kind == TokenKind::Semicolon || kind == TokenKind::RightBrace) {
        break;
      }
      if (kind == TokenKind::LeftBrace && statement.kind == Statement::Kind::Switch) {
        end = source.closing[end];
        break;
      }
      end = source.closing[end];
    }
  }
  const auto& last = source.tokens[end];
  return {
      .source = source_id,
      .begin = source.tokens[start].offset,
      .end = last.offset + last.length,
  };
}

std::size_t SemanticIndex::AddSymbol(SourceLocation location,
                                     CompletionKind kind,
                                     std::string detail) {
  const auto& source = sources_[location.source_id];
  auto token = TokenAt(location);
  // match 绑定的位置指向 let，catch 指向关键字；普通声明已经指向名称。
  if (source.tokens[token].kind == TokenKind::Let) {
    ++token;
  } else if (source.tokens[token].kind == TokenKind::Catch) {
    token += 2;
  }
  const auto& name = source.tokens[token];
  const Key key{location.source_id, name.offset};
  const auto [found, inserted] = declarations_.try_emplace(key, symbols_.size());
  if (inserted) {
    symbols_.push_back({
        .completion =
            {
                .label = name.lexeme,
                .detail = std::move(detail),
                .kind = kind,
                .documentation = DocumentationAt(location),
            },
        .owner = {},
        .key = key,
        .length = name.length,
    });
    references_.try_emplace(key, Reference{.symbol = found->second});
  }
  return found->second;
}

void SemanticIndex::AddReference(SourceLocation location,
                                 std::string_view name,
                                 const Expression* expression) {
  const auto found = names_.find(name);
  if (found == names_.end()) {
    return;
  }
  references_.try_emplace(KeyAt(location),
                          Reference{.symbol = found->second, .expression = expression});
}

void SemanticIndex::VisitType(const TypeName& type) {
  if (type.kind == TypeName::Kind::Parameter) {
    if (const auto parameter = type_parameter_symbols_.find(type.parameter);
        parameter != type_parameter_symbols_.end()) {
      references_.insert_or_assign(KeyAt(type.location), Reference{.symbol = parameter->second});
    }
  }
  if (type.kind == TypeName::Kind::Named || type.kind == TypeName::Kind::Generic) {
    auto token = TokenAt(type.location);
    const auto& tokens = sources_[type.location.source_id].tokens;
    // qualified type 的 location 指向导入前缀，定义属于最后一个标识符。
    while (token + 2 < tokens.size() && tokens[token + 1].kind == TokenKind::Dot &&
           tokens[token + 2].kind == TokenKind::Identifier) {
      token += 2;
    }
    auto location = tokens[token].location;
    location.source_id = type.location.source_id;
    AddReference(location, type.name);
  }
  for (const auto& argument : type.arguments) {
    VisitType(argument);
  }
}

std::optional<std::size_t> SemanticIndex::DeclareLocal(const void* node,
                                                       SourceLocation location,
                                                       std::string detail,
                                                       Scope scope,
                                                       std::size_t available) {
  if (phase_ == IndexPhase::SourceTypes) {
    return std::nullopt;
  }
  const auto symbol = AddSymbol(location, CompletionKind::Variable, detail);
  if (phase_ == IndexPhase::Definitions) {
    symbols_[symbol].completion.detail = std::move(detail);
  }
  if (scope.source == 0) {
    locals_.push_back({.symbol = symbol, .scope = scope, .available = available});
  }
  const auto& facts = Facts();
  if (const auto binding = facts.bindings.find(node); binding != facts.bindings.end()) {
    bindings_.insert_or_assign(binding->second, symbol);
  }
  return symbol;
}

void SemanticIndex::VisitFunction(const frontend::Function& function,
                                  const Scope& scope,
                                  const frontend::FunctionDescriptor* signature) {
  MarkTypeParameters(function.type_parameters);
  VisitType(function.return_type);
  auto body = function.body ? Extent(*function.body) : scope;
  body.depth = scope.depth + 1;
  for (const auto& parameter : function.parameters) {
    VisitType(parameter.type);
    if (function.body) {
      auto detail = DisplaySymbolName(parameter.type.ToString());
      if (signature != nullptr) {
        const auto parameter_index = signature->parameter_indices.find(parameter.name);
        if (parameter_index != signature->parameter_indices.end()) {
          detail = DisplayType(signature->parameters.at(parameter_index->second).type);
        }
      }
      if (const auto symbol =
              DeclareLocal(&parameter, parameter.location, std::move(detail), body, body.begin)) {
        symbols_[*symbol].role = Symbol::Role::Parameter;
      }
    }
  }
  VisitStatement(function.body.get(), body);
}

void SemanticIndex::VisitStatement(const Statement* statement, Scope scope) {
  if (statement == nullptr) {
    return;
  }
  if (statement->kind == Statement::Kind::ForEach) {
    VisitForEach(*statement, scope);
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
    const auto& types = Facts().local_types;
    const auto type = types.find(statement);
    const auto detail = type == types.end() ? DisplaySymbolName(statement->type.ToString())
                                            : DisplayType(type->second);
    if (const auto symbol =
            DeclareLocal(statement, statement->location, detail, scope, Extent(*statement).end);
        symbol && statement->binding == frontend::BindingKind::Constant) {
      symbols_[*symbol].completion.kind = CompletionKind::Constant;
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
      DeclareLocal(&clause, clause.location, DisplaySymbolName(clause.type.ToString()), nested,
                   nested.begin);
    }
    VisitStatement(clause.body.get(), nested);
  }
}

void SemanticIndex::VisitForEach(const Statement& statement, const Scope& scope) {
  // iterable 在循环绑定之前求值，同名绑定只在循环体内可见。
  VisitExpression(statement.iterable.get(), scope);
  auto body = Extent(*statement.then_branch);
  body.depth = scope.depth + 1;
  std::string binding_type;
  const auto& types = Facts().expression_types;
  const auto iterable = types.find(statement.iterable.get());
  if (iterable != types.end()) {
    const auto& type = iterable->second;
    if (const auto element = model_.array_elements.find(type.name);
        element != model_.array_elements.end()) {
      binding_type = DisplayType(element->second);
    } else if (const auto arguments = model_.generic_arguments.find(type.name);
               arguments != model_.generic_arguments.end() && !arguments->second.empty()) {
      binding_type = DisplayType(arguments->second.front());
    }
  }
  const auto& tokens = sources_[statement.location.source_id].tokens;
  auto location = tokens[TokenAt(statement.location) + 1].location;
  location.source_id = statement.location.source_id;
  if (const auto symbol =
          DeclareLocal(&statement.name, location, std::move(binding_type), body, body.begin);
      symbol && phase_ == IndexPhase::Checked) {
    symbols_[*symbol].completion.kind = CompletionKind::Constant;
  }
  VisitStatement(statement.then_branch.get(), body);
}

SemanticIndex::Receiver SemanticIndex::ReceiverFor(const Expression& expression) const {
  Receiver receiver;
  receiver.context_class = context_class_;
  const auto& facts = FactsFor(expression);
  const auto type = facts.expression_types.find(&expression);
  if (type != facts.expression_types.end()) {
    receiver.type = Capability(type->second);
  } else if (expression.kind == Expression::Kind::Name && !facts.bindings.contains(&expression)) {
    if (expression.text == frontend::kFutureTypeName) {
      receiver.type = {
          .kind = frontend::TypeDescriptor::Kind::Future,
          .name = frontend::kFutureTypeName,
      };
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
    const auto& facts = Facts();
    const auto binding = facts.bindings.find(&expression);
    if (binding != facts.bindings.end()) {
      if (const auto symbol = bindings_.find(binding->second); symbol != bindings_.end()) {
        references_.insert_or_assign(
            KeyAt(expression.location),
            Reference{.symbol = symbol->second, .expression = &expression});
      }
    } else {
      const auto application = facts.generic_applications.find(&expression);
      AddReference(
          expression.location,
          application == facts.generic_applications.end() ? expression.text : application->second,
          &expression);
    }
  } else if (expression.kind == Expression::Kind::Member) {
    IndexMemberExpression(expression);
  }
}

void SemanticIndex::IndexMemberExpression(const Expression& expression) {
  auto receiver = ReceiverFor(*expression.object);
  if (expression.null_safe && receiver.type.nullable) {
    receiver.type.name = frontend::NonNullableTypeName(receiver.type);
    receiver.type.nullable = false;
  }
  const auto key = KeyAt(expression.location);
  for (const auto owner : MemberOwners(receiver.type)) {
    const auto found = members_.find(owner);
    if (found != members_.end()) {
      const auto member = found->second.find(expression.text);
      if (member != found->second.end()) {
        references_.insert_or_assign(
            key, Reference{.symbol = member->second, .expression = &expression});
        break;
      }
    }
  }
  if (expression.location.source_id == 0) {
    receivers_.insert_or_assign(key, std::move(receiver));
  }
}

void SemanticIndex::VisitCallArguments(const Expression& expression, const Scope& scope) {
  const auto callee_key = expression.callee ? KeyAt(expression.callee->location) : Key{};
  const auto target = expression.callee ? references_.find(callee_key) : references_.end();
  if (phase_ != IndexPhase::SourceTypes && expression.kind == Expression::Kind::Call &&
      expression.location.source_id == 0) {
    calls_.insert_or_assign(callee_key, &expression);
  }
  const auto parameters =
      target != references_.end() ? parameters_.find(target->second.symbol) : parameters_.end();
  for (const auto& argument : expression.arguments) {
    if (parameters != parameters_.end() && !argument.name.empty()) {
      const auto parameter = parameters->second.find(argument.name);
      if (parameter != parameters->second.end()) {
        references_.insert_or_assign(KeyAt(argument.location),
                                     Reference{.symbol = parameter->second});
      }
    }
    VisitExpression(argument.value.get(), scope);
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
  for (const auto* child : ExpressionOperands(*expression)) {
    VisitExpression(child, scope);
  }
  for (const auto& element : expression->elements) {
    VisitExpression(element.get(), scope);
  }
  for (const auto& entry : expression->entries) {
    VisitExpression(entry.key.get(), scope);
    VisitExpression(entry.value.get(), scope);
  }
  VisitCallArguments(*expression, scope);
  std::size_t match_end = 0;
  if (!expression->arms.empty()) {
    const auto& source = sources_[expression->location.source_id];
    const auto match_start = TokenAt(expression->location);
    const auto match_body = source.closing[match_start + 1] + 1;
    match_end = source.tokens[source.closing[match_body]].offset;
  }
  for (std::size_t arm_index = 0; arm_index < expression->arms.size(); ++arm_index) {
    const auto& arm = expression->arms.at(arm_index);
    VisitExpression(arm.pattern.get(), scope);
    auto nested = scope;
    nested.begin = KeyAt(arm.location).second;
    nested.end = arm_index + 1 < expression->arms.size()
                     ? KeyAt(expression->arms.at(arm_index + 1).location).second
                     : match_end;
    ++nested.depth;
    if (arm.kind == frontend::MatchArm::Kind::Binding) {
      DeclareLocal(&arm, arm.location, "match binding", nested, nested.begin);
    }
    VisitExpression(arm.value.get(), nested);
  }
  if (expression->function) {
    const auto& facts = Facts();
    const auto lambda = facts.lambda_indices.find(expression);
    const auto* signature = lambda == facts.lambda_indices.end()
                                ? nullptr
                                : &facts.lambdas.at(lambda->second).signature;
    VisitFunction(*expression->function, scope, signature);
  }
}

void SemanticIndex::VisitProgram() {
  const Scope root{.end = sources_.front().text.size()};
  for (const auto& function : program_.functions) {
    VisitFunction(function, root);
  }
  for (const auto& declaration : program_.classes) {
    VisitClass(declaration, root);
  }
  for (const auto& declaration : program_.interfaces) {
    VisitInterface(declaration, root);
  }
  for (const auto& alias : program_.aliases) {
    MarkTypeParameters(alias.type_parameters);
    VisitType(alias.target);
  }
  for (const auto& global : program_.globals) {
    VisitType(global.type);
    VisitExpression(global.initializer.get(), root);
  }
  if (phase_ != IndexPhase::SourceTypes) {
    VisitDefinitions(root);
  }
}

void SemanticIndex::VisitClass(const frontend::Class& declaration, const Scope& root) {
  context_class_ = declaration.name;
  MarkTypeParameters(declaration.type_parameters);
  if (declaration.base_class) {
    VisitType(*declaration.base_class);
  }
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
  const auto previous = phase_;
  if (phase_ != IndexPhase::SourceTypes) {
    phase_ = IndexPhase::Definitions;
  }
  for (const auto& method : declaration.method_templates) {
    VisitFunction(method, root);
  }
  phase_ = previous;
  if (declaration.constructor) {
    VisitFunction(*declaration.constructor, root);
  }
  context_class_.clear();
}

void SemanticIndex::VisitInterface(const frontend::Interface& declaration, const Scope& root) {
  MarkTypeParameters(declaration.type_parameters);
  for (const auto& method : declaration.methods) {
    VisitFunction(method, root);
  }
  for (const auto& method : declaration.method_templates) {
    VisitFunction(method, root);
  }
}

std::optional<std::uint32_t> SemanticIndex::ImportSource(
    const frontend::Program::Import& imported) const {
  if (program_.import_sources.empty()) {
    return std::nullopt;
  }
  const auto& targets = program_.import_sources.front();
  const auto index = static_cast<std::size_t>(&imported - sources_.front().imports.data());
  return index < targets.size() ? std::optional(targets[index]) : std::nullopt;
}

void SemanticIndex::CollectImports() {
  exports_.resize(sources_.size());
  for (const auto& [name, symbol] : names_) {
    const auto& declaration = symbols_[symbol];
    if (declaration.key.first == 0 ||
        sources_[declaration.key.first].path.starts_with(frontend::kStandardSourcePrefix)) {
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
    import_targets_.insert_or_assign(tokens[path_token].offset, *source);
    if (!imported.alias.empty()) {
      import_targets_.insert_or_assign(tokens[path_token + 2].offset, *source);
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
    const auto& token = source.tokens[TokenAt(imported.location)];
    const auto target = import_targets_.find(token.offset);
    if (target == import_targets_.end()) {
      continue;
    }
    result.push_back({
        .path = sources_[target->second].path,
        .range = source.lines.RangeAt(source.text, token.offset + 1, token.length - 2, encoding),
    });
  }
  return result;
}

const frontend::Token* SemanticIndex::QueryToken(std::size_t offset) const {
  if (sources_.empty()) {
    return nullptr;
  }
  const auto& tokens = sources_.front().tokens;
  const auto index = TokenIndexAt(tokens, offset);
  if (!index) {
    return nullptr;
  }
  auto selected = tokens.begin() + static_cast<std::ptrdiff_t>(*index);
  // 编辑器的光标可停在单词末尾，此时当前位置可能已经属于紧接着的 '(' 或 '.'。
  if (selected->offset == offset && !frontend::IsMemberName(selected->kind) &&
      selected->kind != TokenKind::Print && selected != tokens.begin()) {
    const auto previous = std::prev(selected);
    if ((frontend::IsMemberName(previous->kind) || previous->kind == TokenKind::Print) &&
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
    const auto& source = sources_[imported->second];
    if (source.path.starts_with(frontend::kStandardSourcePrefix)) {
      return std::nullopt;
    }
    return Definition{.path = source.path, .range = {}};
  }
  const auto found = references_.find(Key{0, token.offset});
  if (found == references_.end()) {
    return std::nullopt;
  }
  const auto& symbol = symbols_[found->second.symbol];
  const auto& source = sources_[symbol.key.first];
  if (source.path.starts_with(frontend::kStandardSourcePrefix)) {
    return std::nullopt;
  }
  return Definition{
      .path = source.path,
      .range = source.lines.RangeAt(source.text, symbol.key.second, symbol.length, encoding),
  };
}

std::string SemanticIndex::DocumentationAt(SourceLocation location) const {
  const auto& source = sources_[location.source_id];
  return DocumentationComment(source.text, source.comments, location);
}

std::vector<Completion> SemanticIndex::CompleteArguments(
    const ArgumentCompletionQuery& query) const {
  std::vector<Completion> result;
  const auto* const signature = FindCallParameters(query.callee_offset);
  if (signature == nullptr) {
    return result;
  }
  const auto& parameters = signature->parameters;
  for (std::size_t index = query.positional_arguments; index < parameters.size(); ++index) {
    const auto& parameter = parameters.at(index);
    if (parameter.rest || query.supplied_names.contains(parameter.name) ||
        !parameter.name.starts_with(query.prefix)) {
      continue;
    }
    auto detail = parameter.name + ": " + DisplayType(parameter.type);
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
