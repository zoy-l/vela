# Native embedding and runtime diagnostics

[**English**](native_embedding.md) | [简体中文](../zh-CN/native_embedding.md)

A source-embedding host uses `compiler::jit` to compile native code, then creates
a `runtime::NativeInstance` in its Isolate. AOT hosts link emitted code with the
same runtime. Source builds link `//compiler:jit`; standalone AOT hosts use the
distribution's runtime library and generated public header. The runtime alone
does not include a source compiler.

## Compile and execute

This example returns `42` through a typed host binding:

```cpp
#include <cstdint>
#include <expected>
#include <string>

#include "compiler/frontend/diagnostic.h"
#include "compiler/jit/compiler.h"
#include "compiler/jit/source_compiler.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_execution.h"
#include "runtime/native/native_function.h"
#include "runtime/native/native_instance.h"

std::int64_t Add(std::int64_t left, std::int64_t right) noexcept {
  return left + right;
}

std::expected<std::int64_t, std::string> Run() {
  vela::runtime::Isolate isolate;
  if (!isolate.RegisterNative(vela::runtime::NativeFunction::Bind<Add>("add"))) {
    return std::unexpected("cannot register add");
  }
  auto compiler = vela::compiler::jit::Compiler::Create();
  if (!compiler) {
    return std::unexpected(vela::frontend::DiagnosticsText(compiler.error()));
  }
  const auto compiled = vela::compiler::jit::CompileSource(**compiler, R"(
external func add(left: int, right: int): int;
func main(): int { return add(40, 2); }
)");
  if (!compiled.Succeeded()) {
    return std::unexpected(vela::frontend::DiagnosticsText(compiled.diagnostics));
  }
  const auto instance =
      vela::runtime::NativeInstance::Instantiate(isolate, compiled.code.image);
  if (!instance) {
    return std::unexpected(instance.error().message);
  }
  const auto entry = (*instance)->FindFunction("main");
  if (!entry) {
    return std::unexpected("main is missing");
  }
  const auto result = vela::runtime::ExecuteNativeFunction(**instance, *entry);
  if (!result.Succeeded()) {
    return std::unexpected(result.message);
  }
  return result.value.AsInteger();
}
```

The image is independent of the AST and compiler lifetime. Its shared owner retains
linked code and metadata; native instances and retained native program state keep
that owner alive. Separate instances have independent globals. Use
`DevelopmentSession` and `DevelopmentRepl` for state-preserving updates, rather
than replacing a closed instance's code. See [hot reload](../zh-CN/hot_reload.md).

## Typed host bindings

Register bindings before instance creation. `NativeFunction::Bind<Function>`
derives the signature from a `noexcept` C++ function. `NativeContext&` may be the
first parameter when the function needs its Isolate, allocation or asynchronous
operations; it is omitted from the script signature.

Supported parameters include `std::int64_t`, `double`, `bool`,
`std::string_view`, `Future` and `CancellationToken`. Results also support
`void`, owned `std::string`, nullable `std::optional<std::string>`, and
`std::expected<T, NativeError>`. Exact scalar results use direct calls; managed
and fallible results use checked codecs. There is no implicit int/float narrowing.
Host arithmetic and resource failures remain the host's responsibility.

An external declaration has no body or generic parameters. An explicit symbol may
name a registry entry, for example
`external func label(): string? = "app.label";`.
Instantiation checks the declaration against the receiving Isolate's immutable
binding. Missing bindings, mismatched signatures and duplicate registrations are
errors. Source checking does not require registration or execute host code.

Generated calls retain logical frames and precise roots while the host runs.
Borrowed strings are valid only for the call; persistent values need host roots.
Returned strings and error messages are checked for UTF-8. Future results retain
their declared payload checks through pending completion. Nested script entry
uses independent native execution state and preserves the outer call's roots.

Allocation elimination may remove unobservable object storage. Heap limits apply
to actual storage requests, so source constructors are not allocation counters.
Objects that cross the host ABI have heap identity and follow the same rooting
rules as other managed objects.

## Values, futures and shutdown

`ExecutionResult::value` is borrowed, including error payloads. Before another
allocation or GC, retain a managed result with `RootedValue(isolate, value)`.
Copies share one immutable root slot; the final handle releases it. A read through
`value()` does not create a new root. The Isolate must outlive instances,
executions, rooted values and other handles belonging to it.

For asynchronous results, `RootFutureValue` creates a rooted Future handle and
the host pumps the owning scheduler. A host waiting for external producers uses
the scheduler's `WaitForHost` mode. Command-line applications use `ProcessEntry`
to apply exit-code rules and drain background work; an embedded host controls
its own event loop.

A script callable retains its native program state. Invocation must use the same
Isolate and compatible code identity or reloadable dispatch domain. Keeping the
callable rooted keeps the needed code reachable; no separate bytecode handle is
required. Host operations are confined to the Isolate thread except APIs that
explicitly expose cross-thread completion or safepoint endpoints.

Before replacing an Isolate, release external borrows. `ReloadBlockers()` reports
root ranges, rooted values, native instances/handles/entries/candidates, Futures,
active executions and host access in progress. `TryStop()` preserves the current
run when blockers remain. On success it closes completion endpoints, discards
queued work and collects unreachable state; the owner still destroys the Isolate.
`FreezeBindings()` prevents later registration and supplies immutable bindings
for a fresh Isolate. Runtime-owned roots do not remove host rooting obligations.

## Diagnostics

Native frames retain source locations from the code version actually executing.
Caller locations identify calls; asynchronous errors preserve throw and await
boundaries. Error results own resolved function names, paths and text so they
survive code retirement. Successful execution does not capture a trace.

Compiler and runtime errors use `path:line:column: error: message`; runtime
diagnostics append `at function (path:line:column)`. Missing source metadata is
reported through instruction positions. C++ exceptions and nonlocal jumps are
not part of this boundary: failures propagate as explicit results while preserving
host RAII. See [debugging](debugger.md) and [execution contracts](native_execution.md).
