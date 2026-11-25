#include "compiler/tooling/format/format_command.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <ostream>
#include <span>
#include <string_view>
#include <system_error>

#include "base/diagnostic.h"
#include "base/file.h"
#include "base/path.h"
#include "base/process_exit.h"
#include "base/source_location.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/tooling/format/formatter.h"
#include "compiler/tooling/source_files.h"

namespace vela::tooling {
namespace {

enum class FormatMode : std::uint8_t { Output, Write, Check };

int FormatFile(const std::filesystem::path& file, FormatMode mode, FormatStreams streams) {
  std::ostream& output = streams.output.get();
  std::ostream& errors = streams.errors.get();
  const auto path = PathText(file);
  const auto bytes = ReadBinaryFile(file);
  if (!bytes) {
    WriteError(errors, path, SourceLocation(), bytes.error());
    return kIoErrorExitCode;
  }
  const void* storage = bytes->data();
  const std::string_view source(static_cast<const char*>(storage), bytes->size());
  const auto formatted = FormatSource(source);
  if (!formatted) {
    frontend::WriteDiagnostics(errors, formatted.error(), path);
    return kSourceErrorExitCode;
  }
  const bool changed = *formatted != source;
  if (mode == FormatMode::Check) {
    if (changed) {
      output << path << '\n';
    }
    return changed ? 1 : 0;
  }
  if (mode == FormatMode::Write) {
    if (!changed) {
      return 0;
    }
    const auto written = WriteFileAtomically(file, std::as_bytes(std::span(*formatted)));
    if (!written) {
      WriteError(errors, path, SourceLocation(), written.error());
      return kIoErrorExitCode;
    }
    return 0;
  }
  output << *formatted;
  return 0;
}

int FormatPath(const std::filesystem::path& path, FormatMode mode, FormatStreams streams) {
  std::ostream& errors = streams.errors.get();
  std::error_code error;
  const auto status = std::filesystem::status(path, error);
  if (error) {
    WriteError(errors, PathText(path), SourceLocation(), error.message());
    return kIoErrorExitCode;
  }
  if (std::filesystem::is_regular_file(status)) {
    return FormatFile(path, mode, streams);
  }
  if (!std::filesystem::is_directory(status)) {
    WriteError(errors, PathText(path), SourceLocation(),
               "format target must be a regular file or directory");
    return kIoErrorExitCode;
  }
  if (mode == FormatMode::Output) {
    errors << "vela: formatting a directory requires --write or --check\n";
    return kUsageExitCode;
  }
  const auto sources = DirectorySources(path);
  if (!sources) {
    WriteError(errors, PathText(path), SourceLocation(), sources.error());
    return kIoErrorExitCode;
  }
  int result = 0;
  for (const auto& file : *sources) {
    // I/O、源码错误和格式差异的退出码依次为 74、65、1，汇总时保留最严重的失败。
    result = std::max(result, FormatFile(file, mode, streams));
  }
  return result;
}

}  // namespace

int FormatCommand(std::span<char*> arguments, FormatStreams streams) {
  if (arguments.size() == 1 && !std::string_view(arguments.front()).starts_with("--")) {
    return FormatPath(Utf8Path(arguments.front()), FormatMode::Output, streams);
  }
  if (arguments.size() == 2 && !std::string_view(arguments[1]).starts_with("--")) {
    const std::string_view option = arguments.front();
    if (option == "--write" || option == "--check") {
      return FormatPath(Utf8Path(arguments[1]),
                        option == "--write" ? FormatMode::Write : FormatMode::Check, streams);
    }
  }
  streams.errors.get() << "vela: fmt [--write|--check] requires one file or directory path\n";
  return kUsageExitCode;
}

}  // namespace vela::tooling
