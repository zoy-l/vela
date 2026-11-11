#include "compiler/cli/repl_console.h"

#include <cstdint>
#include <filesystem>
#include <istream>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/path.h"
#include "base/process_exit.h"
#include "base/streams.h"
#include "compiler/cli/development_diagnostics.h"
#include "compiler/development/development_repl.h"
#include "compiler/development/development_session.h"
#include "compiler/development/source_session.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/project/project_config.h"
#include "compiler/repl/repl_source.h"
#include "runtime/heap/heap.h"
#include "runtime/process/process_entry.h"

namespace vela::tooling {
namespace {

enum class CommandResult : std::uint8_t { Continue, Quit };

CommandResult Command(std::string_view line,
                      compiler::DevelopmentSession& session,
                      compiler::DevelopmentRepl& repl,
                      Streams streams) {
  auto& errors = streams.errors.get();
  if (line == ".quit") {
    return CommandResult::Quit;
  }
  if (line == ".help") {
    errors << ".help  commands\n.source  accepted input\n.reset  discard session\n.quit  exit\n"
           << "Expressions echo their result; a trailing semicolon suppresses it.\n";
  } else if (line == ".source") {
    streams.output.get() << repl.source();
  } else if (line == ".reset") {
    const auto reset = repl.Reset({
        .base_revision = session.snapshot()->revision,
        .changes = {},
        .added_roots = {},
    });
    if (reset.status == compiler::DevelopmentStatus::Accepted) {
      session.isolate().SetOutput(streams.output.get());
    } else {
      errors << "vela: cannot reset REPL: " << reset.message << '\n';
      WriteExternalResources(errors, reset.blockers);
    }
  } else {
    errors << "vela: unknown REPL command; use .help\n";
  }
  return CommandResult::Continue;
}

void ReportSubmission(const compiler::DevelopmentSubmission& result, std::ostream& errors) {
  WriteDiagnostics(errors, result.diagnostics);
  if (result.update && result.status == compiler::SubmissionStatus::Rejected) {
    WriteDiagnostics(errors, result.update->diagnostics);
    if (result.update->diagnostics.empty()) {
      errors << "vela: " << result.update->message << '\n';
    }
  }
  WriteExecutionFailure(errors, result.runtime_diagnostic, result.execution, "vela: ");
}

int ReadSubmissions(compiler::DevelopmentSession& session,
                    compiler::DevelopmentRepl& repl,
                    Streams streams) {
  auto& input = streams.input.get();
  auto& errors = streams.errors.get();
  errors << "Vela REPL (.help for commands)\n";
  std::string pending;
  std::string line;
  for (;;) {
    errors << (pending.empty() ? "> " : "... ") << std::flush;
    if (!std::getline(input, line)) {
      if (!pending.empty()) {
        errors << "vela: incomplete submission at end of input\n";
        return kSourceErrorExitCode;
      }
      return input.bad() ? kIoErrorExitCode : 0;
    }
    if (pending.empty() && line.starts_with('.')) {
      if (Command(line, session, repl, streams) == CommandResult::Quit) {
        return 0;
      }
      continue;
    }
    pending += line;
    pending += '\n';
    const auto result = repl.Submit(pending);
    if (result.status == compiler::SubmissionStatus::Incomplete) {
      continue;
    }
    ReportSubmission(result, errors);
    pending.clear();
  }
}

}  // namespace

int RunRepl(const std::filesystem::path& directory, Streams streams) {
  auto& errors = streams.errors.get();
  const auto repl_path = directory / "__vela_repl__.vela";
  const auto options = compiler::ReadSourceConfiguration(repl_path, errors);
  if (!options) {
    return options.error().exit_code;
  }
  compiler::DevelopmentOptions development;
  development.diagnostics = options->diagnostics;
  development.reflection = options->reflection;
  const auto roots = frontend::ReflectionFileModules(options->reflection);
  frontend::ModuleCache cache;
  const auto reflected_sources = frontend::CaptureSourceFiles(roots, cache, {});
  if (!reflected_sources.diagnostics.empty()) {
    WriteDiagnostics(errors, reflected_sources.diagnostics);
    return kSourceErrorExitCode;
  }
  const auto entry = PathText(directory / "__vela_repl_host__.vela");
  auto created = compiler::DevelopmentSession::Create(entry, {}, std::move(development));
  if (!created) {
    WriteDiagnostics(errors, created.error());
    return kSourceErrorExitCode;
  }
  auto& session = **created;
  session.isolate().SetOutput(streams.output.get());
  compiler::SourceUpdate update{
      .changes = {{.path = entry, .text = "func main(): void {}"}},
      .added_roots = {},
  };
  for (const auto& source : reflected_sources.sources) {
    update.changes.push_back({
        .path = source.path,
        .text = std::string(source.text),
        .syntax = cache.sources.find(source.path)->second,
    });
  }
  auto candidate = session.Prepare(std::move(update), compiler::DevelopmentOperation::Load);
  if (!candidate) {
    errors << "vela: cannot initialize REPL: " << candidate.error().message << '\n';
    return kSourceErrorExitCode;
  }
  (void)session.Submit(std::move(*candidate));
  const auto loaded = session.Pump();
  if (loaded.status != compiler::DevelopmentStatus::Accepted) {
    errors << "vela: cannot initialize REPL: " << loaded.message << '\n';
    return kSourceErrorExitCode;
  }
  compiler::DevelopmentRepl repl(session, PathText(repl_path));
  return ReadSubmissions(session, repl, streams);
}

}  // namespace vela::tooling
