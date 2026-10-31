#include "compiler/test_support/program_test_support.h"

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "compiler/frontend/diagnostic.h"
#include "compiler/frontend/modules/source_loader.h"
#include "compiler/jit/compiler.h"
#include "compiler/jit/source_compiler.h"
#include "runtime/async/future.h"
#include "runtime/async/future_operations.h"
#include "runtime/heap/heap.h"
#include "runtime/native/native_execution.h"
#include "runtime/native/native_instance.h"

namespace vela::compiler::test {
namespace {

std::expected<std::unique_ptr<runtime::NativeInstance>, std::string> Clean(InstanceResult result) {
  if (!result.Succeeded() || !result.diagnostics.empty()) {
    return std::unexpected(frontend::DiagnosticsText(result.diagnostics));
  }
  return std::move(result.instance);
}

}  // namespace

InstanceResult CompileParsed(frontend::ParseResult parsed,
                               runtime::Isolate& isolate,
                               frontend::DiagnosticOptions options,
                               jit::CompileOptions compilation) {
  auto compiler = jit::Compiler::Create();
  if (!compiler) {
    return {.instance = {}, .diagnostics = std::move(compiler.error())};
  }
  auto compiled = jit::CompileParsed(**compiler, std::move(parsed),
                                     {.diagnostics = options, .compilation = std::move(compilation)});
  InstanceResult result{.instance = {}, .diagnostics = std::move(compiled.diagnostics)};
  if (!compiled.Succeeded()) {
    return result;
  }
  auto instance = runtime::NativeInstance::Instantiate(isolate, std::move(compiled.code.image));
  if (!instance) {
    result.diagnostics.push_back({.location = {}, .message = instance.error().message, .source_path = {}});
  } else {
    result.instance = std::move(*instance);
  }
  return result;
}

InstanceResult CompileSource(std::string_view source,
                               runtime::Isolate& isolate,
                               frontend::DiagnosticOptions options) {
  const frontend::SourceInput input{.path = "main.vela", .text = source};
  return CompileModules(std::span(&input, 1), input.path, isolate, options);
}

InstanceResult CompileDebugSource(std::string_view source, runtime::Isolate& isolate) {
  const frontend::SourceInput input{.path = "main.vela", .text = source};
  return CompileParsed(frontend::LoadSources(std::span(&input, 1), input.path), isolate, {},
                        {.debugging = native::NativeDebugging::Instructions});
}

InstanceResult CompileModules(std::span<const frontend::SourceInput> sources,
                                std::string_view entry,
                                runtime::Isolate& isolate,
                                frontend::DiagnosticOptions options) {
  return CompileParsed(frontend::LoadSources(sources, entry), isolate, options);
}

InstanceResult CompileFile(std::string_view path,
                             runtime::Isolate& isolate,
                             frontend::DiagnosticOptions options) {
  return CompileParsed(frontend::LoadProgram(path), isolate, options);
}

std::expected<std::unique_ptr<runtime::NativeInstance>, std::string> CompileCleanSource(
    std::string_view source, runtime::Isolate& isolate) {
  return Clean(CompileSource(source, isolate));
}

std::expected<std::unique_ptr<runtime::NativeInstance>, std::string> CompileCleanModules(
    std::span<const frontend::SourceInput> sources, std::string_view entry, runtime::Isolate& isolate) {
  return Clean(CompileModules(sources, entry, isolate));
}

std::expected<std::unique_ptr<runtime::NativeInstance>, std::string> ReinstantiateSource(
    std::string_view source, runtime::Isolate& isolate) {
  jit::SourceResult compiled;
  {
    auto compiler = jit::Compiler::Create();
    if (!compiler) {
      return std::unexpected(frontend::DiagnosticsText(compiler.error()));
    }
    compiled = jit::CompileSource(**compiler, source);
  }
  if (!compiled.Succeeded()) {
    return std::unexpected(frontend::DiagnosticsText(compiled.diagnostics));
  }
  {
    runtime::Isolate producer(1);
    const auto instance = runtime::NativeInstance::Instantiate(producer, compiled.code.image);
    if (!instance) {
      return std::unexpected(instance.error().message);
    }
  }
  auto instance = runtime::NativeInstance::Instantiate(isolate, std::move(compiled.code.image));
  if (!instance) {
    return std::unexpected(instance.error().message);
  }
  isolate.heap().Collect();
  return std::move(*instance);
}

runtime::ExecutionResult Execute(runtime::NativeInstance& instance, std::string_view entry) {
  const auto function = instance.FindFunction(entry);
  if (!function) {
    return {.status = ExecutionStatus::InvalidEntry, .stack_trace = {},
            .message = "test source is missing entry: " + std::string(entry), .origin = {}};
  }
  return runtime::ExecuteNativeFunction(instance, *function);
}

runtime::Future StartFuture(runtime::NativeInstance& instance, std::string_view entry) {
  auto& isolate = instance.isolate();
  const auto result = Execute(instance, entry);
  if (result.Succeeded()) {
    return runtime::RootFutureValue(isolate, result.value);
  }
  runtime::Future future(&isolate.scheduler());
  future.AttachHeap(isolate.heap());
  runtime::CompleteFutureError(isolate, future, result.value,
                                {.message = result.message, .stack_trace = result.stack_trace,
                                 .status = result.status, .origin = result.origin});
  return future;
}

std::expected<std::int64_t, std::string> ExecuteInteger(runtime::NativeInstance& instance) {
  const auto result = Execute(instance);
  if (!result.Succeeded()) {
    return std::unexpected(result.message);
  }
  if (!result.value.IsInteger()) {
    return std::unexpected("test entry must return int");
  }
  return result.value.AsInteger();
}

std::expected<std::int64_t, std::string> AwaitInteger(runtime::Future& future,
                                                      runtime::Isolate& isolate) {
  if (!isolate.scheduler().PumpUntil(future)) {
    return std::unexpected("test Future has no producer");
  }
  if (future.state() != runtime::FutureState::Succeeded) {
    return std::unexpected(future.error_diagnostic().value_or(runtime::FutureDiagnostic{}).message);
  }
  const auto value = future.value().value_or(runtime::Value::Null());
  if (!value.IsInteger()) {
    return std::unexpected("test Future must complete with int");
  }
  return value.AsInteger();
}

std::expected<std::int64_t, std::string> ExecuteFutureInteger(runtime::NativeInstance& instance) {
  auto future = StartFuture(instance);
  return AwaitInteger(future, instance.isolate());
}

std::expected<std::array<std::int64_t, 2>, std::string> ExecuteRepeated(
    runtime::NativeInstance& instance) {
  const auto first = ExecuteInteger(instance);
  if (!first) {
    return std::unexpected(first.error());
  }
  instance.isolate().heap().Collect();
  const auto second = ExecuteInteger(instance);
  if (!second) {
    return std::unexpected(second.error());
  }
  return std::array{*first, *second};
}

std::expected<std::int64_t, std::string> RunInteger(std::string_view source,
                                                    runtime::Isolate& isolate) {
  const auto instance = CompileCleanSource(source, isolate);
  if (!instance) {
    return std::unexpected(instance.error());
  }
  return ExecuteInteger(**instance);
}

}  // namespace vela::compiler::test
