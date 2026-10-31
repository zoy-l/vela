#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/cleanup/cleanup.h>

#include <benchmark/benchmark.h>

#include "base/native_program_mode.h"
#include "compiler/development/development_session.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/test_support/checkpoint_publication.h"
#include "runtime/async/future.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_context.h"
#include "runtime/value/object.h"
#include "runtime/value/value.h"

namespace vela::benchmarks {
namespace {

using Clock = std::chrono::steady_clock;
using compiler::DevelopmentOperation;
using compiler::DevelopmentResult;
using compiler::DevelopmentSession;
using compiler::DevelopmentStatus;

constexpr std::int64_t kSaved = 40;
constexpr std::int64_t kAdded = 2;
constexpr std::int64_t kRetainedEdits = 8;
constexpr std::int64_t kManyEdits = 32;
constexpr std::int64_t kClosures = 128;
constexpr std::int64_t kManyInstances = 4096;
constexpr std::int64_t kManyContinuations = 1024;
constexpr std::string_view kModule = "reload_benchmark.vela";

enum class RetainedState : std::uint8_t { None, Closures, Instances, Continuations };

std::string Source(RetainedState retained, std::int64_t version) {
  auto source = "func current(): int { return " + std::to_string(version) +
                "; } func main(): int { return current(); } ";
  switch (retained) {
    case RetainedState::None:
      break;
    case RetainedState::Closures:
      source +=
          "export func make(): () => int { const saved = 40; "
          "return (): int => saved + current(); } "
          "export func read(callback: () => int): int { return callback(); }";
      break;
    case RetainedState::Instances:
      source += "class Box { value: int = 40; ";
      if (version != 0) {
        source += "added: int = 2; ";
      }
      source +=
          "} export func make(): Box { return Box(); } "
          "export func read(box: Box): int { return box.value";
      source += version == 0 ? "; }" : " + box.added; }";
      break;
    case RetainedState::Continuations:
      source +=
          "export async func start(gate: Future<int>): Future<int> { "
          "const saved = 40; await gate; return saved + current(); }";
      break;
  }
  return source;
}

std::expected<compiler::DevelopmentCandidate, std::string> Prepare(DevelopmentSession& session,
                                                                   std::string source,
                                                                   DevelopmentOperation operation) {
  auto candidate = session.Prepare(
      {
          .base_revision = session.snapshot()->revision,
          .changes = {{.path = std::string(kModule), .text = std::move(source)}},
          .added_roots = {},
      },
      operation);
  if (!candidate) {
    const auto& error = candidate.error();
    return std::unexpected(error.message + '\n' + frontend::DiagnosticsText(error.diagnostics));
  }
  return std::move(*candidate);
}

std::expected<DevelopmentResult, std::string> Apply(DevelopmentSession& session,
                                                    std::string source,
                                                    DevelopmentOperation operation) {
  auto candidate = Prepare(session, std::move(source), operation);
  if (!candidate) {
    return std::unexpected(std::move(candidate.error()));
  }
  (void)session.Submit(std::move(*candidate));
  auto result = session.Pump();
  if (result.status != DevelopmentStatus::Accepted) {
    return std::unexpected(result.message);
  }
  return result;
}

void AddDuration(benchmark::State& state, const char* name, std::chrono::nanoseconds duration) {
  state.counters[name] += static_cast<double>(duration.count());
}

std::expected<std::unique_ptr<DevelopmentSession>, std::string> LoadSession(NativeCodeTier tier,
                                                                            std::string source) {
  compiler::DevelopmentOptions options;
  options.compilation.tier = tier;
  auto created = DevelopmentSession::Create(std::string(kModule), {}, std::move(options));
  if (!created) {
    return std::unexpected("cannot create development session\n" +
                           frontend::DiagnosticsText(created.error()));
  }
  if (const auto loaded = Apply(**created, std::move(source), DevelopmentOperation::Load);
      !loaded) {
    return std::unexpected(loaded.error());
  }
  return std::move(*created);
}

void AddTimings(benchmark::State& state, const DevelopmentResult& result) {
  const auto& timings = result.timings;
  AddDuration(state, "source_ns", timings.source);
  AddDuration(state, "source_loading_ns", timings.frontend.loading);
  AddDuration(state, "type_checking_ns", timings.frontend.checking);
  AddDuration(state, "typed_ir_ns", timings.frontend.ir);
  AddDuration(state, "llvm_lowering_ns", timings.native.generation);
  AddDuration(state, "llvm_optimization_ns", timings.native.optimization);
  AddDuration(state, "machine_compilation_ns", timings.native.compilation);
  AddDuration(state, "linking_ns", timings.native.linking);
  AddDuration(state, "new_isolate_preparation_ns", timings.preparation);
  AddDuration(state, "safepoint_waiting_ns", timings.waiting);
  AddDuration(state, "pause_ns", timings.pause);
  AddDuration(state, "instantiation_ns", timings.publication.instantiation);
  AddDuration(state, "validation_ns", timings.publication.validation);
  AddDuration(state, "materialization_ns", timings.publication.materialization);
  state.counters["materialized_objects"] +=
      static_cast<double>(timings.publication.materialized_objects);
  AddDuration(state, "state_preparation_ns", timings.publication.state_preparation);
  AddDuration(state, "deoptimization_ns", timings.publication.deoptimization);
  AddDuration(state, "commit_ns", timings.publication.commit);
  AddDuration(state, "refresh_ns", timings.refresh);
  AddDuration(state, "stopping_ns", timings.stopping);
  AddDuration(state, "entry_ns", timings.entry);
  auto& maximum = state.counters["pause_max_ns"];
  maximum = std::max(maximum.value, static_cast<double>(timings.pause.count()));
  auto& migration = state.counters["state_preparation_max_ns"];
  migration =
      std::max(migration.value, static_cast<double>(timings.publication.state_preparation.count()));
}

void AverageTimings(benchmark::State& state, std::int64_t operations) {
  for (auto& [name, counter] : state.counters) {
    if ((name.ends_with("_ns") && !name.contains("_max_")) || name == "materialized_objects") {
      counter.value /= static_cast<double>(operations);
    }
  }
}

void Memory(benchmark::State& state, DevelopmentSession& session, const char* prefix) {
  auto& isolate = session.isolate();
  auto& heap = isolate.heap();
  heap.FinishCollection();
  heap.Collect();
  const auto code = session.code_resources();
  const auto name = std::string(prefix);
  state.counters[name + "heap_bytes"] = static_cast<double>(heap.allocated_bytes());
  state.counters[name + "external_field_bytes"] = static_cast<double>(heap.instance_field_bytes());
  state.counters[name + "code_batches"] = static_cast<double>(code.batches);
  state.counters[name + "executable_bytes"] = static_cast<double>(code.executable_bytes);
  state.counters[name + "read_only_bytes"] = static_cast<double>(code.read_only_bytes);
  state.counters[name + "writable_bytes"] = static_cast<double>(code.writable_bytes);
  state.counters[name + "constant_payload_bytes"] =
      static_cast<double>(code.constant_payload_bytes);
  state.counters[name + "debug_payload_bytes"] = static_cast<double>(code.debug_payload_bytes);
  const auto& scheduler = isolate.scheduler();
  state.counters[name + "suspended_frames"] =
      static_cast<double>(scheduler.suspended_frame_count());
  state.counters[name + "saved_state_bytes"] = static_cast<double>(scheduler.saved_state_bytes());
}

class RetainedValues final {
 public:
  RetainedValues(DevelopmentSession& session, RetainedState kind, std::int64_t count)
      : session_(&session),
        kind_(kind),
        values_(static_cast<std::size_t>(count)),
        roots_(session.isolate().heap(), values_.data(), values_.size()),
        gate_root_(session.isolate().heap(), &gate_value_, 1) {}

