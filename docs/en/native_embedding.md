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
`std::string_view`, and typed references from `<vela/host.h>`, including
`Local<String>`, `Local<Array<T>>`, `Local<Function<R(Args...)>>` and
`Local<Future<T>>`. Template arguments are logical types, such as `Array<String>`.
Nullable values use `std::optional` around the corresponding boundary value.
Results also support `void`, owned `std::string`, and `vela::Result<T>`
(`std::expected<T, vela::Error>`). Persistent handles are host storage and cannot
appear in binding signatures. A direct entry requires every conversion in the
adapter to satisfy the direct-call contract; other paths use checked codecs.
There is no implicit int/float narrowing.
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

`HandleScope scope(isolate)` owns local roots. Every copy of a `Local<T>` expires
with its owning scope. Creation, reads and calls receive the destination scope;
managed results are rooted before return. Host imports access their call scope
through `context.handles()`. `local.Persist()` returns
`Result<Persistent<T>>` for storage across calls. Copies share a root, released
by its final owner. `persistent.ToLocal(scope)` creates an independent local
root, allowing the persistent handle to be reset. The Isolate must outlive its
scopes, persistent handles, error payloads, instances and executions.

Bind public script entries with `Entry<Signature>::Bind(instance, name)` and call
them with `entry.Call(scope, args...)`. Development-session entries select the
current publication; `entry.Capture()` retains a specific version. Script-supplied
`Local<Function<Signature>>` values use `callback.Call(scope, args...)` and retain
their closure, receiver and captured code. `Error` owns diagnostics, resolved
frames and an optional rooted script payload; propagating it preserves exception
identity.

Low-level `ExecutionResult::value` remains borrowed, including error payloads.
Internal callers use the fallible `runtime::HostRoot::Create(isolate, value)`
before allocating or collecting. Public hosts should use typed entries to receive
rooted success values and error payloads through the same boundary.

Command-line applications use `ProcessEntry` to wait for the root result, display
it and close that run's background tasks. A pending root waits until completion
or explicit stop, even without a producer. Ordinary embedded calls return typed
results without printing, exiting the host or clearing other tasks. An embedded
host controls its own event loop and chooses whether to wait for returned Futures.
See [program entry and lifetime](../zh-CN/program_lifecycle.md).

Public `Local<Future<T>>` values provide `Read(scope)` and
`Wait(scope, WaitMode::WaitForHost)`. `CreateCompletion<T>(scope)` returns a Future
and a completion endpoint that can be retained across threads. Ordinary Local and
Persistent operations remain confined to the Isolate thread. Closed endpoints
report Closed and no longer retain the Isolate.

A script callable retains its native program state. Invocation must use the same
Isolate and compatible code identity or reloadable dispatch domain. Keeping the
callable rooted keeps the needed code reachable. Host operations are confined to
the Isolate thread except APIs that
explicitly expose cross-thread completion or safepoint endpoints.

Before replacing an Isolate, release external borrows. `ReloadBlockers()` reports
root ranges, rooted values, native instances/handles/entries/candidates, Futures,
active executions and host access in progress. `TryStop()` preserves the current
run when blockers remain. On success it closes completion endpoints, discards
queued work and collects unreachable state; the owner still destroys the Isolate.
`FreezeBindings()` prevents later registration and supplies immutable bindings
for a fresh Isolate. Runtime-owned roots do not remove host rooting obligations.

Generate script public types and member wrappers with
`vela host-header api.vela --namespace=app -o api.h`; generation does not execute
module initializers. `Type<T>` containing nominal types binds an instance or
development session explicitly; builtin types may bind an Isolate. Generated AOT
headers use the same logical types and destination scopes. C local references and
persistent handles have separate ownership types. See the
[C++ interop contract](../zh-CN/cpp_interop.md) for the full mapping and lifetime rules.

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
