#include "compiler/aot/build_command.h"

#include <cstddef>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "base/binary.h"
#include "base/diagnostic.h"
#include "base/file.h"
#include "base/fingerprint.h"
#include "base/path.h"
#include "base/source_location.h"
#include "compiler/aot/native_compiler.h"
#include "compiler/exit_code.h"
#include "compiler/frontend/source_loader.h"
#include "compiler/ir/preparation.h"

namespace vela::compiler::aot {

int BuildCommand(std::span<char*> arguments, std::ostream& errors) {
  NativeOptions options;
  std::string_view source;
  std::string_view output;
  std::string_view emit = "exe";
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const std::string_view argument = arguments.subspan(index).front();
    if (argument == "-o" && index + 1 < arguments.size()) {
      output = arguments.subspan(++index).front();
    } else if (argument.starts_with("--emit=")) {
      emit = argument.substr(std::string_view("--emit=").size());
    } else if (argument.starts_with("--export=")) {
      options.exports.emplace_back(argument.substr(std::string_view("--export=").size()));
    } else if (!argument.starts_with('-') && source.empty()) {
      source = argument;
    } else {
      errors << "vela: invalid build argument: " << argument << '\n';
      return kUsageExitCode;
    }
  }
  if (source.empty() || output.empty()) {
    errors << "vela: build requires a source file and -o OUTPUT\n";
    return kUsageExitCode;
  }
  if (emit != "obj") {
    errors << "vela: AOT output kind is not implemented: " << emit << '\n';
    return kUsageExitCode;
  }
  auto parsed = frontend::LoadProgram(source);
  BinaryWriter identity;
  for (const auto& file : parsed.program.sources) {
    identity.Write(file.path);
    identity.Write(file.text);
  }
  options.module_name += '_' + ContentFingerprint(identity.bytes()).Hex();
  auto prepared = ir::PrepareParsed(std::move(parsed));
  if (!prepared) {
    for (const auto& diagnostic : prepared.error()) {
      WriteError(errors, diagnostic.source_path, diagnostic.location, diagnostic.message);
    }
    return kSourceErrorExitCode;
  }
  auto artifact = Compile(*prepared, options);
  if (!artifact) {
    for (const auto& diagnostic : artifact.error()) {
      WriteError(errors, diagnostic.source_path, diagnostic.location, diagnostic.message);
    }
    return kSourceErrorExitCode;
  }
  const auto written = WriteFileAtomically(Utf8Path(output), artifact->object);
  if (!written) {
    WriteError(errors, output, SourceLocation(), written.error());
    return kIoErrorExitCode;
  }
  return 0;
}

}  // namespace vela::compiler::aot