  std::expected<void, std::string> Initialize() {
    auto& session = *session_;
    if (kind_ == RetainedState::Continuations) {
      runtime::NativeContext context(session.isolate());
      gate_ = context.CreateCompleter();
      const auto future = context.FutureValue(gate_.future());
      if (!future) {
        return std::unexpected("cannot create suspended-task gate");
      }
      gate_value_ = *future;
      const auto* signature = session.isolate().CreateTypeSignature({
          .parameters = {},
          .result = runtime::TypeConstraint{.kind = runtime::ValueKind::Integer},
      });
      const runtime::TypeConstraint type{
          .kind = runtime::ValueKind::Object,
          .object_kind = runtime::ObjectKind::Future,
          .signature = signature,
      };
      if (!runtime::Heap::BindRuntimeType(*gate_value_.AsObject(), type)) {
        return std::unexpected("cannot bind suspended-task gate type");
      }
    }
    for (auto& value : values_) {
      const auto made = kind_ == RetainedState::Continuations
                            ? session.Invoke(kModule, "start", std::array{gate_value_})
                            : session.Invoke(kModule, "make");
      if (!made) {
        return std::unexpected("retained-state setup failed");
      }
      value = *made;
    }
    original_ = values_.empty() ? runtime::Value::Null() : values_.front();
    return {};
  }

