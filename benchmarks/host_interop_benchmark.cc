#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>
#include <vela/host.h>

#include "benchmarks/heap_metrics.h"
#include "benchmarks/host_interop/entries.h"
#include "compiler/frontend/diagnostic.h"
#include "compiler/jit/compiler.h"
#include "compiler/jit/source_compiler.h"
#include "runtime/async/future.h"
#include "runtime/heap/heap.h"
#include "runtime/heap/host_root.h"
#include "runtime/isolate/external_resources.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/handle_access.h"
#include "runtime/native/host_async.h"
#include "runtime/native/host_object.h"
#include "runtime/native/native_context.h"
#include "runtime/native/native_execution.h"
#include "runtime/native/native_function.h"
#include "runtime/native/native_instance.h"
#include "runtime/native/native_type_traits.h"
#include "runtime/strings/string.h"
#include "runtime/value/value.h"

namespace vela::benchmarks::interop {
namespace {

enum class Mode : std::uint8_t { Jit, Aot };
enum class Path : std::uint8_t { Typed, Runtime, Wrapper };
enum class StringOperation : std::uint8_t { Borrow, Copy };
enum class Element : std::uint8_t { Scalar, Reference };
enum class Call : std::uint8_t { Scalar, Reentry };
constexpr std::int64_t kExpected = 42;
constexpr std::int64_t kInput = kExpected - 1;
using Integers = Array<std::int64_t>;
using Callback = Function<std::int64_t(std::int64_t)>;

template <class T>
struct EntryResult;
template <class R>
struct EntryResult<Entry<R()>> {
  using Type = R;
};
using Box = EntryResult<api::Entry_object>::Type;

std::int64_t Step(std::int64_t value) noexcept {
  return value + 1;
}
Local<Integers> Echo(Local<Integers> value) noexcept {
  return value;
}

Result<std::int64_t> Reenter(runtime::NativeContext& context,
                             Local<Callback> callback,
                             std::int64_t value) noexcept {
  return callback.Call(context.handles(), value);
}

std::unique_ptr<runtime::NativeInstance> Instantiate(benchmark::State& state,
                                                     runtime::Isolate& isolate,
                                                     Mode mode) {
  if (!isolate.RegisterNative(runtime::NativeFunction::Bind<Step>("interop.step")) ||
      !isolate.RegisterNative(runtime::NativeFunction::Bind<Echo>("interop.echo")) ||
      !isolate.RegisterNative(runtime::NativeFunction::Bind<Reenter>("interop.bounce"))) {
    state.SkipWithError("host registration failed");
    return nullptr;
  }
  std::shared_ptr<const runtime::NativeImage> image;
  if (mode == Mode::Jit) {
    auto compiler = compiler::jit::Compiler::Create();
    if (!compiler) {
      state.SkipWithError(frontend::DiagnosticsText(compiler.error()));
      return nullptr;
    }
    auto compiled = compiler::jit::CompileFile(**compiler, kSource);
    if (!compiled.Succeeded()) {
      state.SkipWithError(frontend::DiagnosticsText(compiled.diagnostics));
      return nullptr;
    }
    image = std::move(compiled.code.image);
  }
  auto instance = image ? runtime::NativeInstance::Instantiate(isolate, std::move(image))
                        : runtime::NativeInstance::Instantiate(isolate, *kImage);
  if (!instance) {
    state.SkipWithError(instance.error().message);
    return nullptr;
  }
  return std::move(*instance);
}

template <class Action>
void Measure(benchmark::State& state, runtime::Isolate& isolate, Action action) {
  auto& heap = isolate.heap();
  CollectHeapBaseline(state, heap);
  const auto baseline_bytes = heap.allocated_bytes();
  std::vector<runtime::ExternalResourceInfo> roots;
  heap.AppendExternalRoots(roots);
  const auto baseline_roots = roots.size();
  const auto baseline_blockers = isolate.ReloadBlockers().size();
  heap.ResetAllocationStatistics();
  for (auto iteration : state) {
    HandleScope scope(isolate);
    auto result = action(scope);
    if (!result || *result != kExpected) {
      state.SkipWithError(result ? "interop checksum mismatch" : result.error().message);
      break;
    }
    benchmark::DoNotOptimize(*result);
  }
  const auto statistics = heap.allocation_statistics();
  const auto iterations = static_cast<double>(state.iterations());
  state.counters["heap_allocations_per_operation"] =
      static_cast<double>(statistics.count) / iterations;
  state.counters["heap_bytes_per_operation"] = static_cast<double>(statistics.bytes) / iterations;
  state.counters["peak_extra_bytes"] =
      static_cast<double>(statistics.peak_live_bytes - baseline_bytes);
  state.counters["external_roots"] = static_cast<double>(baseline_roots);
  state.counters["shutdown_blockers"] = static_cast<double>(baseline_blockers);
  state.counters["scope_storage_bytes"] = static_cast<double>(sizeof(HandleScope));
  const auto retained_bytes = ReportHeap(state, heap);
  const auto retained_blockers = isolate.ReloadBlockers().size();
  state.counters["retained_extra_bytes"] =
      static_cast<double>(retained_bytes) - static_cast<double>(baseline_bytes);
  roots.clear();
  heap.AppendExternalRoots(roots);
  if (roots.size() != baseline_roots || retained_blockers != baseline_blockers ||
      retained_bytes != baseline_bytes) {
    state.SkipWithError("interop cleanup changed roots " + std::to_string(baseline_roots) + " -> " +
                        std::to_string(roots.size()) + ", blockers " +
                        std::to_string(baseline_blockers) + " -> " +
                        std::to_string(retained_blockers) + ", heap bytes " +
                        std::to_string(baseline_bytes) + " -> " + std::to_string(retained_bytes));
  }
}

template <Mode Execution, Path Boundary, Call Operation = Call::Scalar>
void ScalarEntry(benchmark::State& state) {
  runtime::Isolate isolate;
  auto instance = Instantiate(state, isolate, Execution);
  if (!instance) {
    return;
  }
  const std::string_view name = Operation == Call::Scalar ? "scalar" : "reentry";
  auto entry = Entry<std::int64_t(std::int64_t)>::Bind(*instance, name);
  const auto function = instance->FindFunction(name);
  if (!entry || !function) {
    state.SkipWithError("scalar entry is missing");
    return;
  }
  Measure(state, isolate, [&](HandleScope& scope) -> Result<std::int64_t> {
    if constexpr (Boundary == Path::Typed) {
      return entry->Call(scope, std::int64_t{kInput});
    } else if constexpr (Boundary == Path::Wrapper) {
      if constexpr (Operation == Call::Scalar) {
        return api::Call_scalar(scope, instance.get(), kInput);
      } else {
        return api::Call_reentry(scope, instance.get(), kInput);
      }
    } else {
      const std::array arguments{runtime::Value::Integer(kInput)};
      const auto result = runtime::ExecuteNativeFunction(*instance, *function, arguments);
      if (!result.Succeeded()) {
        return std::unexpected(
            Error{.status = result.status, .message = result.message, .stack_trace = {}});
      }
      return result.value.AsInteger();
    }
  });
}

template <Mode Execution, Path Boundary>
void LocalEntry(benchmark::State& state) {
  runtime::Isolate isolate;
  auto instance = Instantiate(state, isolate, Execution);
  if (!instance) {
    return;
  }
  HandleScope owner(isolate);
  auto values = CreateArray<std::int64_t>(owner);
  auto entry = Entry<Integers(Integers)>::Bind(*instance, "local");
  const auto function = instance->FindFunction("local");
  if (!values || !entry || !function || !values->Push(owner, kExpected)) {
    state.SkipWithError("local entry preparation failed");
    return;
  }
  Measure(state, isolate, [&](HandleScope& scope) -> Result<std::int64_t> {
    Result<Local<Integers>> result = [&]() -> Result<Local<Integers>> {
      if constexpr (Boundary == Path::Typed) {
        return entry->Call(scope, *values);
      } else if constexpr (Boundary == Path::Wrapper) {
        return api::Call_local(scope, instance.get(), *values);
      } else {
        const std::array arguments{internal::HandleAccess::Read(*values)};
        const auto returned = runtime::ExecuteNativeFunction(*instance, *function, arguments);
        if (!returned.Succeeded()) {
          return std::unexpected(
              Error{.status = returned.status, .message = returned.message, .stack_trace = {}});
        }
        return internal::HandleAccess::Root<Integers>(scope, returned.value);
      }
    }();
    if (!result) {
      return std::unexpected(std::move(result.error()));
    }
    if (internal::HandleAccess::Read(*result).AsObject() !=
        internal::HandleAccess::Read(*values).AsObject()) {
      return std::unexpected(Error{.message = "array identity changed", .stack_trace = {}});
    }
    return result->Get(scope, 0);
  });
}

template <Path Boundary, StringOperation Operation>
void Strings(benchmark::State& state) {
  runtime::Isolate isolate;
  HandleScope owner(isolate);
  const auto text = CreateString(owner, std::string(kExpected, 'x'));
  if (!text) {
    state.SkipWithError("string preparation failed");
    return;
  }
  Measure(state, isolate, [&](HandleScope&) -> Result<std::int64_t> {
    if constexpr (Boundary == Path::Typed && Operation == StringOperation::Borrow) {
      return static_cast<std::int64_t>(text->Utf8().size());
    } else if constexpr (Boundary == Path::Typed) {
      auto copied = CopyString(*text);
      benchmark::DoNotOptimize(copied);
      return static_cast<std::int64_t>(copied.size());
    } else if constexpr (Operation == StringOperation::Copy) {
      std::string copied(internal::HandleAccess::Read(*text).AsString()->value());
      benchmark::DoNotOptimize(copied);
      return static_cast<std::int64_t>(copied.size());
    } else {
      return static_cast<std::int64_t>(
          internal::HandleAccess::Read(*text).AsString()->value().size());
    }
  });
}

template <Path Boundary, Element Kind, class Container>
Result<std::int64_t> ReadArrayElement(HandleScope& scope, const Container& values) {
  if constexpr (Boundary == Path::Typed) {
    auto value = values->Get(scope, 0);
    if (!value) {
      return std::unexpected(std::move(value.error()));
    }
    if constexpr (Kind == Element::Scalar) {
      return *value;
    } else {
      return static_cast<std::int64_t>(value->Utf8().size());
    }
  } else {
    const auto value =
        runtime::Heap::GetElement(*internal::HandleAccess::Read(*values).AsObject(), 0);
    if (!value) {
      return std::unexpected(Error{.message = "array read failed", .stack_trace = {}});
    }
    if constexpr (Kind == Element::Scalar) {
      return value->AsInteger();
    } else {
      const auto rooted = internal::HandleAccess::Root<String>(scope, *value);
      if (!rooted) {
        return std::unexpected(rooted.error());
      }
      return static_cast<std::int64_t>(rooted->Utf8().size());
    }
  }
}

template <Path Boundary, Element Kind>
void Arrays(benchmark::State& state) {
  runtime::Isolate isolate;
  HandleScope owner(isolate);
  using Item = std::conditional_t<Kind == Element::Scalar, std::int64_t, String>;
  auto values = CreateArray<Item>(owner);
  auto text = CreateString(owner, std::string(kExpected, 'x'));
  if (!values || !text) {
    state.SkipWithError("array preparation failed");
    return;
  }
  const auto appended = [&] {
    if constexpr (Kind == Element::Scalar) {
      return values->Push(owner, kExpected);
    } else {
      return values->Push(owner, *text);
    }
  }();
  if (!appended) {
    state.SkipWithError("array initialization failed");
    return;
  }
  Measure(state, isolate,
          [&](HandleScope& scope) { return ReadArrayElement<Boundary, Kind>(scope, values); });
}

template <Path Boundary>
void Roots(benchmark::State& state) {
  runtime::Isolate isolate;
  HandleScope owner(isolate);
  const auto text = CreateString(owner, "kept");
  if (!text) {
    state.SkipWithError("root preparation failed");
    return;
  }
  Measure(state, isolate, [&](HandleScope&) -> Result<std::int64_t> {
    if constexpr (Boundary == Path::Typed) {
      auto root = text->Persist();
      if (!root) {
        return std::unexpected(root.error());
      }
      benchmark::DoNotOptimize(*root);
    } else {
      auto root = runtime::HostRoot::Create(isolate, internal::HandleAccess::Read(*text));
      if (!root) {
        return std::unexpected(Error::FromStatus(root.error()));
      }
      benchmark::DoNotOptimize(*root);
    }
    return kExpected;
  });
}

void BoundTypeResolution(benchmark::State& state) {
  runtime::Isolate isolate;
  const auto type = Type<Integers>::Bind(isolate);
  if (!type) {
    state.SkipWithError("type preparation failed");
    return;
  }
  Measure(state, isolate, [&](HandleScope& scope) -> Result<std::int64_t> {
    auto values = CreateArray<std::int64_t>(scope, *type);
    if (!values) {
      return std::unexpected(std::move(values.error()));
    }
    return values->Size() + kExpected;
  });
}

void ColdTypeResolution(benchmark::State& state) {
  double allocations = 0;
  double bytes = 0;
  double peak = 0;
  for (auto iteration : state) {
    state.PauseTiming();
    auto owner = std::make_unique<runtime::Isolate>();
    auto& isolate = *owner;
    auto& heap = isolate.heap();
    const auto baseline = heap.allocated_bytes();
    const auto initial_layouts = isolate.layout_counts();
    heap.ResetAllocationStatistics();
    state.ResumeTiming();
    {
      const auto type = Type<Integers>::Bind(isolate);
      HandleScope scope(isolate);
      if (!type) {
        state.SkipWithError("cold type binding failed");
        break;
      }
      auto values = CreateArray<std::int64_t>(scope, *type);
      if (!values || values->Size() != 0) {
        state.SkipWithError("cold type creation failed");
        break;
      }
      benchmark::DoNotOptimize(*values);
    }
    state.PauseTiming();
    const auto statistics = heap.allocation_statistics();
    allocations += static_cast<double>(statistics.count);
    bytes += static_cast<double>(statistics.bytes);
    peak = std::max(peak, static_cast<double>(statistics.peak_live_bytes - baseline));
    if (!isolate.ReloadBlockers().empty()) {
      state.SkipWithError("cold binding leaked a root");
    }
    heap.Collect();
    if (heap.allocated_bytes() != baseline || isolate.layout_counts() != initial_layouts) {
      state.SkipWithError("cold binding retained heap or type metadata");
    }
    owner.reset();
    state.ResumeTiming();
  }
  const auto iterations = static_cast<double>(state.iterations());
  state.counters["heap_allocations_per_operation"] = allocations / iterations;
  state.counters["heap_bytes_per_operation"] = bytes / iterations;
  state.counters["peak_extra_bytes"] = peak;
  state.counters["external_roots"] = 0;
  state.counters["shutdown_blockers"] = 0;
  state.counters["retained_extra_bytes"] = 0;
  state.counters["scope_storage_bytes"] = static_cast<double>(sizeof(HandleScope));
}

template <Mode Execution, Path Boundary>
void Callbacks(benchmark::State& state) {
  runtime::Isolate isolate;
  auto instance = Instantiate(state, isolate, Execution);
  if (!instance) {
    return;
  }
  HandleScope owner(isolate);
  auto entry = Entry<Callback()>::Bind(*instance, "callback");
  if (!entry) {
    state.SkipWithError("callback entry preparation failed");
    return;
  }
  auto callback = entry->Call(owner);
  if (!callback) {
    state.SkipWithError("callback preparation failed");
    return;
  }
  Measure(state, isolate, [&](HandleScope& scope) -> Result<std::int64_t> {
    if constexpr (Boundary == Path::Typed) {
      return callback->Call(scope, std::int64_t{kInput});
    } else {
      const std::array arguments{runtime::Value::Integer(kInput)};
      const auto result = runtime::ExecuteNativeCallable(
          *instance, internal::HandleAccess::Read(*callback), arguments);
      if (!result.Succeeded()) {
        return std::unexpected(
            Error{.status = result.status, .message = result.message, .stack_trace = {}});
      }
      return result.value.AsInteger();
    }
  });
}

template <Mode Execution, Path Boundary>
void Objects(benchmark::State& state) {
  runtime::Isolate isolate;
  auto instance = Instantiate(state, isolate, Execution);
  if (!instance) {
    return;
  }
  HandleScope owner(isolate);
  auto entry = Entry<Box()>::Bind(*instance, "object");
  auto type = Type<Box>::Bind(*instance);
  if (!entry || !type) {
    state.SkipWithError("object entry preparation failed");
    return;
  }
  auto object = entry->Call(owner);
  if (!object) {
    state.SkipWithError("object preparation failed");
    return;
  }
  Measure(state, isolate, [&](HandleScope& scope) -> Result<std::int64_t> {
    if constexpr (Boundary == Path::Typed) {
      return type->template Get<std::int64_t>(scope, *object, "value");
    } else {
      runtime::Value output;
      auto result = internal::ReadHostMember(
          scope, internal::TypeAccess::Read(*type), internal::HandleAccess::Read(*object), "value",
          runtime::internal::NativeTypeOf<std::int64_t>(), output);
      if (!result) {
        return std::unexpected(std::move(result.error()));
      }
      return output.AsInteger();
    }
  });
}

template <Path Boundary>
void FutureDelivery(benchmark::State& state) {
  runtime::Isolate isolate;
  const auto type = Type<Future<std::int64_t>>::Bind(isolate);
  if (!type) {
    state.SkipWithError("future type preparation failed");
    return;
  }
  Measure(state, isolate, [&](HandleScope& scope) -> Result<std::int64_t> {
    if constexpr (Boundary == Path::Typed) {
      auto pair = CreateCompletion<std::int64_t>(scope, *type);
      if (!pair) {
        return std::unexpected(std::move(pair.error()));
      }
      auto completed = pair->completion.Complete(kExpected);
      if (!completed) {
        return std::unexpected(std::move(completed.error()));
      }
      return pair->future.Wait(scope, WaitMode::WaitForHost);
    } else {
      runtime::FutureCompletionEndpoint endpoint;
      auto future = internal::ReceiveHostValue<Future<std::int64_t>>(
          scope, [&](runtime::Value& output) -> Result<void> {
            auto created = internal::CreateHostCompletion(
                scope, internal::TypeAccess::Read(*type).constraint(), output);
            if (!created) {
              return std::unexpected(std::move(created.error()));
            }
            endpoint = std::move(*created);
            return {};
          });
      if (!future) {
        return std::unexpected(std::move(future.error()));
      }
      auto completed =
          internal::CompletionResult(endpoint.Complete(runtime::Value::Integer(kExpected)));
      if (!completed) {
        return std::unexpected(std::move(completed.error()));
      }
      runtime::Value output;
      auto waited = internal::WaitHostFuture(scope, internal::HandleAccess::Read(*future),
                                             WaitMode::WaitForHost, output);
      if (!waited) {
        return std::unexpected(std::move(waited.error()));
      }
      return output.AsInteger();
    }
  });
}

template <Mode Execution, Path Boundary>
void RegisterEntries(std::string_view mode, std::string_view path) {
  const auto prefix = "interop/" + std::string(mode) + "/" + std::string(path) + "/";
  benchmark::RegisterBenchmark((prefix + "scalar"), ScalarEntry<Execution, Boundary>);
  benchmark::RegisterBenchmark((prefix + "reentry"),
                               ScalarEntry<Execution, Boundary, Call::Reentry>);
  benchmark::RegisterBenchmark((prefix + "local"), LocalEntry<Execution, Boundary>);
  if constexpr (Boundary != Path::Wrapper) {
    benchmark::RegisterBenchmark((prefix + "callback"), Callbacks<Execution, Boundary>);
    benchmark::RegisterBenchmark((prefix + "object"), Objects<Execution, Boundary>);
  }
}

template <Path Boundary>
void RegisterValues(std::string_view path) {
  const auto prefix = "interop/values/" + std::string(path) + "/";
  benchmark::RegisterBenchmark((prefix + "string_borrow"),
                               Strings<Boundary, StringOperation::Borrow>);
  benchmark::RegisterBenchmark((prefix + "string_copy"), Strings<Boundary, StringOperation::Copy>);
  benchmark::RegisterBenchmark((prefix + "array_scalar"), Arrays<Boundary, Element::Scalar>);
  benchmark::RegisterBenchmark((prefix + "array_reference"), Arrays<Boundary, Element::Reference>);
  benchmark::RegisterBenchmark((prefix + "persistent"), Roots<Boundary>);
}

[[maybe_unused]] const int kRegistered = [] {
  RegisterEntries<Mode::Jit, Path::Typed>("jit", "typed");
  RegisterEntries<Mode::Jit, Path::Runtime>("jit", "runtime");
  RegisterEntries<Mode::Aot, Path::Typed>("aot", "typed");
  RegisterEntries<Mode::Aot, Path::Runtime>("aot", "runtime");
  RegisterEntries<Mode::Aot, Path::Wrapper>("aot", "wrapper");
  RegisterValues<Path::Typed>("typed");
  RegisterValues<Path::Runtime>("runtime");
  benchmark::RegisterBenchmark("interop/type/cold", ColdTypeResolution);
  benchmark::RegisterBenchmark("interop/type/bound", BoundTypeResolution);
  benchmark::RegisterBenchmark("interop/async/typed/completion", FutureDelivery<Path::Typed>);
  benchmark::RegisterBenchmark("interop/async/runtime/completion", FutureDelivery<Path::Runtime>);
  return 0;
}();

}  // namespace
}  // namespace vela::benchmarks::interop
