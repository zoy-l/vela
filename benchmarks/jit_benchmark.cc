#include "benchmarks/jit_benchmark.h"

#include <chrono>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include <benchmark/benchmark.h>

#include "base/execution_status.h"
#include "benchmarks/aot_benchmark_memory.h"
#include "benchmarks/process_memory.h"
#include "compiler/development/development_files.h"
#include "compiler/development/development_session.h"
#include "compiler/development/source_session.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/jit/compiler.h"
#include "runtime/heap.h"
#include "runtime/isolate.h"
#include "runtime/value.h"

namespace vela::benchmarks {
namespace {

void ReportDuration(benchmark::State& state, const char* name, std::chrono::nanoseconds duration) {
  state.counters[name] = static_cast<double>(duration.count());
}

}  // namespace

std::expected<JitSetup, std::string> LoadJitWorkload(const std::string& path,
                                                     runtime::NativeBindings bindings,
                                                     compiler::jit::CompileOptions options) {
  using Clock = std::chrono::steady_clock;
  JitSetup setup{.session = {}};
  compiler::DevelopmentFiles files(path);
  const auto capturing = Clock::now();
  auto update = files.Capture(compiler::CompiledSourceSnapshot{});
  setup.capture = Clock::now() - capturing;
  if (!update) {
    return std::unexpected(frontend::DiagnosticsText(update.error()));
  }
  const auto starting = Clock::now();
  compiler::DevelopmentOptions development;
  development.compilation = std::move(options);
  auto session = compiler::DevelopmentSession::Create(files.entry(), std::move(bindings),
                                                      std::move(development));
  setup.engine = Clock::now() - starting;
  if (!session) {
    return std::unexpected(frontend::DiagnosticsText(session.error()));
  }
  setup.session = std::move(*session);
  auto candidate = setup.session->Prepare(std::move(*update), compiler::DevelopmentOperation::Load);
  if (!candidate) {
    const auto& error = candidate.error();
    return std::unexpected(error.message + '\n' + frontend::DiagnosticsText(error.diagnostics));
  }
  const auto peak = PeakResidentBytes();
  if (!peak) {
    return std::unexpected(peak.error());
  }
  setup.compilation_peak_resident_bytes = *peak;
  (void)setup.session->Submit(std::move(*candidate));
  const auto loaded = setup.session->Pump();
  if (loaded.status != compiler::DevelopmentStatus::Accepted) {
    return std::unexpected(loaded.message + '\n' + frontend::DiagnosticsText(loaded.diagnostics));
  }
  setup.load = Clock::now() - capturing;
  return setup;
}

std::expected<runtime::Value, ExecutionStatus> InvokeJitWorkload(
    compiler::DevelopmentSession& session,
    std::string_view name) {
  const auto snapshot = session.snapshot();
  return session.InvokeEntry({.module = snapshot->roots.front(), .name = name});
}

void ReportJitSetup(benchmark::State& state, const JitSetup& setup) {
  const auto& timings = setup.session->result().timings;
  ReportDuration(state, "file_capture_ns", setup.capture);
  ReportDuration(state, "engine_creation_ns", setup.engine);
  ReportDuration(state, "load_ns", setup.load);
  ReportDuration(state, "source_total_ns", timings.source);
  ReportDuration(state, "code_total_ns", timings.code);
  ReportDuration(state, "source_loading_ns", timings.frontend.loading);
  ReportDuration(state, "type_checking_ns", timings.frontend.checking);
  ReportDuration(state, "typed_ir_ns", timings.frontend.ir);
  ReportDuration(state, "llvm_lowering_ns", timings.native.generation);
  ReportDuration(state, "llvm_optimization_ns", timings.native.optimization);
  ReportDuration(state, "machine_compilation_ns", timings.native.compilation);
  ReportDuration(state, "linking_ns", timings.native.linking);
  ReportDuration(state, "cache_ns", timings.native.cache);
  ReportDuration(state, "instantiation_ns", timings.publication.instantiation);
  ReportDuration(state, "initial_publication_pause_ns", timings.pause);
  state.counters["compilation_peak_resident_bytes"] =
      static_cast<double>(setup.compilation_peak_resident_bytes);
}

void ReportJitMemory(benchmark::State& state, compiler::DevelopmentSession& session) {
  auto& isolate = session.isolate();
  ReportAotHeap(state, isolate);
  const auto code = session.code_resources();
  const auto& heap = isolate.heap();
  const auto layouts = isolate.layout_counts();
  state.counters["code_batches"] = static_cast<double>(code.batches);
  state.counters["code_functions"] = static_cast<double>(code.functions);
  state.counters["code_executable_bytes"] = static_cast<double>(code.executable_bytes);
  state.counters["code_read_only_bytes"] = static_cast<double>(code.read_only_bytes);
  state.counters["code_writable_bytes"] = static_cast<double>(code.writable_bytes);
  state.counters["constant_payload_bytes"] = static_cast<double>(code.constant_payload_bytes);
  state.counters["debug_payload_bytes"] = static_cast<double>(code.debug_payload_bytes);
  state.counters["external_field_bytes"] = static_cast<double>(heap.instance_field_bytes());
  state.counters["class_layouts"] = static_cast<double>(layouts.classes);
  state.counters["nominal_types"] = static_cast<double>(layouts.nominal_types);
}

void ReportJitFirstExecution(benchmark::State& state,
                             std::chrono::steady_clock::time_point started) {
  ReportDuration(state, "first_execution_ns",
                 std::chrono::duration_cast<std::chrono::nanoseconds>(
                     std::chrono::steady_clock::now() - started));
}

}  // namespace vela::benchmarks