  std::expected<void, std::string> Verify(std::int64_t version) {
    auto& session = *session_;
    if (kind_ == RetainedState::Continuations) {
      if (!gate_.Complete(runtime::Value::Integer(0))) {
        return std::unexpected("cannot release suspended tasks");
      }
      for (const auto value : values_) {
        auto& future = *runtime::Heap::GetFuture(*value.AsObject());
        if (!session.isolate().scheduler().PumpUntil(future)) {
          return std::unexpected("suspended task did not complete");
        }
        const auto result = future.value();
        if (!result || result->AsInteger() != kSaved + version) {
          return std::unexpected("old continuation checksum mismatch");
        }
      }
      return {};
    }
    const auto expected = kSaved + (kind_ == RetainedState::Instances ? kAdded : version);
    for (const auto value : values_) {
      const auto read = session.Invoke(kModule, "read", std::array{value});
      if (!read || read->AsInteger() != expected) {
        return std::unexpected("retained object or closure checksum mismatch");
      }
    }
    if (!values_.empty() && values_.front().AsObject() != original_.AsObject()) {
      return std::unexpected("reload changed retained object identity");
    }
    return {};
  }

  void Release() {
    std::ranges::fill(values_, runtime::Value::Null());
    gate_value_ = runtime::Value::Null();
    gate_ = runtime::Completer();
  }

 private:
  DevelopmentSession* session_;
  RetainedState kind_;
  std::vector<runtime::Value> values_;
  runtime::RootRange roots_;
  runtime::Completer gate_;
  runtime::Value gate_value_;
  runtime::RootRange gate_root_;
  runtime::Value original_;
};

void Reload(benchmark::State& state, NativeCodeTier tier, RetainedState retained) {
  auto created = LoadSession(tier, Source(retained, 0));
  if (!created) {
    state.SkipWithError(created.error());
    return;
  }
  auto& session = **created;
  const auto count = retained == RetainedState::None ? 0 : state.range(0);
  const auto edits = retained == RetainedState::None ? state.range(0) : kRetainedEdits;
  RetainedValues values(session, retained, count);
  if (const auto initialized = values.Initialize(); !initialized) {
    state.SkipWithError(initialized.error());
    return;
  }
  Memory(state, session, "before_");
  std::int64_t version = 0;
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    const auto started = Clock::now();
    for (std::int64_t edit = 0; edit < edits; ++edit) {
      const auto updated =
          Apply(session, Source(retained, ++version), DevelopmentOperation::HotReload);
      if (!updated) {
        state.SkipWithError(updated.error());
        return;
      }
      AddTimings(state, *updated);
    }
    state.SetIterationTime(std::chrono::duration<double>(Clock::now() - started).count() /
                           static_cast<double>(edits));
  }
  AverageTimings(state, state.iterations() * edits);
  Memory(state, session, "retained_");
  if (const auto verified = values.Verify(version); !verified) {
    state.SkipWithError(verified.error());
    return;
  }
  values.Release();
  Memory(state, session, "released_");
  if (session.code_resources().batches != 1 ||
      session.isolate().scheduler().suspended_frame_count() != 0) {
    state.SkipWithError("old code or suspended frames survived their final reference");
  }
  state.counters["edits"] = static_cast<double>(version);
  state.counters["retained_values"] = static_cast<double>(count);
}

void ActiveReload(benchmark::State& state, NativeCodeTier tier) {
  auto created = LoadSession(tier, Source(RetainedState::None, 0));
  if (!created) {
    state.SkipWithError(created.error());
    return;
  }
  auto& session = **created;
  std::int64_t version = 0;
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    const auto started = Clock::now();
    auto candidate =
        Prepare(session, Source(RetainedState::None, ++version), DevelopmentOperation::HotReload);
    if (!candidate) {
      state.SkipWithError(candidate.error());
      return;
    }
    (void)session.Submit(std::move(*candidate));
    const auto value = session.RunEntry();
    state.SetIterationTime(std::chrono::duration<double>(Clock::now() - started).count());
    if (!value || value->AsInteger() != version ||
        session.result().status != DevelopmentStatus::Accepted) {
      state.SkipWithError("active frame did not continue with the current callee");
      return;
    }
    AddTimings(state, session.result());
  }
  AverageTimings(state, state.iterations());
  state.counters["deoptimized_frames"] =
      static_cast<double>(session.isolate().native_dispatch().deoptimized_frames());
  if (tier == NativeCodeTier::Optimized &&
      session.isolate().native_dispatch().deoptimized_frames() == 0) {
    state.SkipWithError("active optimized frame was not deoptimized");
    return;
  }
  Memory(state, session, "released_");
}

