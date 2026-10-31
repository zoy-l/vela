#include "compiler/repl/repl_session.h"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "base/path.h"
#include "compiler/bytecode/compilation_state.h"
#include "compiler/bytecode/compiler.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/frontend/typing/type_checker.h"
#include "compiler/repl/repl_identity.h"
#include "compiler/repl/repl_source.h"
#include "runtime/isolate/external_resources.h"
#include "runtime/isolate/isolate.h"
#include "vm/program.h"
#include "vm/vm.h"

namespace vela::compiler {

ReplSession::ReplSession(const std::filesystem::path& directory,
                         std::size_t gc_threshold,
                         frontend::DiagnosticOptions options)
    : path_(directory / "__vela_repl__.vela"),
      gc_threshold_(gc_threshold),
      options_(options),
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
  source_ = {};
  return {};
}

CompileResult ReplSession::Compile(ReplSourceCandidate& prepared,
                                   CompilationState& compilation,
                                   frontend::ModuleCache& modules) {
  auto loaded = frontend::LoadPreparedProgram(PathText(path_),
                                              {
                                                  .source = prepared.parsed.source,
                                                  .program = std::move(prepared.parsed.program),
                                                  .diagnostics = {},
                                              },
                                              modules);
  auto diagnostics = std::move(loaded.diagnostics);
  if (!diagnostics.empty()) {
    return {.program = {}, .diagnostics = std::move(diagnostics)};
  }
  auto checked = frontend::TypeChecker(&compilation.interfaces, options_).Check(loaded.program);
  NameSessionLambdas(loaded.program);
  if (!checked.Succeeded()) {
    return {.program = {}, .diagnostics = std::move(checked.diagnostics)};
  }
  auto compiled = CompileProgram(loaded.program, checked.model, *isolate_, &compilation);
  for (auto& diagnostic : checked.diagnostics) {
    compiled.diagnostics.push_back(std::move(diagnostic));
  }
  return compiled;
}

SubmissionResult ReplSession::Submit(std::string_view source) {
  auto prepared = source_.Prepare(source);
  if (!prepared) {
    return {
        .status = prepared.error().status,
        .diagnostics = std::move(prepared.error().diagnostics),
        .execution = {},
    };
  }
  const auto current = prepared->entry;
  auto modules = modules_;
  auto compilation = compilation_;
  auto compiled = Compile(*prepared, compilation, modules);
  if (!compiled.Succeeded()) {
    source_.Remap(compiled.diagnostics, current);
    return {
        .status = SubmissionStatus::Rejected,
        .diagnostics = std::move(compiled.diagnostics),
        .execution = {},
    };
  }
  if (!bindings_) {
    bindings_ = isolate_->FreezeBindings();
  }
  program_ = compiled.program.Retained();
  compilation_ = std::move(compilation);
  modules_ = std::move(modules);
  source_.Commit(*prepared);
  auto execution = machine_->Execute(program_.FindFunction(current.name()));
  for (auto& frame : execution.stack_trace) {
    if (frame.location.source_id == 0) {
      source_.Remap(frame.location, frame.source_path, current);
      if (frame.function.starts_with("$repl")) {
        frame.function = "<submission>";
      }
    }
  }
  const auto status =
      execution.Succeeded() ? SubmissionStatus::Executed : SubmissionStatus::RuntimeError;
  source_.Remap(compiled.diagnostics, current);
  return {
      .status = status,
      .diagnostics = std::move(compiled.diagnostics),
      .execution = std::move(execution),
  };
}

}  // namespace vela::compiler
