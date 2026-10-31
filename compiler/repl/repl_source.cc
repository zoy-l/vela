#include "compiler/repl/repl_source.h"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "base/source_location.h"
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

bool IsBlockDeclaration(std::span<const frontend::Token> tokens) {
  using Kind = frontend::TokenKind;
  while (tokens.front().kind == Kind::Export || tokens.front().kind == Kind::Async) {
    tokens = tokens.subspan(1);
  }
  const auto kind = tokens.front().kind;
  return kind == Kind::Func || kind == Kind::Class || kind == Kind::Interface || kind == Kind::Enum;
}

InputBody NormalizeBody(std::string_view source, const frontend::LexResult& lexed) {
  using Token = frontend::TokenKind;
  const auto first_kind = lexed.tokens.front().kind;
  const auto last_kind =
      lexed.tokens.size() > 1 ? lexed.tokens.at(lexed.tokens.size() - 2).kind : Token::EndOfFile;
  InputBody body{
      .text = std::string(source),
      .declaration = frontend::IsDeclarationStart(first_kind),
      .terminated = last_kind == Token::Semicolon,
  };
  const bool block_statement = first_kind == Token::LeftBrace || first_kind == Token::If ||
                               first_kind == Token::While || first_kind == Token::For ||
                               first_kind == Token::Switch || first_kind == Token::Try;
  const bool needs_semicolon = !body.terminated && !IsBlockDeclaration(lexed.tokens) &&
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
  auto parsed = frontend::ParseSource(addition);
  for (auto& diagnostic : parsed.diagnostics) {
    diagnostic.location.line += next_line_ - 1;
  }
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
  auto accepted = std::make_shared<const frontend::Program>(
      frontend::CloneProgram(parsed.program, {.line = next_line_}));
  frontend::Program candidate;
  for (const auto& fragment : syntax_) {
    frontend::AppendProgram(candidate, frontend::CloneProgram(*fragment, {}));
  }
  frontend::AppendProgram(candidate, frontend::CloneProgram(*accepted, {}));
  BindEntries(candidate, current, body.terminated ? Echo::Suppress : Echo::Display);
  auto text = history_ ? history_->text : std::string();
  text += addition;
  return ReplSourceCandidate{
      .entry = current,
      .input = std::string(source),
      .addition = std::move(accepted),
      .parsed =
          {
              .source = std::make_shared<const frontend::SourceSnapshot>(frontend::SourceSnapshot{
                  .text = std::move(text),
                  .tokens = {},
                  .comments = {},
                  .imports = candidate.imports,
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
    const auto retired = std::ranges::lower_bound(entries_, line, {}, &ReplEntry::header_line);
    const bool historical = retired != entries_.end() && retired->header_line == line;
    if (line == current.header_line || historical) {
      const auto submission =
          line == current.header_line ? current.submission : retired->submission;
      function.name = "$repl" + std::to_string(submission);
      function.role = line == current.header_line ? frontend::Function::Role::InteractiveEntry
                                                  : frontend::Function::Role::RetiredEntry;
      if (line == current.header_line) {
        active = &function;
      }
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
  syntax_.push_back(std::move(candidate.addition));
  history_ = std::move(candidate.parsed.source);
  user_source_ += candidate.input;
  user_source_ += '\n';
  entries_.push_back(candidate.entry);
  next_line_ = candidate.entry.last_line;
}

}  // namespace vela::compiler
