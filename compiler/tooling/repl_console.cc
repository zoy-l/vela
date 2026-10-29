#include "compiler/tooling/repl_console.h"

#include <cstdint>
#include <filesystem>
#include <istream>
#include <ostream>
#include <string>
#include <string_view>

#include "base/diagnostic.h"
#include "base/process_exit.h"
#include "compiler/repl_session.h"
#include "runtime/external_resources.h"
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
  compiler::ReplSession session(directory);
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
      WriteError(errors, diagnostic.source_path, diagnostic.location, diagnostic.message);
    }
    if (result.status == compiler::SubmissionStatus::RuntimeError) {
      vm::WriteRuntimeError(errors, result.execution);
    }
    pending.clear();
  }
}

}  // namespace vela::tooling
