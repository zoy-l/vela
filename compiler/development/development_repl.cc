#include "compiler/development/development_repl.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/path.h"
#include "base/source_location.h"
#include "compiler/development/development_session.h"
#include "compiler/development/source_session.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/frontend/syntax/ast.h"
#include "compiler/frontend/syntax/ast_clone.h"
#include "compiler/frontend/syntax/parser.h"
#include "compiler/repl/repl_source.h"
#include "runtime/async/future.h"

namespace vela::compiler {

DevelopmentRepl::DevelopmentRepl(DevelopmentSession& session, const std::string& path)
    : session_(&session), path_(PathText(Utf8Path(path).lexically_normal())) {}

void DevelopmentRepl::Synchronize() {
  const auto snapshot = session_->snapshot();
  const auto current = snapshot->sources.find(path_);
  if (current == snapshot->sources.end() || *current->second != accepted_text_) {
    source_ = {};
    accepted_text_.clear();
  }
}

std::string_view DevelopmentRepl::source() const noexcept {
  const auto snapshot = session_->snapshot();
  const auto current = snapshot->sources.find(path_);
  return current != snapshot->sources.end() && *current->second == accepted_text_
             ? source_.source()
             : std::string_view();
}

void DevelopmentRepl::Remap(frontend::Diagnostics& diagnostics, const ReplEntry& entry) const {
  for (auto& diagnostic : diagnostics) {
    if (diagnostic.source_path == path_) {
      source_.Remap(diagnostic.location, diagnostic.source_path, entry);
    }
  }
}

DevelopmentResult DevelopmentRepl::Publish(DevelopmentCandidate candidate) {
  auto result = session_->Submit(std::move(candidate));
  while (result.status == DevelopmentStatus::WaitingForSafepoint) {
    result = session_->Pump();
  }
  return result;
}

DevelopmentSubmission DevelopmentRepl::Submit(std::string_view input) {
  Synchronize();
  auto prepared = source_.Prepare(input);
  if (!prepared) {
    return {
        .status = prepared.error().status,
        .diagnostics = std::move(prepared.error().diagnostics),
        .execution = {},
        .runtime_diagnostic = {},
        .update = {},
    };
  }
  const auto entry = prepared->entry;
  auto& parsed = prepared->parsed;
  // 旧交互入口的机器码由闭包和续延保活；新图不再绑定或检查历史语句。
  std::erase_if(parsed.program.functions, [](const auto& function) {
    return function.role == frontend::Function::Role::RetiredEntry;
  });
  auto syntax = std::make_shared<const frontend::ParsedSource>(frontend::ParsedSource{
      .source = parsed.source,
      .program = std::move(parsed.program),
      .diagnostics = {},
  });
  const auto snapshot = session_->snapshot();
  if (snapshot->sources.contains(path_) && !snapshot->prepared_sources.contains(path_)) {
    return {
        .status = SubmissionStatus::Rejected,
        .diagnostics =
            {
                {
                    .location = {},
                    .message = "interactive module conflicts with a loaded source file",
                    .source_path = path_,
                },
            },
        .execution = {},
        .runtime_diagnostic = {},
        .update = {},
    };
  }
  auto modules = snapshot->modules;
  // 已接纳模块来自固定缓存；只在本次导入首次出现时读取文件，随后请求仅用此快照。
  auto imported =
      frontend::LoadPreparedProgram(path_,
                                    {
                                        .source = syntax->source,
                                        .program = frontend::CloneProgram(syntax->program, {}),
                                        .diagnostics = {},
                                    },
                                    modules);
  if (!imported.diagnostics.empty()) {
    Remap(imported.diagnostics, entry);
    return {
        .status = SubmissionStatus::Rejected,
        .diagnostics = std::move(imported.diagnostics),
        .execution = {},
        .runtime_diagnostic = {},
        .update = {},
    };
  }
  SourceUpdate update{
      .base_revision = snapshot->revision,
      .changes = {{.path = path_, .text = syntax->source->text, .syntax = syntax}},
      .added_roots = {path_},
  };
  for (const auto& [path, module] : modules.sources) {
    if (path != path_ && !snapshot->sources.contains(path)) {
      update.changes.push_back({.path = path, .text = module->source->text});
    }
  }
  auto candidate = session_->Prepare(std::move(update), DevelopmentOperation::HotReload);
  DevelopmentSubmission result;
  if (!candidate) {
    result.status = SubmissionStatus::Rejected;
    result.update = std::move(candidate.error());
    Remap(result.update->diagnostics, entry);
    return result;
  }
  result.update = Publish(std::move(*candidate));
  if (result.update->status != DevelopmentStatus::Accepted &&
      result.update->status != DevelopmentStatus::RefreshFailed) {
    result.status = SubmissionStatus::Rejected;
    return result;
  }
  accepted_text_ = syntax->source->text;
  source_.Commit(*prepared);
  const auto entry_name = entry.name();
  const auto executed = session_->InvokeEntry({.module = path_, .name = entry_name});
  result.status = executed ? SubmissionStatus::Executed : SubmissionStatus::RuntimeError;
  if (!executed) {
    result.execution = executed.error();
    result.runtime_diagnostic = session_->last_diagnostic();
    if (result.runtime_diagnostic) {
      Remap(*result.runtime_diagnostic, entry);
    }
  }
  return result;
}

void DevelopmentRepl::Remap(runtime::FutureDiagnostic& diagnostic, const ReplEntry& entry) const {
  for (auto& frame : diagnostic.stack_trace) {
    if (frame.source_path == path_) {
      SourceLocation location{.line = frame.line, .column = frame.column};
      source_.Remap(location, frame.source_path, entry);
      frame.line = location.line;
      if (frame.function.starts_with("$repl")) {
        frame.function = "<submission>";
      }
    }
  }
}

DevelopmentResult DevelopmentRepl::Reset(SourceUpdate update) {
  update.changes.push_back({.path = path_, .text = std::nullopt});
  auto candidate = session_->Prepare(std::move(update), DevelopmentOperation::Reload);
  if (!candidate) {
    return std::move(candidate.error());
  }
  auto result = Publish(std::move(*candidate));
  Synchronize();
  return result;
}

}  // namespace vela::compiler
