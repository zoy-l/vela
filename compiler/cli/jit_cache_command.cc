#include "compiler/cli/jit_cache_command.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <yyjson.h>

#include "base/command_line.h"
#include "base/json.h"
#include "base/path.h"
#include "base/process_exit.h"
#include "base/streams.h"
#include "compiler/development/development_files.h"
#include "compiler/development/source_session.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/jit/compiler.h"
#include "compiler/jit/function_compiler.h"
#include "compiler/native/compile_options.h"
#include "compiler/project/project_config.h"

namespace vela::tooling {

int JitCacheCommand(std::span<char*> arguments, Streams streams) {
  auto& output = streams.output.get();
  auto& errors = streams.errors.get();
  if (arguments.empty() || std::string_view(arguments.front()).starts_with("--")) {
    errors << "vela: jit-cache requires FILE [--config=FILE] --code-cache=DIR\n";
    return kUsageExitCode;
  }
  std::optional<std::filesystem::path> config;
  compiler::jit::CompileOptions options;
  options.optimization = compiler::NativeOptimization::Baseline;
  for (const std::string_view argument : arguments.subspan(1)) {
    if (auto path = ReadPathOption(argument, compiler::kConfigurationOption)) {
      config = std::move(path);
    } else if (auto cache = ReadPathOption(argument, compiler::jit::kCodeCacheOption)) {
      options.code_cache = std::move(cache);
    } else {
      errors << "vela: unknown jit-cache option: " << argument << '\n';
      return kUsageExitCode;
    }
  }
  if (!options.code_cache) {
    errors << "vela: jit-cache requires --code-cache=DIR\n";
    return kUsageExitCode;
  }
  const auto configuration =
      compiler::ReadSourceConfiguration(Utf8Path(arguments.front()), errors, config);
  if (!configuration) {
    return configuration.error().exit_code;
  }
  compiler::DevelopmentFiles files(arguments.front(), configuration->imports);
  auto update = files.Capture({});
  if (!update) {
    frontend::WriteDiagnostics(errors, update.error(), arguments.front());
    return kSourceErrorExitCode;
  }
  compiler::SourceSession sources(files.entry(), configuration->diagnostics, {},
                                  configuration->imports);
  auto source = sources.Prepare(std::move(*update));
  if (!source) {
    frontend::WriteDiagnostics(errors, source.error().diagnostics, files.entry());
    return kSourceErrorExitCode;
  }
  const auto& snapshot = source->snapshot();
  frontend::WriteDiagnostics(errors, snapshot.diagnostics, files.entry());
  auto functions = compiler::jit::PrepareFunctions(snapshot.program, options);
  if (!functions) {
    frontend::WriteDiagnostics(errors, functions.error(), files.entry());
    return kSourceErrorExitCode;
  }
  const auto count = (*functions)->symbols.size();
  for (std::size_t index = 0; index < count; ++index) {
    auto compiled = (*functions)->Compile(index);
    if (!compiled) {
      errors << "vela: jit-cache: " << compiled.error() << '\n';
      return kRuntimeErrorExitCode;
    }
  }
  std::vector<std::string> paths;
  for (const auto& [path, text] : snapshot.sources) {
    paths.push_back(path);
  }
  std::ranges::sort(paths);
  JsonOutput result;
  auto* document = result.document();
  auto* root = result.root();
  auto* inputs = yyjson_mut_arr(document);
  for (const auto& path : paths) {
    yyjson_mut_arr_add_strncpy(document, inputs, path.data(), path.size());
  }
  AddJsonField(document, root, "sources", inputs);
  AddJsonField(document, root, "functions", yyjson_mut_uint(document, count));
  auto json = result.Serialize();
  if (!json) {
    errors << "vela: jit-cache: " << json.error().message << '\n';
    return kIoErrorExitCode;
  }
  output << *json << '\n';
  return 0;
}

}  // namespace vela::tooling
