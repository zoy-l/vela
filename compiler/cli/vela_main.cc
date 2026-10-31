#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "base/diagnostic.h"
#include "base/file.h"
#include "base/native_program_mode.h"
#include "base/path.h"
#include "base/process_exit.h"
#include "base/source_location.h"
#include "compiler/aot/build_command.h"
#include "compiler/build_info.h"
#include "compiler/cli/debug_console.h"
#include "compiler/cli/development_console.h"
#include "compiler/cli/repl_console.h"
#include "compiler/cli/standard_streams.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/typing/checker.h"
#include "compiler/jit/compiler.h"
#include "compiler/jit/source_compiler.h"
#include "compiler/native/native_roots.h"
#include "compiler/project/project_config.h"
#include "compiler/testing/command.h"
#include "compiler/tooling/documentation/documentation.h"
#include "compiler/tooling/format/formatter.h"
#include "compiler/tooling/lsp/language_server.h"
#include "compiler/tooling/mock/mock_generator.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_execution.h"
#include "runtime/native/native_image.h"
#include "runtime/native/native_instance.h"
#include "runtime/process/process_entry.h"
#include "runtime/value/value.h"

namespace {

using vela::kIoErrorExitCode;
using vela::kRuntimeErrorExitCode;
using vela::kSourceErrorExitCode;
using vela::kUsageExitCode;

void PrintUsage(std::ostream& output) {
  output
      << "Usage: vela <command> [arguments]\n"
      << "\n"
      << "Commands:\n"
      << "  --help       Show this message\n"
      << "  --version    Show the compiler version\n"
      << "  check FILE   Parse and statically check a Vela source file\n"
      << "  run FILE     Compile native code with a local cache and run main\n"
      << "  test [PATH...] [OPTIONS]   Run configured source test suites\n"
      << "  mock API_FILE --interface NAME -o OUTPUT [--check]   Generate interface mocks\n"
      << "  dev FILE [--optimized] [--code-cache=DIR]   Run a JIT development session\n"
      << "  build FILE [--emit=exe|obj|staticlib] [--export=NAME] -o OUTPUT   Build native code\n"
      << "  build --verify-manifest=FILE   Verify a native library and its header\n"
      << "  fmt [--write|--check] FILE   Format Vela source\n"
      << "  doc FILE OUTPUT   Generate exported API Markdown\n"
      << "  lsp          Run the language server over stdio\n"
      << "  debug FILE   Debug main with source breakpoints and stepping\n"
      << "  repl         Start a persistent interactive session\n";
}

void PrintDiagnostics(std::string_view path, const vela::frontend::Diagnostics& diagnostics) {
  for (const vela::frontend::Diagnostic& diagnostic : diagnostics) {
    vela::frontend::WriteDiagnostic(std::cerr, diagnostic, path);
  }
}

std::expected<vela::frontend::DiagnosticOptions, vela::compiler::ConfigurationError>
ReadDiagnosticOptions(std::string_view path) {
  auto options = vela::compiler::LoadSourceDiagnosticOptions(vela::Utf8Path(path));
  if (!options) {
    vela::frontend::WriteDiagnostic(std::cerr,
                                    vela::compiler::ConfigurationDiagnostic(options.error()));
  }
  return options;
}

int CheckFile(std::string_view path_text) {
  const std::string path(path_text);
  const auto options = ReadDiagnosticOptions(path);
  if (!options) {
    return options.error().exit_code;
  }
  const vela::frontend::CheckResult result = vela::frontend::CheckFile(path, {}, *options);
  PrintDiagnostics(path, result.diagnostics);
  if (!result.Succeeded()) {
    return kSourceErrorExitCode;
  }
  std::cout << "checked " << path << ": " << result.function_count << " function(s)\n";
  return 0;
}

enum class ExecutionMode : std::uint8_t { Run, Debug };

int RunFile(std::string_view path, ExecutionMode mode = ExecutionMode::Run) {
  const auto diagnostics = ReadDiagnosticOptions(path);
  if (!diagnostics) {
    return diagnostics.error().exit_code;
  }
  auto compiler = vela::compiler::jit::Compiler::Create();
  if (!compiler) {
    PrintDiagnostics(path, compiler.error());
    return kRuntimeErrorExitCode;
  }
  vela::compiler::jit::SourceOptions options;
  options.diagnostics = *diagnostics;
  options.compilation.code_cache = ".cache/vela/native/run";
  if (mode == ExecutionMode::Debug) {
    options.compilation.debugging = vela::compiler::native::NativeDebugging::Instructions;
  }
  const auto compiled = vela::compiler::jit::CompileFile(**compiler, path, options);
  PrintDiagnostics(path, compiled.diagnostics);
  if (!compiled.Succeeded()) {
    return kSourceErrorExitCode;
  }
  vela::runtime::Isolate isolate;
  auto instance = vela::runtime::NativeInstance::Instantiate(isolate, compiled.code.image);
  if (!instance) {
    std::cerr << "vela: " << instance.error().message << '\n';
    return kSourceErrorExitCode;
  }
  auto& program = **instance;
  const auto main = program.FindFunction("main");
  if (!main || program.image().functions.values().subspan(*main).front().parameter_count != 0) {
    std::cerr << "vela: run requires a zero-argument main function\n";
    return kSourceErrorExitCode;
  }
  if (mode == ExecutionMode::Debug) {
    return vela::tooling::RunDebugConsole(program, *main, std::cin, std::cerr);
  }
  if (vela::testing::HasTestModule(program)) {
    return vela::testing::RunDirectSuite(program, path,
                                         {.output = std::cout, .errors = std::cerr});
  }
  vela::runtime::ProcessEntry process(isolate, std::cerr);
  const auto result = vela::runtime::ExecuteNativeFunction(program, *main);
  if (!result.Succeeded()) {
    vela::runtime::WriteRuntimeDiagnostic(std::cerr, {
        .message = result.message, .stack_trace = result.stack_trace,
        .status = result.status, .origin = result.origin,
    });
    return kRuntimeErrorExitCode;
  }
  return process.Finish(result.value);
}

enum class FormatMode : std::uint8_t { Output, Write, Check };

int DocumentFile(std::string_view path, const std::filesystem::path& output) {
  const auto options = vela::compiler::LoadSourceDiagnosticOptions(vela::Utf8Path(path));
  if (!options) {
    vela::frontend::WriteDiagnostic(std::cerr,
                                    vela::compiler::ConfigurationDiagnostic(options.error()));
    return options.error().exit_code;
  }
  const auto documentation = vela::tooling::GenerateDocumentation(path, *options);
  PrintDiagnostics(path, documentation.diagnostics);
  if (!documentation.Succeeded()) {
    return kSourceErrorExitCode;
  }
  const auto written =
      vela::WriteFileAtomically(output, std::as_bytes(std::span(documentation.markdown)));
  if (!written) {
    vela::WriteError(std::cerr, vela::PathText(output), vela::SourceLocation(), written.error());
    return kIoErrorExitCode;
  }
  return 0;
}

int FormatFile(std::string_view path, FormatMode mode) {
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

int FormatCommand(std::span<char*> arguments) {
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

int ReplCommand(std::span<char*> arguments) {
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

int CheckCommand(std::span<char*> arguments) {
  if (arguments.size() != 1) {
    std::cerr << "vela: check requires exactly one source file\n";
    PrintUsage(std::cerr);
    return kUsageExitCode;
  }
  return CheckFile(arguments.front());
}

int DevelopmentCommand(std::span<char*> arguments) {
  if (arguments.empty() || std::string_view(arguments.front()).starts_with("--")) {
    std::cerr << "vela: dev requires a source file\n";
    return kUsageExitCode;
  }
  vela::compiler::jit::CompileOptions options;
  constexpr std::string_view kCodeCache = "--code-cache=";
  for (const std::string_view argument : arguments.subspan(1)) {
    if (argument == "--optimized") {
      options.tier = vela::NativeCodeTier::Optimized;
    } else if (argument.starts_with(kCodeCache) && argument.size() > kCodeCache.size()) {
      options.code_cache = vela::Utf8Path(argument.substr(kCodeCache.size()));
    } else {
      std::cerr << "vela: unknown development option: " << argument << '\n';
      return kUsageExitCode;
    }
  }
  return vela::tooling::RunDevelopmentConsole(
      arguments.front(), {.input = std::cin, .output = std::cout, .errors = std::cerr},
      std::move(options));
}

int LanguageServerCommand(std::span<char*> arguments) {
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

int TestCommand(std::span<char*> arguments) {
  std::vector<std::string_view> options;
  options.reserve(arguments.size());
  for (const auto* argument : arguments) {
    options.emplace_back(argument);
  }
  return vela::testing::TestCommand(options, {.output = std::cout, .errors = std::cerr});
}

int WriteArtifactCommand(std::string_view command, std::span<char*> arguments) {
  if (arguments.size() != 2) {
    std::cerr << "vela: " << command << " requires a source and output path\n";
    return kUsageExitCode;
  }
  const std::string_view source = arguments.front();
  const auto output = vela::Utf8Path(arguments.subspan(1).front());
  return DocumentFile(source, output);
}

}  // namespace

int main(int argc, char** argv) {
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
    return CheckCommand(arguments.subspan(2));
  }
  if (command == "test") {
    return TestCommand(arguments.subspan(2));
  }
  if (command == "mock") {
    return vela::tooling::MockCommand(arguments.subspan(2), std::cout, std::cerr);
  }

  if (command == "fmt") {
    return FormatCommand(arguments.subspan(2));
  }
  if (command == "build") {
    return vela::compiler::aot::BuildCommand(arguments.subspan(2), std::cerr);
  }
  if (command == "lsp") {
    return LanguageServerCommand(arguments.subspan(2));
  }
  if (command == "repl") {
    return ReplCommand(arguments.subspan(2));
  }
  if (command == "dev") {
    return DevelopmentCommand(arguments.subspan(2));
  }
  if (command == "doc") {
    return WriteArtifactCommand(command, arguments.subspan(2));
  }

  if (command == "run" || command == "debug") {
    if (argc != 3) {
      std::cerr << "vela: " << command << " requires exactly one file\n";
      PrintUsage(std::cerr);
      return kUsageExitCode;
    }
    const std::string_view path = arguments.subspan(2).front();
    return RunFile(path, command == "debug" ? ExecutionMode::Debug : ExecutionMode::Run);
  }

  std::cerr << "vela: unknown command\n";
  PrintUsage(std::cerr);
  return kUsageExitCode;
}
