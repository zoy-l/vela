#include "compiler/aot/build_command.h"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/binary.h"
#include "base/diagnostic.h"
#include "base/file.h"
#include "base/fingerprint.h"
#include "base/path.h"
#include "base/process_exit.h"
#include "base/source_location.h"
#include "compiler/aot/artifact_publication.h"
#include "compiler/aot/native_compiler.h"
#include "compiler/aot/target_support.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/source_loader.h"
#include "compiler/ir/preparation.h"
#include "compiler/project_config.h"

namespace vela::compiler::aot {
namespace {

constexpr std::string_view kEmitOption = "--emit=";
constexpr std::string_view kExportOption = "--export=";
constexpr std::string_view kTargetSupportOption = "--target-support=";
constexpr std::string_view kExecutableFormat = "exe";

struct BuildArguments final {
  NativeOptions options;
  std::string_view source;
  std::string_view output;
  NativeOutputKind kind = NativeOutputKind::Executable;
  std::filesystem::path support_path = DefaultTargetSupportPath();
};

std::expected<BuildArguments, std::string> ParseArguments(std::span<char*> arguments) {
  BuildArguments result;
  std::string_view emit = kExecutableFormat;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const std::string_view argument = arguments.subspan(index).front();
    if (argument == "-o" && index + 1 < arguments.size()) {
      result.output = arguments.subspan(++index).front();
    } else if (argument.starts_with(kEmitOption)) {
      emit = argument.substr(kEmitOption.size());
    } else if (argument.starts_with(kExportOption)) {
      result.options.exports.emplace_back(argument.substr(kExportOption.size()));
    } else if (argument.starts_with(kTargetSupportOption)) {
      result.support_path = Utf8Path(argument.substr(kTargetSupportOption.size()));
    } else if (!argument.starts_with('-') && result.source.empty()) {
      result.source = argument;
    } else {
      return std::unexpected("invalid build argument: " + std::string(argument));
    }
  }
  if (result.source.empty() || result.output.empty()) {
    return std::unexpected("build requires a source file and -o OUTPUT");
  }
  if (emit == "obj") {
    result.kind = NativeOutputKind::Object;
  } else if (emit == "staticlib") {
    result.kind = NativeOutputKind::StaticLibrary;
  } else if (emit != kExecutableFormat) {
    return std::unexpected("unsupported AOT output kind: " + std::string(emit));
  }
  result.options.entry_mode = result.kind == NativeOutputKind::Executable
                                  ? NativeEntryMode::Executable
                                  : NativeEntryMode::Library;
  return result;
}

void PrintDiagnostics(std::ostream& errors, const frontend::Diagnostics& diagnostics) {
  for (const auto& diagnostic : diagnostics) {
    frontend::WriteDiagnostic(errors, diagnostic);
  }
}

std::expected<void, std::string> PublishBuild(const BuildArguments& arguments,
                                              const NativeArtifact& artifact,
                                              std::span<const std::byte> binary,
                                              const TargetSupport* support) {
  const auto output = Utf8Path(arguments.output);
  if (arguments.kind == NativeOutputKind::Executable) {
    return WriteFileAtomically(output, binary, FileMode::kExecutable);
  }
  const auto kind = arguments.kind == NativeOutputKind::Object ? LibraryArtifactKind::Object
                                                               : LibraryArtifactKind::StaticLibrary;
  return PublishNativeLibrary(output, artifact, binary, kind, support);
}

int ExecuteBuild(BuildArguments arguments, std::ostream& errors) {
  const auto kind = arguments.kind;
  std::optional<TargetSupport> support;
  const TargetSupport* target_support = nullptr;
  if (kind != NativeOutputKind::Object) {
    auto loaded = LoadTargetSupport(arguments.support_path, kind);
    if (!loaded) {
      errors << "vela: " << loaded.error() << '\n';
      return kIoErrorExitCode;
    }
    target_support = &support.emplace(std::move(*loaded));
  }
  auto parsed = frontend::LoadProgram(arguments.source);
  BinaryWriter identity;
  for (const auto& file : parsed.program.sources) {
    identity.Write(file.path);
    identity.Write(file.text);
  }
  auto& options = arguments.options;
  options.module_name += '_' + ContentFingerprint(identity.bytes()).Hex();
  const auto diagnostics = LoadSourceDiagnosticOptions(Utf8Path(arguments.source));
  if (!diagnostics) {
    frontend::WriteDiagnostic(errors, ConfigurationDiagnostic(diagnostics.error()));
    return diagnostics.error().exit_code;
  }
  auto prepared = ir::PrepareParsed(std::move(parsed), *diagnostics);
  PrintDiagnostics(errors, prepared.diagnostics);
  if (!prepared.Succeeded()) {
    return kSourceErrorExitCode;
  }
  auto artifact = Compile(std::move(prepared.program), options);
  if (!artifact) {
    PrintDiagnostics(errors, artifact.error());
    return kSourceErrorExitCode;
  }
  auto binary =
      kind == NativeOutputKind::Object
          ? std::expected<std::vector<std::byte>, std::string>(std::move(artifact->object))
          : LinkNativeArtifact(*target_support, artifact->object, kind);
  if (!binary) {
    errors << "vela: " << binary.error() << '\n';
    return kIoErrorExitCode;
  }
  const auto written = PublishBuild(arguments, *artifact, *binary, target_support);
  if (!written) {
    WriteError(errors, arguments.output, SourceLocation(), written.error());
    return kIoErrorExitCode;
  }
  return 0;
}

}  // namespace

int BuildCommand(std::span<char*> arguments, std::ostream& errors) {
  constexpr std::string_view kVerify = "--verify-manifest=";
  if (arguments.size() == 1 && std::string_view(arguments.front()).starts_with(kVerify)) {
    const auto verified =
        VerifyNativeLibrary(Utf8Path(std::string_view(arguments.front()).substr(kVerify.size())));
    if (!verified) {
      errors << "vela: " << verified.error() << '\n';
      return kSourceErrorExitCode;
    }
    return 0;
  }
  auto parsed = ParseArguments(arguments);
  if (!parsed) {
    errors << "vela: " << parsed.error() << '\n';
    return kUsageExitCode;
  }
  return ExecuteBuild(std::move(*parsed), errors);
}

}  // namespace vela::compiler::aot
