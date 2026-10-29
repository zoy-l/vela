#include "compiler/repl_session.h"
#include "compiler/frontend/ast_clone.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "base/path.h"
#include "compiler/compiler.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/lexer.h"
#include "compiler/frontend/parser.h"
#include "compiler/frontend/source_loader.h"
#include "compiler/frontend/specialization.h"
#include "compiler/frontend/token.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/repl_identity.h"
#include "runtime/isolate.h"
#include "vm/program.h"
#include "vm/vm.h"

namespace vela::compiler {
namespace {

bool IsDeclaration(frontend::TokenKind kind) {
  using Kind = frontend::TokenKind;
  switch (kind) {
    case Kind::Async:
    case Kind::Func:
    case Kind::Class:
    case Kind::Enum:
    case Kind::Interface:
    case Kind::Type:
    case Kind::Import:
    case Kind::Export:
    case Kind::External:
    case Kind::Let:
    case Kind::Const:
      return true;
    default:
      return false;
  }
}

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

SubmissionResult SubmissionFailure(SubmissionStatus status,
                                   frontend::Diagnostics diagnostics = {}) {
  SubmissionResult result;
  result.status = status;
  result.diagnostics = std::move(diagnostics);
  return result;
}

struct InputBody final {
  std::string text;
  bool declaration = false;
  bool terminated = false;
};

InputBody NormalizeBody(std::string_view source, const frontend::LexResult& lexed) {
  using Token = frontend::TokenKind;
  const auto first_kind = lexed.tokens.front().kind;
  const auto last_kind =
      lexed.tokens.size() > 1 ? lexed.tokens.at(lexed.tokens.size() - 2).kind : Token::EndOfFile;
  InputBody body{
      .text = std::string(source),
      .declaration = IsDeclaration(first_kind),
      .terminated = last_kind == Token::Semicolon,
  };
  const bool block_statement = first_kind == Token::LeftBrace || first_kind == Token::If ||
                               first_kind == Token::While || first_kind == Token::For ||
                               first_kind == Token::Switch || first_kind == Token::Try;
  const bool needs_semicolon = !body.terminated && (last_kind != Token::RightBrace ||
                                                    (!body.declaration && !block_statement));
  if (needs_semicolon && lexed.tokens.size() > 1) {
    const auto& last = lexed.tokens.at(lexed.tokens.size() - 2);
    body.text.insert(last.offset + last.length, 1, ';');
  }
  return body;
}

}  // namespace

ReplSession::ReplSession(const std::filesystem::path& directory, std::size_t gc_threshold)
    : path_(directory / "__vela_repl__.vela"),
      gc_threshold_(gc_threshold),
      isolate_(std::make_unique<runtime::Isolate>(gc_threshold)),
      machine_(std::make_unique<vm::VirtualMachine>(*isolate_)) {}

std::expected<void, std::vector<runtime::ExternalResourceInfo>> ReplSession::Reset() {
  auto stopped = isolate_->TryStop();
  if (!stopped) {
    return stopped;
  }
  if (!bindings_) {
    bindings_ = isolate_->FreezeBindings();
  }
  program_ = {};
  machine_.reset();
  compilation_ = {};
  isolate_ = std::make_unique<runtime::Isolate>(*bindings_, gc_threshold_);
  machine_ = std::make_unique<vm::VirtualMachine>(*isolate_);
  modules_ = {};
  syntax_.clear();
  history_.reset();
  user_source_.clear();
  entries_.clear();
  next_line_ = 1;
  return {};
}

ReplSession::Entry ReplSession::Locate(std::uint32_t line, const Entry& current) const {
  if (line >= current.first_line || entries_.empty()) {
    return current;
  }
  const auto found = std::ranges::upper_bound(entries_, line, {}, &Entry::first_line);
  return found == entries_.begin() ? entries_.front() : *std::prev(found);
}

void ReplSession::Remap(frontend::Diagnostics& diagnostics, const Entry& current) const {
  for (auto& diagnostic : diagnostics) {
    if (diagnostic.location.source_id != 0) {
      continue;
    }
    const auto& entry = Locate(diagnostic.location.line, current);
    diagnostic.source_path = SubmissionPath(entry.submission);
    diagnostic.location.line = diagnostic.location.line >= entry.first_line
                                   ? diagnostic.location.line - entry.first_line + 1
                                   : 1;
  }
}

void ReplSession::Remap(vm::ExecutionResult& result, const Entry& current) const {
  for (auto& frame : result.stack_trace) {
    if (frame.location.source_id == 0) {
      const auto& entry = Locate(frame.location.line, current);
      frame.source_path = SubmissionPath(entry.submission);
      frame.location.line =
          frame.location.line >= entry.first_line ? frame.location.line - entry.first_line + 1 : 1;
      if (frame.function.starts_with("$repl")) {
        frame.function = "<submission>";
      }
    }
  }
}

std::expected<ReplSession::Prepared, SubmissionResult> ReplSession::Prepare(
    std::string_view source) {
  const auto lexed = frontend::Lexer(source).Tokenize();
  if (lexed.diagnostics.empty() && lexed.tokens.size() == 1) {
    return std::unexpected(SubmissionResult());
  }
  if (HasOpenDelimiter(lexed)) {
    return std::unexpected(SubmissionFailure(SubmissionStatus::Incomplete));
  }
  const auto body = NormalizeBody(source, lexed);
  std::string addition;
  Entry current{
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
  return Prepared{
      .entry = current,
      .addition = std::move(accepted),
      .parsed = {.source = std::make_shared<const frontend::SourceSnapshot>(
                     frontend::SourceSnapshot{.text = std::move(text),
                                              .tokens = {},
                                              .comments = {},
                                              .imports = candidate.imports}),
                 .program = std::move(candidate),
                 .diagnostics = {}},
  };
}

void ReplSession::BindEntries(frontend::Program& program, const Entry& current, Echo echo) const {
  frontend::Function* active = nullptr;
  for (auto& function : program.functions) {
    const auto line = function.location.line;
    const auto retired = std::ranges::lower_bound(entries_, line, {}, &Entry::header_line);
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

CompileResult ReplSession::Compile(Prepared& prepared,
                                   CompilationState& compilation,
                                   frontend::ModuleCache& modules) {
  auto loaded = frontend::LoadPreparedProgram(PathText(path_),
                                              {.source = prepared.parsed.source,
                                               .program = std::move(prepared.parsed.program),
                                               .diagnostics = {}},
                                              modules);
  auto diagnostics = std::move(loaded.diagnostics);
  if (diagnostics.empty()) {
    diagnostics = frontend::SpecializeGenerics(loaded.program);
  }
  if (!diagnostics.empty()) {
    return {
        .program = {},
        .diagnostics = std::move(diagnostics),
    };
  }
  NameSessionLambdas(loaded.program);
  auto checked = frontend::TypeChecker(&compilation.interfaces).Check(loaded.program);
  if (!checked.diagnostics.empty()) {
    return {
        .program = {},
        .diagnostics = std::move(checked.diagnostics),
    };
  }
  return CompileProgram(loaded.program, checked.model, *isolate_, &compilation);
}

SubmissionResult ReplSession::Submit(std::string_view source) {
  auto prepared = Prepare(source);
  if (!prepared) {
    return std::move(prepared.error());
  }
  const auto current = prepared->entry;
  auto modules = modules_;
  auto compilation = compilation_;
  auto compiled = Compile(*prepared, compilation, modules);
  if (!compiled.Succeeded()) {
    Remap(compiled.diagnostics, current);
    return SubmissionFailure(SubmissionStatus::Rejected, std::move(compiled.diagnostics));
  }
  if (!bindings_) {
    bindings_ = isolate_->FreezeBindings();
  }
  program_ = compiled.program.Retained();
  compilation_ = std::move(compilation);
  modules_ = std::move(modules);
  syntax_.push_back(std::move(prepared->addition));
  history_ = std::move(prepared->parsed.source);
  user_source_ += source;
  user_source_ += '\n';
  const auto entry = program_.FindFunction("$repl" + std::to_string(current.submission));
  auto execution = machine_->Execute(entry);
  Remap(execution, current);
  entries_.push_back(current);
  next_line_ = current.last_line;
  const auto status =
      execution.Succeeded() ? SubmissionStatus::Executed : SubmissionStatus::RuntimeError;
  return {.status = status, .diagnostics = {}, .execution = std::move(execution)};
}

}  // namespace vela::compiler
