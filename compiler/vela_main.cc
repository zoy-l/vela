#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include "base/diagnostic.h"
#include "base/file.h"
#include "base/path.h"
#include "base/source_location.h"
#include "compiler/build_info.h"
#include "compiler/compilation_cache.h"
#include "compiler/compiler.h"
#include "compiler/exit_code.h"
#include "compiler/frontend/checker.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/tooling/debug_console.h"
#include "compiler/tooling/documentation.h"
#include "compiler/tooling/formatter.h"
#include "compiler/tooling/language_server.h"
#include "compiler/tooling/repl_console.h"
#include "compiler/tooling/standard_streams.h"
#include "runtime/isolate.h"
#include "vm/bytecode_image.h"
#include "vm/execution_status.h"
#include "vm/program.h"
#include "vm/vm.h"

namespace {

using vela::compiler::kIoErrorExitCode;
using vela::compiler::kRuntimeErrorExitCode;
using vela::compiler::kSourceErrorExitCode;
using vela::compiler::kUsageExitCode;

void PrintUsage(std::ostream& output) {
  output << "Usage: vela <command> [arguments]\n"
         << "\n"
         << "Commands:\n"
         << "  --help       Show this message\n"
         << "  --version    Show the compiler version\n"
         << "  check FILE   Parse and statically check a Vela source file\n"
         << "  run FILE     Compile with a local content cache and run main\n"
         << "  compile FILE OUTPUT   Write a validated bytecode image\n"
         << "  exec IMAGE   Load a bytecode image and run main\n"
         << "  fmt [--write|--check] FILE   Format Vela source\n"
         << "  doc FILE OUTPUT   Generate exported API Markdown\n"
         << "  lsp          Run the language server over stdio\n"
         << "  debug FILE   Debug main with source breakpoints and stepping\n"
         << "  repl         Start a persistent interactive session\n";
}

void PrintDiagnostics(std::string_view path, const vela::frontend::Diagnostics& diagnostics) {
  for (const vela::frontend::Diagnostic& diagnostic : diagnostics) {
    const std::string_view source = diagnostic.source_path.empty() ? path : diagnostic.source_path;
    vela::WriteError(std::cerr, source, diagnostic.location, diagnostic.message);
  }
}

auto CheckFile(std::string_view path_text) -> int {
  const std::string path(path_text);
  const vela::frontend::CheckResult result = vela::frontend::CheckFile(path);
  PrintDiagnostics(path, result.diagnostics);
  if (!result.Succeeded()) {
    return kSourceErrorExitCode;
  }
  std::cout << "checked " << path << ": " << result.function_count << " function(s)\n";
  return 0;
}

enum class ExecutionMode : std::uint8_t { Run, Debug };

auto Execute(const vela::vm::Program& program,
             vela::runtime::Isolate& isolate,
             ExecutionMode mode = ExecutionMode::Run) -> int {
  const auto function = program.FindFunction("main");
  if (!function.IsValid()) {
    std::cerr << "vela: run requires a zero-argument main function\n";
    return kSourceErrorExitCode;
  }
  if (function.parameter_count() != 0) {
    std::cerr << "vela: run does not accept main function parameters yet\n";
    return kSourceErrorExitCode;
  }

  const auto result = mode == ExecutionMode::Debug
                          ? vela::tooling::RunDebugConsole(function, isolate, std::cin, std::cerr)
                          : vela::vm::VirtualMachine(isolate).Execute(program, function.index());
  if (result.status == vela::vm::ExecutionStatus::DebuggerStopped) {
    std::cerr << "debugger stopped\n";
    return 0;
  }
  if (!result.Succeeded()) {
    vela::vm::WriteRuntimeError(std::cerr, result);
    return kRuntimeErrorExitCode;
  }
  const auto exit_code = vela::compiler::ProcessExitCode(result.value);
  if (!exit_code) {
    std::cerr << "vela: error: " << exit_code.error() << '\n';
    return kRuntimeErrorExitCode;
  }
  return *exit_code;
}

auto RunFile(std::string_view path, ExecutionMode mode = ExecutionMode::Run) -> int {
  vela::runtime::Isolate isolate;
  const auto cached = vela::compiler::CompileFileCached(path, ".cache/vela", isolate);
  if (!cached) {
    vela::WriteError(std::cerr, path, vela::SourceLocation(), cached.error());
    return kIoErrorExitCode;
  }
  const auto& compiled = cached->compiled;
  PrintDiagnostics(path, compiled.diagnostics);
  return compiled.Succeeded() ? Execute(compiled.program, isolate, mode) : kSourceErrorExitCode;
}

auto CompileFile(std::string_view path, const std::filesystem::path& output) -> int {
  vela::runtime::Isolate isolate;
  const auto compiled = vela::compiler::CompileFile(path, isolate);
  PrintDiagnostics(path, compiled.diagnostics);
  if (!compiled.Succeeded()) {
    return kSourceErrorExitCode;
  }
  const auto image = vela::vm::SaveBytecode(compiled.program, isolate);
  if (!image) {
    vela::WriteError(std::cerr, path, vela::SourceLocation(), image.error().message);
    return kSourceErrorExitCode;
  }
  const auto written = vela::WriteFileAtomically(output, *image);
  if (!written) {
    vela::WriteError(std::cerr, vela::PathText(output), vela::SourceLocation(), written.error());
    return kIoErrorExitCode;
  }
  return 0;
}

auto ExecuteImage(std::string_view path) -> int {
  const auto bytes = vela::ReadBinaryFile(vela::Utf8Path(path));
  if (!bytes) {
    vela::WriteError(std::cerr, path, vela::SourceLocation(), bytes.error());
    return kIoErrorExitCode;
  }
  vela::runtime::Isolate isolate;
  const auto program = vela::vm::LoadBytecode(*bytes, isolate);
  if (!program) {
    vela::WriteError(std::cerr, path, vela::SourceLocation(), program.error().message);
    return kSourceErrorExitCode;
  }
  return Execute(*program, isolate);
}

enum class FormatMode : std::uint8_t { Output, Write, Check };

auto DocumentFile(std::string_view path, const std::filesystem::path& output) -> int {
  const auto documentation = vela::tooling::GenerateDocumentation(path);
  if (!documentation) {
    PrintDiagnostics(path, documentation.error());
    return kSourceErrorExitCode;
  }
  const auto written = vela::WriteFileAtomically(output, std::as_bytes(std::span(*documentation)));
  if (!written) {
    vela::WriteError(std::cerr, vela::PathText(output), vela::SourceLocation(), written.error());
    return kIoErrorExitCode;
  }
  return 0;
}

auto FormatFile(std::string_view path, FormatMode mode) -> int {
  const auto file = vela::Utf8Path(path);
  const auto bytes = vela::ReadBinaryFile(file);
  if (!bytes) {
    vela::WriteError(std::cerr, path, vela::SourceLocation(), bytes.error());
    return kIoErrorExitCode;
  }
  const void* storage = bytes->data();
  const std::string_view source(static_cast<const char*>(storage), bytes->size());
  const auto formatted = vela::tooling::FormatSource(source);
  if (!formatted) {
    PrintDiagnostics(path, formatted.error());
    return kSourceErrorExitCode;
  }
  if (mode == FormatMode::Check) {
    return *formatted == source ? 0 : 1;
  }
  if (mode == FormatMode::Write) {
    if (*formatted == source) {
      return 0;
    }
    const auto written = vela::WriteFileAtomically(file, std::as_bytes(std::span(*formatted)));
    if (!written) {
      vela::WriteError(std::cerr, path, vela::SourceLocation(), written.error());
      return kIoErrorExitCode;
    }
  } else {
    std::cout << *formatted;
  }
  return 0;
}

auto FormatCommand(std::span<char*> arguments) -> int {
  if (arguments.size() == 1) {
    return FormatFile(arguments.front(), FormatMode::Output);
  }
  if (arguments.size() == 2) {
    const std::string_view option = arguments.front();
    if (option == "--write" || option == "--check") {
      return FormatFile(arguments.subspan(1).front(),
                        option == "--write" ? FormatMode::Write : FormatMode::Check);
    }
  }
  std::cerr << "vela: fmt [--write|--check] requires one source file\n";
  return kUsageExitCode;
}

auto ReplCommand(std::span<char*> arguments) -> int {
  if (!arguments.empty()) {
    std::cerr << "vela: repl takes no arguments\n";
    return kUsageExitCode;
  }
  std::error_code error;
  const auto directory = std::filesystem::current_path(error);
  if (error) {
    std::cerr << "vela: cannot resolve working directory: " << error.message() << '\n';
    return kIoErrorExitCode;
  }
  return vela::tooling::RunRepl(directory,
                                {.input = std::cin, .output = std::cout, .errors = std::cerr});
}

auto LanguageServerCommand(std::span<char*> arguments) -> int {
  if (!arguments.empty()) {
    std::cerr << "vela: lsp takes no arguments\n";
    return kUsageExitCode;
  }
  const auto configured = vela::tooling::SetBinaryStandardStreams();
  if (!configured) {
    std::cerr << "vela: " << configured.error() << '\n';
    return kIoErrorExitCode;
  }
  return vela::tooling::RunLanguageServer(
      {.input = std::cin, .output = std::cout, .errors = std::cerr});
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const std::span<char*> arguments(argv, static_cast<std::size_t>(argc));
  if (argc == 1) {
    PrintUsage(std::cout);
    return kUsageExitCode;
  }
  const std::string_view command = arguments.subspan(1).front();
  if (command == "--help" || command == "-h") {
    PrintUsage(std::cout);
    return 0;
  }

  if (command == "--version") {
    std::cout << vela::kProductName << ' ' << vela::kVersion << '\n';
    return 0;
  }

  if (command == "check") {
    if (argc != 3) {
      std::cerr << "vela: check requires exactly one source file\n";
      PrintUsage(std::cerr);
      return kUsageExitCode;
    }
    return CheckFile(arguments.subspan(2).front());
  }

  if (command == "fmt") {
    return FormatCommand(arguments.subspan(2));
  }
  if (command == "lsp") {
    return LanguageServerCommand(arguments.subspan(2));
  }
  if (command == "repl") {
    return ReplCommand(arguments.subspan(2));
  }
  if (command == "compile" || command == "doc") {
    if (argc != 4) {
      std::cerr << "vela: " << command << " requires a source and output path\n";
      return kUsageExitCode;
    }
    const std::string_view source = arguments.subspan(2).front();
    const auto output = vela::Utf8Path(arguments.subspan(3).front());
    return command == "compile" ? CompileFile(source, output) : DocumentFile(source, output);
  }

  if (command == "run" || command == "exec" || command == "debug") {
    if (argc != 3) {
      std::cerr << "vela: " << command << " requires exactly one file\n";
      PrintUsage(std::cerr);
      return kUsageExitCode;
    }
    const std::string_view path = arguments.subspan(2).front();
    if (command == "exec") {
      return ExecuteImage(path);
    }
    return RunFile(path, command == "debug" ? ExecutionMode::Debug : ExecutionMode::Run);
  }

  std::cerr << "vela: unknown command\n";
  PrintUsage(std::cerr);
  return kUsageExitCode;
}
