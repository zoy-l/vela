#include "compiler/tooling/formatter.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/lexer.h"
#include "compiler/frontend/parser.h"
#include "compiler/frontend/token.h"

namespace vela::tooling {
namespace {

using frontend::TokenKind;
using frontend::TokenRole;

enum class ItemKind : std::uint8_t { Syntax, LineComment, BlockComment };

struct FormatItem final {
  TokenKind kind = TokenKind::EndOfFile;
  TokenRole role = TokenRole::Ordinary;
  ItemKind item = ItemKind::Syntax;
  std::string_view text;
  std::size_t offset = 0;
  std::optional<std::string> formatted = std::nullopt;

  [[nodiscard]] std::string_view spelling() const {
    return formatted ? std::string_view(*formatted) : text;
  }
};

void AppendStringRun(std::string& output, std::string_view text) {
  for (std::size_t index = 0; index < text.size(); ++index) {
    const auto character = text.at(index);
    if (character == '\\') {
      const auto escaped = text.at(++index);
      if (escaped != '"') {
        output += '\\';
      }
      output += escaped;
    } else {
      if (character == '\'') {
        output += '\\';
      }
      output += character;
    }
  }
}

std::string StringSpelling(std::string_view source, std::span<const frontend::Token> tokens) {
  std::string output;
  const auto start = tokens.front().offset;
  auto copied = start;
  for (const auto& token : tokens) {
    const auto kind = token.kind;
    if (kind != TokenKind::StringLiteral && kind != TokenKind::StringHead &&
        kind != TokenKind::StringMiddle && kind != TokenKind::StringTail) {
      continue;
    }
    output.append(source.substr(copied, token.offset - copied));
    auto run = source.substr(token.offset, token.length);
    if (kind == TokenKind::StringLiteral || kind == TokenKind::StringHead) {
      output += '\'';
      run.remove_prefix(1);
    }
    const auto interpolates = kind == TokenKind::StringHead || kind == TokenKind::StringMiddle;
    run.remove_suffix(interpolates ? 2 : 1);
    AppendStringRun(output, run);
    output.append(interpolates ? "${" : "'");
    copied = token.offset + token.length;
  }
  return output;
}

std::size_t StringTailIndex(std::span<const frontend::Token> tokens, std::size_t head) {
  std::size_t depth = 1;
  while (depth != 0) {
    const auto kind = tokens.subspan(++head).front().kind;
    if (kind == TokenKind::StringHead) {
      ++depth;
    }
    if (kind == TokenKind::StringTail) {
      --depth;
    }
  }
  return head;
}

std::vector<FormatItem> Items(std::string_view source, const frontend::LexResult& lexed) {
  std::vector<FormatItem> items;
  std::size_t comment = 0;
  const auto& comments = lexed.comments;
  const auto& tokens = lexed.tokens;
  for (std::size_t index = 0; index < tokens.size(); ++index) {
    const auto first = index;
    const auto& token = tokens.at(index);
    while (comment < comments.size() && comments.at(comment).offset < token.offset) {
      const auto& trivia = comments.at(comment++);
      const auto text = source.substr(trivia.offset, trivia.length);
      items.push_back({
          .item = text.starts_with("//") ? ItemKind::LineComment : ItemKind::BlockComment,
          .text = text,
          .offset = trivia.offset,
      });
    }
    if (token.kind == TokenKind::EndOfFile) {
      break;
    }
    auto end = token.offset + token.length;
    if (token.kind == TokenKind::StringHead) {
      index = StringTailIndex(tokens, index);
      const auto& tail = tokens.at(index);
      end = tail.offset + tail.length;
      while (comment < comments.size() && comments.at(comment).offset < end) {
        ++comment;
      }
    }
    items.push_back({
        .kind = token.kind,
        .role = token.role,
        .text = source.substr(token.offset, end - token.offset),
        .offset = token.offset,
        .formatted = token.kind == TokenKind::StringLiteral || token.kind == TokenKind::StringHead
                         ? std::optional(StringSpelling(
                               source, std::span(tokens).subspan(first, index - first + 1)))
                         : std::nullopt,
    });
  }
  return items;
}

bool IsControlParenthesis(TokenKind kind) {
  return kind == TokenKind::If || kind == TokenKind::While || kind == TokenKind::For ||
         kind == TokenKind::Switch || kind == TokenKind::Match || kind == TokenKind::Catch;
}

bool IsOpening(const FormatItem& item) {
  return item.kind == TokenKind::LeftBrace || item.kind == TokenKind::LeftParen ||
         item.kind == TokenKind::LeftBracket ||
         (item.kind == TokenKind::Less && item.role == TokenRole::TypeDelimiter);
}

bool IsClosing(const FormatItem& item) {
  return item.kind == TokenKind::RightBrace || item.kind == TokenKind::RightParen ||
         item.kind == TokenKind::RightBracket ||
         (item.kind == TokenKind::Greater && item.role == TokenRole::TypeDelimiter);
}

bool IsOperator(const FormatItem& item) {
  if (item.role != TokenRole::Ordinary) {
    return false;
  }
  switch (item.kind) {
    case TokenKind::Equal:
    case TokenKind::EqualEqual:
    case TokenKind::BangEqual:
    case TokenKind::Plus:
    case TokenKind::Minus:
    case TokenKind::Star:
    case TokenKind::Slash:
    case TokenKind::Less:
    case TokenKind::LessEqual:
    case TokenKind::Greater:
    case TokenKind::GreaterEqual:
    case TokenKind::AndAnd:
    case TokenKind::OrOr:
    case TokenKind::Arrow:
      return true;
    default:
      return false;
  }
}

bool NeedsSpace(const FormatItem& left, const FormatItem& right) {
  const auto previous = left.kind;
  const auto current = right.kind;
  if (left.role == TokenRole::ConditionalOperator || right.role == TokenRole::ConditionalOperator) {
    return true;
  }
  if (current == TokenKind::Dot || current == TokenKind::Comma || current == TokenKind::Semicolon ||
      current == TokenKind::Colon || current == TokenKind::Question ||
      current == TokenKind::RightParen || current == TokenKind::RightBracket ||
      right.role == TokenRole::TypeDelimiter) {
    return false;
  }
  if (left.role == TokenRole::PrefixOperator) {
    return right.role == TokenRole::PrefixOperator;
  }
  if (previous == TokenKind::Dot || previous == TokenKind::LeftParen ||
      previous == TokenKind::LeftBracket ||
      (previous == TokenKind::Less && left.role == TokenRole::TypeDelimiter)) {
    return false;
  }
  if (previous == TokenKind::Colon) {
    return true;
  }
  if (IsOperator(left) || IsOperator(right)) {
    return true;
  }
  if (current == TokenKind::LeftParen) {
    return IsControlParenthesis(previous) || previous == TokenKind::Return ||
           previous == TokenKind::Throw;
  }
  if (current == TokenKind::LeftBracket) {
    return previous == TokenKind::Return || previous == TokenKind::Throw ||
           previous == TokenKind::Comma || previous == TokenKind::Colon;
  }
  return true;
}

struct Scope final {
  TokenKind opener = TokenKind::LeftBrace;
  bool multiline = false;
  bool case_indented = false;
};

bool IsDeclarationStart(TokenKind kind) {
  switch (kind) {
    case TokenKind::Import:
    case TokenKind::Export:
    case TokenKind::External:
    case TokenKind::Func:
    case TokenKind::Async:
    case TokenKind::Class:
    case TokenKind::Interface:
    case TokenKind::Enum:
    case TokenKind::Type:
    case TokenKind::Let:
    case TokenKind::Const:
      return true;
    default:
      return false;
  }
}

class Formatter final {
 public:
  Formatter(std::string_view source, std::span<const FormatItem> items)
      : source_(source), items_(items), multiline_(items.size()) {
    std::vector<std::size_t> openers;
    std::vector<std::size_t> widths(items.size() + 1);
    constexpr std::size_t kContinuationWidth = 80;
    for (std::size_t index = 0; index < items.size(); ++index) {
      const auto& item = items.subspan(index).front();
      widths.at(index + 1) = widths.at(index) + item.spelling().size() + 1;
      if (item.kind == TokenKind::LeftParen || item.kind == TokenKind::LeftBracket) {
        openers.push_back(index);
      } else if ((item.kind == TokenKind::RightParen || item.kind == TokenKind::RightBracket) &&
                 !openers.empty()) {
        const auto opening = openers.back();
        openers.pop_back();
        multiline_.at(opening) = widths.at(index + 1) - widths.at(opening) > kContinuationWidth;
      }
    }
  }
  std::string Run();

