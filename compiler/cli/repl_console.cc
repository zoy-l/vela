#include "compiler/cli/repl_console.h"

#include <cstdint>
#include <filesystem>
#include <istream>
#include <ostream>
#include <string>
#include <string_view>

#include "base/process_exit.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/project/project_config.h"
#include "compiler/repl/repl_session.h"
#include "compiler/repl/repl_source.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/external_resources.h"
#include "vm/vm.h"

namespace vela::tooling {
namespace {

enum class CommandResult : std::uint8_t { Continue, Quit };

CommandResult Command(std::string_view line, compiler::ReplSession& session, ReplStreams streams) {
  auto& errors = streams.errors.get();
  if (line == ".quit") {
    return CommandResult::Quit;
  }
  if (line == ".help") {
    errors << ".help  commands\n.source  accepted input\n.reset  discard session\n.quit  exit\n"
           << "Expressions echo their result; a trailing semicolon suppresses it.\n";
  } else if (line == ".source") {
    streams.output.get() << session.source();
  } else if (line == ".reset") {
    const auto reset = session.Reset();
    if (reset) {
      session.isolate().SetOutput(streams.output.get());
    } else {
      errors << "vela: cannot reset REPL; release external handles first\n";
      for (const auto& resource : reset.error()) {
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
  compiler::ReplSession session(directory, runtime::kDefaultGcThreshold, *options);
  session.isolate().SetOutput(output);
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
      if (Command(line, session, streams) == CommandResult::Quit) {
        return 0;
      }
      continue;
    }
    pending += line;
    pending += '\n';
    const auto result = session.Submit(pending);
    if (result.status == compiler::SubmissionStatus::Incomplete) {
      continue;
    }
    for (const auto& diagnostic : result.diagnostics) {
      frontend::WriteDiagnostic(errors, diagnostic);
    }
    if (result.status == compiler::SubmissionStatus::RuntimeError) {
      vm::WriteRuntimeError(errors, result.execution);
    }
    pending.clear();
  }
}

}  // namespace vela::tooling
