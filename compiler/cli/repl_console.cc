#include "compiler/cli/repl_console.h"

#include <cstdint>
#include <filesystem>
#include <istream>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

#include "base/execution_status.h"
#include "base/path.h"
#include "base/process_exit.h"
#include "compiler/development/development_repl.h"
#include "compiler/development/development_session.h"
#include "compiler/development/source_session.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/project/project_config.h"
#include "compiler/repl/repl_source.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/external_resources.h"
#include "runtime/process/process_entry.h"

namespace vela::tooling {
namespace {

enum class CommandResult : std::uint8_t { Continue, Quit };

CommandResult Command(std::string_view line,
                       compiler::DevelopmentSession& session,
                       compiler::DevelopmentRepl& repl,
                       ReplStreams streams) {
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
      for (const auto& resource : reset.blockers) {
        errors << "  " << runtime::ExternalResourceKindName(resource.kind) << " #"
               << resource.identity << ": " << resource.references << " reference(s)\n";
      }
    }
  } else {
    errors << "vela: unknown REPL command; use .help\n";
  }
  return CommandResult::Continue;
}

}  // namespace

int RunRepl(const std::filesystem::path& directory, ReplStreams streams) {
  auto& input = streams.input.get();
  auto& output = streams.output.get();
  auto& errors = streams.errors.get();
  const auto options = compiler::LoadSourceDiagnosticOptions(directory / "__vela_repl__.vela");
  if (!options) {
    frontend::WriteDiagnostic(errors, compiler::ConfigurationDiagnostic(options.error()));
    return options.error().exit_code;
  }
  compiler::DevelopmentOptions development;
  development.diagnostics = *options;
  const auto entry = PathText(directory / "__vela_repl_host__.vela");
  auto created = compiler::DevelopmentSession::Create(entry, {}, std::move(development));
  if (!created) {
    for (const auto& diagnostic : created.error()) {
      frontend::WriteDiagnostic(errors, diagnostic);
    }
    return kSourceErrorExitCode;
  }
  auto& session = **created;
  session.isolate().SetOutput(output);
  auto candidate = session.Prepare(
      {.changes = {{.path = entry, .text = "func main(): void {}"}}, .added_roots = {}},
      compiler::DevelopmentOperation::Load);
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
  compiler::DevelopmentRepl repl(session, PathText(directory / "__vela_repl__.vela"));
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
    for (const auto& diagnostic : result.diagnostics) {
      frontend::WriteDiagnostic(errors, diagnostic);
    }
    if (result.update && result.status == compiler::SubmissionStatus::Rejected) {
      for (const auto& diagnostic : result.update->diagnostics) {
        frontend::WriteDiagnostic(errors, diagnostic);
      }
      if (result.update->diagnostics.empty()) {
        errors << "vela: " << result.update->message << '\n';
      }
    }
    if (result.runtime_diagnostic) {
      runtime::WriteRuntimeDiagnostic(errors, *result.runtime_diagnostic);
    } else if (result.execution) {
      errors << "vela: " << ExecutionStatusName(*result.execution) << '\n';
    }
    pending.clear();
  }
}

}  // namespace vela::tooling
