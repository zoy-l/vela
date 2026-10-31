#include "compiler/program_test_support.h"

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

#include "compiler/compiler.h"
#include "compiler/frontend/diagnostic.h"
#include "vm/bytecode_image.h"
#include "vm/vm.h"

namespace vela::compiler::test {
namespace {

constexpr std::string_view kEntryPoint = "main";

std::expected<std::int64_t, std::string> InvokeInteger(vm::VirtualMachine& machine,
                                                       const vm::Program& program) {
  const auto result = machine.Execute(program.FindFunction(kEntryPoint));
  if (!result.Succeeded()) {
    return std::unexpected(result.message);
  }
  if (!result.value.IsInteger()) {
    return std::unexpected("test entry must return int");
  }
  return result.value.AsInteger();
}

std::expected<std::vector<std::byte>, std::string> CompileImage(std::string_view source) {
  runtime::Isolate isolate(1);
  const auto compiled = CompileSource(source, isolate);
  if (!compiled.Succeeded()) {
    return std::unexpected(frontend::DiagnosticsText(compiled.diagnostics));
  }
  auto saved = vm::SaveBytecode(compiled.program, isolate);
  if (!saved) {
    return std::unexpected(saved.error().message);
  }
  return std::move(*saved);
}

}  // namespace

std::expected<vm::Program, std::string> CompileCleanSource(std::string_view source,
                                                           runtime::Isolate& isolate) {
  const frontend::SourceInput input{.path = "main.vela", .text = source};
  return CompileCleanModules(std::span(&input, 1), input.path, isolate);
}

std::expected<vm::Program, std::string> CompileCleanModules(
    std::span<const frontend::SourceInput> sources,
    std::string_view entry,
    runtime::Isolate& isolate) {
  auto compiled = CompileModules(sources, entry, isolate);
  if (!compiled.Succeeded() || !compiled.diagnostics.empty()) {
    return std::unexpected(frontend::DiagnosticsText(compiled.diagnostics));
  }
  return std::move(compiled.program);
}

std::expected<vm::FunctionHandle, std::string> CompileEntry(std::string_view source,
                                                            runtime::Isolate& isolate) {
  const auto program = CompileCleanSource(source, isolate);
  if (!program) {
    return std::unexpected(program.error());
  }
  auto entry = program->FindFunction(kEntryPoint);
  if (!entry.IsValid()) {
    return std::unexpected("test source must define main");
  }
  return entry;
}

std::expected<vm::Program, std::string> ReloadSource(std::string_view source,
                                                     runtime::Isolate& isolate) {
  auto image = CompileImage(source);
  if (!image) {
    return std::unexpected(image.error());
  }
  auto loaded = vm::LoadBytecode(*image, isolate);
  if (!loaded) {
    return std::unexpected(loaded.error().message);
  }
  std::ranges::fill(*image, std::byte{0});
  image->clear();
  isolate.heap().Collect();
  return std::move(*loaded);
}

std::expected<std::int64_t, std::string> ExecuteInteger(const vm::Program& program,
                                                        runtime::Isolate& isolate) {
  vm::VirtualMachine machine(isolate);
  return InvokeInteger(machine, program);
}

std::expected<std::int64_t, std::string> AwaitInteger(runtime::Future& future,
                                                      runtime::Isolate& isolate) {
  const auto pumped = isolate.scheduler().PumpUntil(future);
  if (!pumped) {
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

std::expected<std::int64_t, std::string> ExecuteFutureInteger(const vm::Program& program,
                                                              runtime::Isolate& isolate) {
  const auto main = program.FindFunction(kEntryPoint);
  auto future = vm::VirtualMachine(isolate).ExecuteFuture(program, main.index());
  return AwaitInteger(future, isolate);
}

std::expected<std::array<std::int64_t, 2>, std::string> ExecuteProgramCopy(
    const vm::Program& program,
    runtime::Isolate& isolate) {
  const auto copy = program;
  vm::VirtualMachine machine(isolate);
  const auto first = InvokeInteger(machine, copy);
  if (!first) {
    return std::unexpected(first.error());
  }
  isolate.heap().Collect();
  const auto second = InvokeInteger(machine, program);
  if (!second) {
    return std::unexpected(second.error());
  }
  return std::array{*first, *second};
}

std::expected<std::int64_t, std::string> RunInteger(std::string_view source,
                                                    runtime::Isolate& isolate) {
  const auto program = CompileCleanSource(source, isolate);
  if (!program) {
    return std::unexpected(program.error());
  }
  return ExecuteInteger(*program, isolate);
}

}  // namespace vela::compiler::test
