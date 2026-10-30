#include "compiler/tooling/mock_generator.h"

#include <expected>
#include <filesystem>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "base/file.h"
#include "base/path.h"
#include "base/process_exit.h"
#include "compiler/frontend/checker.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/source_loader.h"
#include "compiler/project_config.h"

namespace vela::tooling {
namespace {

struct MockOptions final {
  std::string input;
  std::string output;
  std::vector<std::string> interfaces;
  bool check = false;
};

std::expected<std::string_view, std::string> OptionValue(std::span<char*>& arguments,
                                                         std::string_view option) {
  if (arguments.empty()) {
    return std::unexpected(std::string(option) + " requires a value");
  }
  const std::string_view value = arguments.front();
  arguments = arguments.subspan(1);
  if (value.empty() || value.starts_with('-')) {
    return std::unexpected(std::string(option) + " requires a value");
  }
  return value;
}

std::expected<MockOptions, std::string> ParseOptions(std::span<char*> arguments) {
  MockOptions options;
  while (!arguments.empty()) {
    const std::string_view argument = arguments.front();
    arguments = arguments.subspan(1);
    if (argument == "--check") {
      if (options.check) {
        return std::unexpected("duplicate --check");
      }
      options.check = true;
      continue;
    }
    if (argument == "--interface" || argument == "-o") {
      const auto value = OptionValue(arguments, argument);
      if (!value) {
        return std::unexpected(value.error());
      }
      if (argument == "--interface") {
        options.interfaces.emplace_back(*value);
        continue;
      }
      if (!options.output.empty()) {
        return std::unexpected("duplicate output path");
      }
      options.output = *value;
      continue;
    }
    if (argument.starts_with('-')) {
      return std::unexpected("unknown mock option '" + std::string(argument) + "'");
    }
    if (!options.input.empty()) {
      return std::unexpected("mock accepts exactly one API file");
    }
    options.input = argument;
  }
  if (options.input.empty() || options.output.empty() || options.interfaces.empty()) {
    return std::unexpected(
        "usage: vela mock API_FILE --interface NAME [--interface OTHER] -o OUTPUT [--check]");
  }
  if (Utf8Path(options.output).extension() != ".vela") {
    return std::unexpected("mock output must be a .vela source file");
  }
  return options;
}

void Diagnostics(std::ostream& errors, const frontend::Diagnostics& diagnostics) {
  for (const auto& diagnostic : diagnostics) {
    frontend::WriteDiagnostic(errors, diagnostic);
  }
}

int DiagnosticExit(const frontend::Diagnostics& diagnostics) {
  for (const auto& diagnostic : diagnostics) {
    if (diagnostic.kind == frontend::DiagnosticKind::Io) {
      return kIoErrorExitCode;
    }
    if (diagnostic.kind == frontend::DiagnosticKind::Internal) {
      return kRuntimeErrorExitCode;
    }
  }
  return kSourceErrorExitCode;
}

int Publish(const std::filesystem::path& path,
            std::string_view content,
            const MockOptions& options,
            std::ostream& output,
            std::ostream& errors) {
  std::error_code error;
  const bool exists = std::filesystem::exists(path, error);
  if (error) {
    errors << "vela mock: " << error.message() << '\n';
    return kIoErrorExitCode;
  }
  if (exists) {
    const auto existing = ReadBinaryFile(path);
    if (!existing) {
      errors << "vela mock: " << existing.error() << '\n';
      return kIoErrorExitCode;
    }
    const void* data = existing->data();
    if (std::string_view(static_cast<const char*>(data), existing->size()) == content) {
      return 0;
    }
  }
  if (options.check) {
    errors << "vela mock: generated output is " << (exists ? "out of date: " : "missing: ")
           << options.output << '\n';
    return 1;
  }
  const auto written = WriteFileAtomically(path, std::as_bytes(std::span(content)));
  if (!written) {
    errors << "vela mock: " << written.error() << '\n';
    return kIoErrorExitCode;
  }
  output << "generated " << options.output << '\n';
  return 0;
}

}  // namespace

int MockCommand(std::span<char*> arguments, std::ostream& output, std::ostream& errors) {
  const auto options = ParseOptions(arguments);
  if (!options) {
    errors << "vela mock: " << options.error() << '\n';
    return kUsageExitCode;
  }
  std::error_code error;
  const auto absolute = std::filesystem::absolute(Utf8Path(options->output), error);
  if (error) {
    errors << "vela mock: " << error.message() << '\n';
    return kIoErrorExitCode;
  }
  const auto destination = std::filesystem::weakly_canonical(absolute, error);
  if (error) {
    errors << "vela mock: " << error.message() << '\n';
    return kIoErrorExitCode;
  }
  frontend::ModuleCache cache;
  auto parsed = frontend::LoadProgram(options->input, cache, {});
  if (!parsed.diagnostics.empty()) {
    Diagnostics(errors, parsed.diagnostics);
    return DiagnosticExit(parsed.diagnostics);
  }
  const auto& program = parsed.program;
  const bool output_exists = std::filesystem::exists(destination, error);
  if (error) {
    errors << "vela mock: " << error.message() << '\n';
    return kIoErrorExitCode;
  }
  for (const auto& source : program.sources) {
    if (source.path.starts_with("sdk:") || source.path.starts_with("builtin:")) {
      continue;
    }
    const bool same =
        output_exists && std::filesystem::equivalent(Utf8Path(source.path), destination, error);
    if (error) {
      errors << "vela mock: " << error.message() << '\n';
      return kIoErrorExitCode;
    }
    if (same || Utf8Path(source.path) == destination) {
      errors << "vela mock: output cannot replace the API file or its dependencies\n";
      return kUsageExitCode;
    }
  }
  // 同一个不可变模块缓存分别提供检查用 AST 和保留公开别名的生成用 AST。
  const auto diagnostics = compiler::LoadSourceDiagnosticOptions(Utf8Path(options->input));
  if (!diagnostics) {
    frontend::WriteDiagnostic(errors, compiler::ConfigurationDiagnostic(diagnostics.error()));
    return diagnostics.error().exit_code;
  }
  const auto checked =
      frontend::CheckParsed(frontend::LoadProgram(options->input, cache, {}), *diagnostics);
  Diagnostics(errors, checked.diagnostics);
  if (!checked.Succeeded()) {
    return DiagnosticExit(checked.diagnostics);
  }
  const auto generated = GenerateMocks(program, options->interfaces, destination);
  if (!generated) {
    Diagnostics(errors, generated.error());
    return DiagnosticExit(generated.error());
  }
  const auto output_path = PathText(destination);
  const frontend::SourceInput overlay{.path = output_path, .text = *generated};
  const auto artifact =
      frontend::CheckParsed(frontend::LoadProgram(output_path, cache, {&overlay, 1}), *diagnostics);
  if (!artifact.Succeeded()) {
    Diagnostics(errors, artifact.diagnostics);
    return DiagnosticExit(artifact.diagnostics) == kIoErrorExitCode ? kIoErrorExitCode
                                                                    : kRuntimeErrorExitCode;
  }
  return Publish(destination, *generated, *options, output, errors);
}

}  // namespace vela::tooling