void Rebuild(benchmark::State& state, NativeCodeTier tier) {
  auto created = LoadSession(tier, Source(RetainedState::None, 0));
  if (!created) {
    state.SkipWithError(created.error());
    return;
  }
  auto& session = **created;
  std::int64_t version = 0;
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    const auto started = Clock::now();
    const auto updated =
        Apply(session, Source(RetainedState::None, ++version), DevelopmentOperation::Reload);
    state.SetIterationTime(std::chrono::duration<double>(Clock::now() - started).count());
    if (!updated || session.entry_value().AsInteger() != version) {
      state.SkipWithError(updated ? "Reload entry checksum mismatch" : updated.error());
      return;
    }
    AddTimings(state, *updated);
  }
  AverageTimings(state, state.iterations());
  Memory(state, session, "released_");
  if (session.code_resources().batches != 1) {
    state.SkipWithError("Reload retained code from the stopped Isolate");
  }
}

std::string VirtualSource(std::int64_t version) {
  return "func checkpoint(): void {} class Box { value: int; text: string; "
         "read(): int { return this.value + this.text.length + " +
         std::to_string(version) +
         "; } } func main(): int { const box = Box(value: 38, text: 'a' + 'b'); "
         "checkpoint(); return box.read(); }";
}

void VirtualReload(benchmark::State& state, NativeCodeTier tier) {
  auto created = LoadSession(tier, VirtualSource(0));
  if (!created) {
    state.SkipWithError(created.error());
    return;
  }
  auto& session = **created;
  compiler::test::CheckpointPublication pause(session);
  session.SetDebugger(&pause);
  absl::Cleanup detach_debugger = [&] { session.SetDebugger(nullptr); };
  std::int64_t version = 0;
  for (auto iteration : state) {
    benchmark::DoNotOptimize(iteration);
    const auto started = Clock::now();
    auto candidate = Prepare(session, VirtualSource(++version), DevelopmentOperation::HotReload);
    if (!candidate) {
      state.SkipWithError(candidate.error());
      return;
    }
    pause.Stage(std::move(*candidate));
    const auto value = session.RunEntry();
    state.SetIterationTime(std::chrono::duration<double>(Clock::now() - started).count());
    if (!value || value->AsInteger() != kSaved + version || pause.HasPending() ||
        pause.result().status != DevelopmentStatus::Accepted ||
        pause.result().timings.publication.materialized_objects !=
            (tier == NativeCodeTier::Optimized ? 1U : 0U)) {
      state.SkipWithError("virtual object reload or original caller recovery failed");
      return;
    }
    AddTimings(state, pause.result());
  }
  std::move(detach_debugger).Invoke();
  AverageTimings(state, state.iterations());
  Memory(state, session, "released_");
  if (session.code_resources().batches != 1) {
    state.SkipWithError("virtual descriptors retained old code after recovery");
  }
}

int RegisterReloadBenchmarks() {
  benchmark::AddCustomContext("jit_baseline",
                              "LLVM O2; current dispatch; precise roots; safepoints");
  benchmark::AddCustomContext(
      "jit_optimized", "LLVM O2; version dependencies; direct calls/inlining; baseline recovery");
  for (const auto tier : {NativeCodeTier::Baseline, NativeCodeTier::Optimized}) {
    const auto prefix =
        std::string("hot/") + (tier == NativeCodeTier::Baseline ? "baseline/" : "optimized/");
    benchmark::RegisterBenchmark(prefix + "edits", Reload, tier, RetainedState::None)
        ->Arg(kRetainedEdits)
        ->Arg(kManyEdits)
        ->UseManualTime();
    benchmark::RegisterBenchmark(prefix + "closures", Reload, tier, RetainedState::Closures)
        ->Arg(kClosures)
        ->UseManualTime();
    benchmark::RegisterBenchmark(prefix + "instances", Reload, tier, RetainedState::Instances)
        ->Arg(1)
        ->Arg(kManyInstances)
        ->UseManualTime();
    benchmark::RegisterBenchmark(prefix + "continuations", Reload, tier,
                                 RetainedState::Continuations)
        ->Arg(1)
        ->Arg(kManyContinuations)
        ->UseManualTime();
    benchmark::RegisterBenchmark(prefix + "rebuild", Rebuild, tier)->UseManualTime();
    benchmark::RegisterBenchmark(prefix + "active", ActiveReload, tier)->UseManualTime();
    benchmark::RegisterBenchmark(prefix + "virtual", VirtualReload, tier)->UseManualTime();
  }
  return 0;
}

[[maybe_unused]] const int kRegistered = RegisterReloadBenchmarks();

}  // namespace
}  // namespace vela::benchmarks
