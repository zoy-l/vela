#include "compiler/repl/repl_source.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "base/source_lines.h"
#include "base/source_location.h"
#include "compiler/frontend/builtins/standard_environment.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/syntax/ast_clone.h"
#include "compiler/frontend/syntax/lexer.h"
#include "compiler/frontend/syntax/parser.h"
#include "compiler/frontend/syntax/token.h"

namespace vela::compiler {
namespace {

bool HasOpenDelimiter(const frontend::LexResult& lexed) {
  using Kind = frontend::TokenKind;
  std::int64_t braces = 0;
  std::int64_t parentheses = 0;
  std::int64_t brackets = 0;
  for (const auto& token : lexed.tokens) {
    switch (token.kind) {
      case Kind::LeftBrace:
        ++braces;
        break;
      case Kind::RightBrace:
        --braces;
        break;
      case Kind::LeftParen:
        ++parentheses;
        break;
      case Kind::RightParen:
        --parentheses;
        break;
      case Kind::LeftBracket:
        ++brackets;
        break;
      case Kind::RightBracket:
        --brackets;
        break;
      default:
        break;
    }
    if (braces < 0 || parentheses < 0 || brackets < 0) {
      return false;
    }
  }
  return braces > 0 || parentheses > 0 || brackets > 0;
}

std::string SubmissionPath(std::uint32_t submission) {
  return "<repl:" + std::to_string(submission) + ">";
}

ReplSourceError SubmissionFailure(SubmissionStatus status, frontend::Diagnostics diagnostics = {}) {
  ReplSourceError result;
  result.status = status;
  result.diagnostics = std::move(diagnostics);
  return result;
}

struct InputBody final {
  std::string text;
  bool declaration = false;
  bool terminated = false;
};

std::span<const frontend::Token> DeclarationHead(std::span<const frontend::Token> tokens) {
  using Kind = frontend::TokenKind;
  while (tokens.front().kind == Kind::LeftBracket) {
    std::size_t depth = 1;
    std::size_t end = 1;
    while (depth != 0 && end + 1 < tokens.size()) {
      const auto kind = tokens[end++].kind;
      if (kind == Kind::LeftBracket) {
        ++depth;
      } else if (kind == Kind::RightBracket) {
        --depth;
      }
    }
    if (depth != 0) {
      break;
    }
    tokens = tokens.subspan(end);
  }
  return tokens;
}

bool IsBlockDeclaration(std::span<const frontend::Token> tokens) {
  using Kind = frontend::TokenKind;
  while (tokens.front().kind == Kind::Export || tokens.front().kind == Kind::Async) {
    tokens = tokens.subspan(1);
  }
  if (tokens.front().kind == Kind::External) {
    return tokens[1].kind == Kind::Class;
  }
  const auto kind = tokens.front().kind;
  return kind == Kind::Func || kind == Kind::Class || kind == Kind::Interface || kind == Kind::Enum;
}

InputBody NormalizeBody(std::string_view source, const frontend::LexResult& lexed) {
  using Token = frontend::TokenKind;
  const auto first_kind = lexed.tokens.front().kind;
  const auto declaration = DeclarationHead(lexed.tokens);
  const auto last_kind =
      lexed.tokens.size() > 1 ? lexed.tokens.at(lexed.tokens.size() - 2).kind : Token::EndOfFile;
  InputBody body{
      .text = std::string(source),
      .declaration = frontend::IsDeclarationStart(declaration.front().kind),
      .terminated = last_kind == Token::Semicolon,
  };
  const bool block_statement = first_kind == Token::LeftBrace || first_kind == Token::If ||
                               first_kind == Token::While || first_kind == Token::For ||
                               first_kind == Token::Switch || first_kind == Token::Try;
  const bool needs_semicolon = !body.terminated && !IsBlockDeclaration(declaration) &&
                               (last_kind != Token::RightBrace || !block_statement);
  if (needs_semicolon && lexed.tokens.size() > 1) {
    const auto& last = lexed.tokens.at(lexed.tokens.size() - 2);
    body.text.insert(last.offset + last.length, 1, ';');
  }
  return body;
}

}  // namespace

std::string ReplEntry::name() const {
  return "$repl" + std::to_string(submission);
}

ReplEntry ReplSource::Locate(std::uint32_t line, const ReplEntry& current) const {
  if (line >= current.first_line || entries_.empty()) {
    return current;
  }
  const auto found = std::ranges::upper_bound(entries_, line, {}, &ReplEntry::first_line);
  return found == entries_.begin() ? entries_.front() : *std::prev(found);
}

void ReplSource::Remap(frontend::Diagnostics& diagnostics, const ReplEntry& current) const {
  for (auto& diagnostic : diagnostics) {
    if (diagnostic.location.source_id != 0) {
      continue;
    }
    Remap(diagnostic.location, diagnostic.source_path, current);
  }
}

void ReplSource::Remap(SourceLocation& location,
                       std::string& path,
                       const ReplEntry& current) const {
  const auto entry = Locate(location.line, current);
  path = SubmissionPath(entry.submission);
  location.line = location.line >= entry.first_line ? location.line - entry.first_line + 1 : 1;
}

std::expected<ReplSourceCandidate, ReplSourceError> ReplSource::Prepare(
    std::string_view source) const {
  const auto lexed = frontend::Lexer(source).Tokenize();
  if (lexed.diagnostics.empty() && lexed.tokens.size() == 1) {
    return std::unexpected(ReplSourceError());
  }
  if (HasOpenDelimiter(lexed)) {
    return std::unexpected(SubmissionFailure(SubmissionStatus::Incomplete));
  }
  const auto body = NormalizeBody(source, lexed);
  std::string addition;
  ReplEntry current{
      .header_line = next_line_,
      .first_line = next_line_,
      .last_line = 0,
      .submission = static_cast<std::uint32_t>(entries_.size() + 1),
      .wrapper = !body.declaration,
  };
  if (body.declaration) {
    addition = body.text;
    addition += "\nfunc replEntry(): void {\n}\n";
    current.header_line += static_cast<std::uint32_t>(std::ranges::count(source, '\n')) + 1;
  } else {
    addition = "func replEntry(): void {\n";
    addition += body.text;
    addition += "\n}\n";
    ++current.first_line;
  }
  current.last_line = next_line_ + static_cast<std::uint32_t>(std::ranges::count(addition, '\n'));
  auto addition_tokens = frontend::Lexer(addition).Tokenize();
  for (auto& token : addition_tokens.tokens) {
    token.location.line += next_line_ - 1;
  }
  for (auto& diagnostic : addition_tokens.diagnostics) {
    diagnostic.location.line += next_line_ - 1;
  }
  auto parsed = addition_tokens.diagnostics.empty()
                    ? frontend::Parser(addition_tokens.tokens).Parse()
                    : frontend::ParseResult{
                          .program = {},
                          .diagnostics = std::move(addition_tokens.diagnostics),
                      };
  if (!parsed.diagnostics.empty()) {
    // EOF 缺少函数体、参数或表达式时允许继续输入，内部语法错误立即报告。
    const auto eof_line = current.last_line - (body.declaration ? 2U : 1U);
    const bool incomplete = std::ranges::all_of(
        parsed.diagnostics,
        [&](const auto& diagnostic) -> bool { return diagnostic.location.line >= eof_line; });
    if (incomplete && !body.terminated) {
      return std::unexpected(SubmissionFailure(SubmissionStatus::Incomplete));
    }
    Remap(parsed.diagnostics, current);
    return std::unexpected(
        SubmissionFailure(SubmissionStatus::Rejected, std::move(parsed.diagnostics)));
  }
  parsed.program.standard_names = frontend::CollectStandardNames(lexed.tokens);
  auto accepted = std::make_shared<frontend::Program>(std::move(parsed.program));
  frontend::Program candidate;
  for (const auto& fragment : syntax_) {
    frontend::AppendProgram(candidate, frontend::CloneProgram(*fragment, {}));
  }
  frontend::AppendProgram(candidate, frontend::CloneProgram(*accepted, {}));
  BindEntries(candidate, current, body.terminated ? Echo::Suppress : Echo::Display);
  // 已执行入口由机器码所有者保活；历史语法只保存仍需绑定和检查的声明。
  std::erase_if(accepted->functions, [&](const auto& function) {
    return function.location.line == current.header_line;
  });
  auto text = history_ ? history_->text : std::string();
  text += addition;
  auto lines = IndexSourceLines(text);
  return ReplSourceCandidate{
      .entry = current,
      .input = std::string(source),
      .addition = std::move(accepted),
      .parsed =
          {
              .source = std::make_shared<const frontend::SourceSnapshot>(frontend::SourceSnapshot{
                  .text = std::move(text),
                  .lines = std::move(lines),
                  .tokens = {},
                  .comments = {},
                  .imports = candidate.imports,
                  .standard_names = candidate.standard_names,
              }),
              .program = std::move(candidate),
              .diagnostics = {},
          },
  };
}

void ReplSource::BindEntries(frontend::Program& program,
                             const ReplEntry& current,
                             Echo echo) const {
  frontend::Function* active = nullptr;
  for (auto& function : program.functions) {
    const auto line = function.location.line;
    if (line == current.header_line) {
      function.name = current.name();
      function.role = frontend::Function::Role::InteractiveEntry;
      active = &function;
    }
  }
  if (!current.wrapper) {
    for (const auto& global : program.globals) {
      if (global.location.line < next_line_) {
        continue;
      }
      auto statement = std::make_unique<frontend::Statement>();
      statement->location = global.location;
      statement->expression = std::make_unique<frontend::Expression>();
      auto& expression = *statement->expression;
      expression.kind = frontend::Expression::Kind::Name;
      expression.location = global.location;
      expression.text = global.name;
      active->body->statements.push_back(std::move(statement));
    }
  } else if (echo == Echo::Display && !active->body->statements.empty()) {
    auto& last = *active->body->statements.back();
    last.echo = last.kind == frontend::Statement::Kind::Expression;
  }
}

void ReplSource::Commit(ReplSourceCandidate& candidate) {
  if (!candidate.entry.wrapper) {
    syntax_.push_back(std::move(candidate.addition));
  }
  history_ = std::move(candidate.parsed.source);
  user_source_ += candidate.input;
  user_source_ += '\n';
  entries_.push_back(candidate.entry);
  next_line_ = candidate.entry.last_line;
}

}  // namespace vela::compiler
