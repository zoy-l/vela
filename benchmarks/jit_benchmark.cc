#include "benchmarks/jit_benchmark.h"

#include <chrono>
#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_set.h>
#include <benchmark/benchmark.h>
#include <llvm/Support/Regex.h>

#include "base/execution_status.h"
#include "base/native_program_mode.h"
#include "benchmarks/aot_benchmark_memory.h"
#include "benchmarks/process_memory.h"
#include "compiler/development/development_files.h"
#include "compiler/development/source_session.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/jit/compiler.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_abi.h"
#include "runtime/native/native_call.h"
#include "runtime/native/native_dispatch.h"
#include "runtime/native/native_instance.h"
#include "runtime/value/value.h"

namespace vela::benchmarks {
namespace {

using Clock = std::chrono::steady_clock;

struct JitRegistration final {
  std::string name;
  std::shared_ptr<JitWorkload> workload;
};

std::vector<JitRegistration>& Registrations() {
  static std::vector<JitRegistration> registrations;
  return registrations;
}

void ReportDuration(benchmark::State& state, const char* name, std::chrono::nanoseconds duration) {
  state.counters[name] = static_cast<double>(duration.count());
}

std::expected<std::shared_ptr<JitProgram>, std::string> CompileWorkload(const std::string& path,
                                                                        NativeCodeTier tier) {
  auto program = std::make_shared<JitProgram>();
  compiler::DevelopmentFiles files(path);
  const auto capturing = Clock::now();
  auto update = files.Capture(compiler::CompiledSourceSnapshot{});
  program->capture = Clock::now() - capturing;
  if (!update) {
    return std::unexpected(frontend::DiagnosticsText(update.error()));
  }
  program->entry = files.entry();
  const auto starting = Clock::now();
  auto compiler = compiler::jit::Compiler::Create();
  program->engine = Clock::now() - starting;
  if (!compiler) {
    return std::unexpected(frontend::DiagnosticsText(compiler.error()));
  }
  compiler::SourceSession sources(program->entry);
  const auto sourcing = Clock::now();
  const auto candidate = sources.Prepare(std::move(*update));
  program->source = Clock::now() - sourcing;
  if (!candidate) {
    return std::unexpected(frontend::DiagnosticsText(candidate.error().diagnostics));
  }
  const auto& snapshot = candidate->snapshot();
  program->frontend = snapshot.timings;
  const auto compiling = Clock::now();
  auto code = (*compiler)->Compile(snapshot.program, {.tier = tier});
  program->compilation = Clock::now() - compiling;
  if (!code) {
    return std::unexpected(frontend::DiagnosticsText(code.error()));
  }
  program->code = std::move(*code);
  program->resources = (*compiler)->resource_counts();
  const auto peak = PeakResidentBytes();
  if (!peak) {
    return std::unexpected(peak.error());
  }
  program->compilation_peak_resident_bytes = *peak;
  return program;
}

}  // namespace

JitWorkload::JitWorkload(std::string path, NativeCodeTier tier)
    : path_(std::move(path)), tier_(tier) {}

std::expected<std::shared_ptr<const JitProgram>, std::string> JitWorkload::Prepare() {
  // Google Benchmark 在本线程顺序执行用例；失败也只报告一次编译结果，不重试。
  if (!program_) {
    const auto preparing = Clock::now();
    auto program = CompileWorkload(path_, tier_);
    if (program) {
      auto prepared = std::move(*program);
      prepared->preparation = Clock::now() - preparing;
      program_ = std::move(prepared);
    } else {
      program_ = std::unexpected(std::move(program.error()));
    }
  }
  return *program_;
}

void RegisterJitPreparation(std::string name, std::shared_ptr<JitWorkload> workload) {
  Registrations().push_back(
      {.name = std::move(name) + "/real_time", .workload = std::move(workload)});
}

std::expected<std::size_t, std::string> PrepareJitWorkloads(std::string filter) {
  if (filter.empty() || filter == "all") {
    filter = ".";
  }
  const auto negative = filter.starts_with('-');
  if (negative) {
    filter.erase(0, 1);
  }
  // Google Benchmark 也使用 POSIX extended 语法，包括整条过滤器的负向前缀。
  const llvm::Regex expression(filter);
  std::string error;
  if (!expression.isValid(error)) {
    return std::unexpected(std::move(error));
  }
  absl::flat_hash_set<JitWorkload*> prepared;
  for (const auto& registration : Registrations()) {
    auto* workload = registration.workload.get();
    if (expression.match(registration.name) == negative || !prepared.insert(workload).second) {
      continue;
    }
    if (const auto program = workload->Prepare(); !program) {
      return std::unexpected(program.error());
    }
  }
  return prepared.size();
}

std::expected<JitSetup, std::string> LoadJitWorkload(JitWorkload& workload,
                                                     runtime::NativeBindings bindings) {
  const auto program = workload.Prepare();
  if (!program) {
    return std::unexpected(program.error());
  }
  const auto loading = Clock::now();
  JitSetup setup{
      .program = *program,
      .isolate = std::make_unique<runtime::Isolate>(std::move(bindings)),
      .publication = {},
      .entry_name = {},
      .entry = std::nullopt,
  };
  auto& dispatch = setup.isolate->native_dispatch();
  const auto publishing = Clock::now();
  auto publication = dispatch.Prepare(0, setup.program->code.image);
  if (!publication) {
    return std::unexpected(publication.error().message);
  }
  const auto published = dispatch.Publish(std::move(*publication));
  if (!published) {
    return std::unexpected(published.error().message);
  }
  setup.publication = *published;
  setup.pause = Clock::now() - publishing;
  setup.load = Clock::now() - loading;
  return setup;
}

std::expected<runtime::Value, ExecutionStatus> InvokeJitWorkload(JitSetup& setup,
                                                                 std::string_view name) {
  auto& dispatch = setup.isolate->native_dispatch();
  if (!setup.entry || setup.entry_name != name) {
    const auto entry = dispatch.FindEntry(setup.program->entry, name);
    if (!entry) {
      return std::unexpected(entry.error());
    }
    setup.entry_name = name;
    setup.entry = entry->declaration();
  }
  // 与开发会话的稳态入口一致：缓存声明身份，每次调用仍解析当前已发布版本。
  const auto target = dispatch.Resolve(*setup.entry);
  if (!target) {
    return std::unexpected(target.error());
  }
  auto instance = runtime::NativeInstance::Borrow(*target->state);
  runtime::NativeExecution execution(instance);
  return runtime::InvokeNativeTarget(execution, target->function, {});
}

void ReportJitSetup(benchmark::State& state, const JitSetup& setup) {
  const auto& program = *setup.program;
  const auto& timings = program.code.timings;
  ReportDuration(state, "file_capture_ns", program.capture);
  ReportDuration(state, "engine_creation_ns", program.engine);
  ReportDuration(state, "suite_preparation_ns", program.preparation);
  ReportDuration(state, "load_ns", setup.load);
  ReportDuration(state, "source_total_ns", program.source);
  ReportDuration(state, "code_total_ns", program.compilation);
  ReportDuration(state, "source_loading_ns", program.frontend.loading);
  ReportDuration(state, "type_checking_ns", program.frontend.checking);
  ReportDuration(state, "typed_ir_ns", program.frontend.ir);
  ReportDuration(state, "llvm_lowering_ns", timings.generation);
  ReportDuration(state, "llvm_optimization_ns", timings.optimization);
  ReportDuration(state, "machine_compilation_ns", timings.compilation);
  ReportDuration(state, "linking_ns", timings.linking);
  ReportDuration(state, "cache_ns", timings.cache);
  ReportDuration(state, "instantiation_ns", setup.publication.instantiation);
  ReportDuration(state, "initial_publication_pause_ns", setup.pause);
  state.counters["compilation_peak_resident_bytes"] =
      static_cast<double>(program.compilation_peak_resident_bytes);
}

void ReportJitMemory(benchmark::State& state, const JitSetup& setup) {
  auto& isolate = *setup.isolate;
  ReportAotHeap(state, isolate);
  const auto& code = setup.program->resources;
  const auto& heap = isolate.heap();
  const auto layouts = isolate.layout_counts();
  state.counters["code_batches"] = static_cast<double>(code.batches);
  state.counters["code_images"] = static_cast<double>(code.images);
  state.counters["image_data_bytes"] = static_cast<double>(code.image_data_bytes);
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

void ReportJitFirstExecution(benchmark::State& state, Clock::time_point started) {
  ReportDuration(state, "first_execution_ns",
                 std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - started));
}

}  // namespace vela::benchmarks