 private:
  void Newline();
  void Blankline();
  void Space();
  void Text(std::string_view text);
  void Open(const FormatItem& item, std::size_t index);
  void Close(const FormatItem& item, TokenKind next);
  void Comment(const FormatItem& item, std::size_t index);
  void Separate(std::size_t index);
  void Emit(std::size_t index);

  std::string_view source_;
  std::span<const FormatItem> items_;
  std::vector<bool> multiline_;
  std::vector<Scope> scopes_;
  std::string output_;
  std::size_t indent_ = 0;
  bool case_label_ = false;
  TokenKind top_level_kind_ = TokenKind::EndOfFile;
};

void Formatter::Newline() {
  while (!output_.empty() && output_.back() == ' ') {
    output_.pop_back();
  }
  if (!output_.empty() && output_.back() != '\n') {
    output_.push_back('\n');
  }
}

void Formatter::Space() {
  if (!output_.empty() && output_.back() != '\n' && output_.back() != ' ') {
    output_.push_back(' ');
  }
}

void Formatter::Blankline() {
  Newline();
  if (!output_.empty() && !output_.ends_with("\n\n")) {
    output_ += '\n';
  }
}

void Formatter::Text(std::string_view text) {
  if (output_.empty() || output_.back() == '\n') {
    output_.append(indent_ * 2, ' ');
  }
  output_.append(text);
}

void Formatter::Open(const FormatItem& item, std::size_t index) {
  const auto brace = item.kind == TokenKind::LeftBrace;
  const auto multiline = brace || multiline_.at(index);
  if (brace) {
    Space();
  }
  Text(item.text);
  scopes_.push_back({.opener = item.kind, .multiline = multiline});
  if (multiline) {
    ++indent_;
    Newline();
  }
}

void Formatter::Close(const FormatItem& item, TokenKind next) {
  const auto scope = scopes_.back();
  scopes_.pop_back();
  if (scope.case_indented) {
    --indent_;
  }
  if (scope.multiline) {
    --indent_;
    Newline();
  }
  Text(item.text);
  if (item.kind != TokenKind::RightBrace) {
    return;
  }
  switch (next) {
    case TokenKind::Else:
    case TokenKind::Catch:
    case TokenKind::Finally:
      Space();
      return;
    case TokenKind::Semicolon:
    case TokenKind::Comma:
    case TokenKind::RightParen:
    case TokenKind::RightBracket:
    case TokenKind::Dot:
    case TokenKind::LeftParen:
      return;
    default:
      Newline();
  }
}

void Formatter::Comment(const FormatItem& item, std::size_t index) {
  if (index != 0) {
    const auto& previous = items_.subspan(index - 1).front();
    const auto end = previous.offset + previous.text.size();
    const auto gap = source_.substr(end, item.offset - end);
    if (gap.contains('\n')) {
      if (scopes_.empty() && previous.item == ItemKind::Syntax &&
          (previous.kind == TokenKind::Semicolon || previous.kind == TokenKind::RightBrace)) {
        Blankline();
      } else {
        Newline();
      }
    } else if (output_.ends_with('\n')) {
      output_.pop_back();
    }
  }
  Space();
  auto text = item.text;
  if (text.ends_with('\r')) {
    text.remove_suffix(1);
  }
  // 注释文本可以跨行；统一 CRLF 为 LF，保留其余原文。
  std::string normalized(text);
  std::erase(normalized, '\r');
  Text(normalized);
  if (item.item == ItemKind::LineComment || text.contains('\n')) {
    Newline();
  } else {
    Space();
  }
}

void Formatter::Separate(std::size_t index) {
  const auto& item = items_.subspan(index).front();
  const auto kind = item.kind;
  if (index != 0 && output_.ends_with('\n') && !IsClosing(item)) {
    const auto& previous = items_.subspan(index - 1).front();
    const auto previous_end = previous.offset + previous.text.size();
    const auto gap = source_.substr(previous_end, item.offset - previous_end);
    if (gap.find('\n') != gap.rfind('\n')) {
      Blankline();
    }
    if (scopes_.empty() && previous.item == ItemKind::Syntax && IsDeclarationStart(kind) &&
        (previous.kind == TokenKind::Semicolon || previous.kind == TokenKind::RightBrace) &&
        (kind != TokenKind::Import || top_level_kind_ != TokenKind::Import)) {
      Blankline();
    }
  }
  if (scopes_.empty() && IsDeclarationStart(kind)) {
    top_level_kind_ = kind;
  }
}

void Formatter::Emit(std::size_t index) {
  const auto& item = items_.subspan(index).front();
  if (item.item != ItemKind::Syntax) {
    Comment(item, index);
    return;
  }
  Separate(index);
  const auto kind = item.kind;
  const auto next =
      index + 1 < items_.size() ? items_.subspan(index + 1).front().kind : TokenKind::EndOfFile;
  if (kind == TokenKind::Case || kind == TokenKind::Default) {
    Newline();
    auto& scope = scopes_.back();
    if (scope.case_indented) {
      --indent_;
      scope.case_indented = false;
    }
    case_label_ = true;
  }
  if (IsClosing(item)) {
    Close(item, next);
    return;
  }
  if (index != 0 && NeedsSpace(items_.subspan(index - 1).front(), item)) {
    Space();
  }
  if (IsOpening(item)) {
    Open(item, index);
    return;
  }
  Text(item.spelling());
  if (kind == TokenKind::Colon && item.role != TokenRole::ConditionalOperator && case_label_) {
    case_label_ = false;
    scopes_.back().case_indented = true;
    ++indent_;
    Newline();
  } else if (kind == TokenKind::Semicolon) {
    if (!scopes_.empty() && scopes_.back().opener == TokenKind::LeftParen) {
      Space();
    } else {
      Newline();
    }
  } else if (kind == TokenKind::Comma) {
    if (!scopes_.empty() && scopes_.back().multiline) {
      Newline();
    } else {
      Space();
    }
  }
}

std::string Formatter::Run() {
  for (std::size_t index = 0; index < items_.size(); ++index) {
    Emit(index);
  }
  Newline();
  return std::move(output_);
}

}  // namespace

std::expected<std::string, frontend::Diagnostics> FormatSource(std::string_view source) {
  auto lexed = frontend::Lexer(source, 0, frontend::CommentRetention::Preserve).Tokenize();
  if (!lexed.diagnostics.empty()) {
    return std::unexpected(std::move(lexed.diagnostics));
  }
  auto parsed = frontend::Parser(lexed.tokens).Parse();
  if (!parsed.diagnostics.empty()) {
    return std::unexpected(std::move(parsed.diagnostics));
  }
  const auto items = Items(source, lexed);
  return Formatter(source, items).Run();
}

}  // namespace vela::tooling
