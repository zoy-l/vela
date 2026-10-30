# Native embedding and runtime diagnostics

[**English**](native_embedding.md) | [简体中文](../zh-CN/native_embedding.md)


Vela uses explicit VM frames and recoverable error results. C++ exceptions,
`longjmp`, and implicit numeric coercions are not part of this boundary.

## Typed native functions

Register functions on the owning isolate before compilation:

```cpp
std::int64_t Add(std::int64_t left, std::int64_t right) noexcept {
  return left + right;
}

vela::runtime::Isolate isolate;
auto registered = isolate.RegisterNative(
    vela::runtime::NativeFunction::Bind<Add>("add"));
```

The script declares the binding explicitly:

```typescript
external func add(left: int, right: int): int;

func main(): int {
  const operation: (int, int) => int = add;
  return operation(40, 2);
}
```

The C++ signature supplies the complete binding contract. Supported parameters
are `std::int64_t`, `double`, `bool`, and borrowed `std::string_view`. Return
types additionally include `void` and owned `std::string`. A function may return
`std::expected<T, NativeError>` for a recoverable host failure. Functions must
be `noexcept`; host arithmetic and resource failures remain the host's
responsibility. An `int`, `float`, nullable value, or differently shaped signature
is never silently converted to match a binding.

Heap limits and `OutOfMemory` apply to actual storage requests. Native optimization
may remove unobservable instance storage or delay allocation until heap identity
is required. Hosts cannot assume that every script constructor increases heap
counters or that the VM and native backends fail at the same constructor. Objects
passed through the host ABI always have ordinary heap identity and follow the
existing borrowing and rooting rules. See
[escape analysis and materialization](../zh-CN/escape_analysis.md).

Functions that need the Isolate, allocation or asynchronous operations take
`NativeContext&` as their first parameter. Ordinary C++ functions may omit it;
`Bind` derives the same script signature in either form. In AOT, plain scalar
results return in registers. Exact scalar signatures call the original function
pointer directly, while codecs and fallible results retain the required adapters.

An explicitly nullable string return uses `std::optional<std::string>` (or an
expected containing it), and the declaration must return `string?`. Other types
are not promoted to satisfy that return contract. An optional explicit symbol
supports namespaced host registries without language namespace syntax:
`external func readLabel(): string? = "app.readLabel";`.

`external func` has no body or generic parameters. Its symbol is the declared
name before module qualification. Declarations follow ordinary module visibility
and aliases; the host registry is isolate-wide. Compilation links each declaration
to an owned immutable binding and diagnoses unregistered symbols and signature
mismatches. Ordinary source checking can check external declarations without
executing or registering host code. Native function values support indirect calls.
Duplicate registrations fail rather than replacing existing contracts.

Native declarations contain an immutable binding and no wrapper bytecode. Direct
and resolved indirect calls borrow the caller's rooted register window; the
adapter reads typed payloads in place. They allocate no callee register file or
intermediate `Value` argument buffer and register no duplicate argument roots.
Dynamic argument binding prepares its own rooted parameter window. A lightweight
logical frame preserves the external declaration in diagnostics and debugger
callbacks, including when the host triggers collection. Name lookup and arity
validation happen at linking/loading. Payload types are checked by the generated
binding adapter because structural bytecode validation does not prove value types.
Returned strings are validated as UTF-8 and copied into the isolate heap. Invalid
text is an error, not replacement text. Native Future results retain declaration
validation for successful payloads and error values, including pending completion.

The low-level `NativeFunction::Invoke` borrows a `RegisterView`: its caller must
provide the exact declared argument count and keep that window rooted throughout
the invocation. Reference results must be rooted before subsequent allocation.
Reentering the same VM uses independent register storage, preserving the outer
invocation's slots and borrowed strings across nested execution and collection.

## Host roots and lifetime

`runtime::RootedValue(isolate, value)` owns a stable heap root slot. Copies share
that immutable slot; moving or relocating the handle cannot invalidate the root
address. The last handle releases the root. Assigning a new handle replaces the
old root through ordinary RAII. The isolate must outlive every program, function
handle, root range, and rooted value that belongs to it. These APIs share the
isolate's single-thread ownership; they do not introduce shared concurrent access.

A VM result's bare `Value` is borrowed from the heap. If it contains a heap
reference, create a `RootedValue` before any subsequent allocation or collection.
Reading `handle.value()` does not create another root. Native parameters remain
rooted only for the duration of invocation; persistent host references need their
own `RootedValue`.

Before replacing an Isolate, the host stops creating new borrows and releases its
external handles. `Isolate::ReloadBlockers()` lists remaining roots, native instances,
opaque native handles, named declaration entries, prepared native publications,
Programs, Futures, active execution and host releases still in progress. Each entry
identifies the resource kind, its identity within that Isolate, and its reference count.
`TryStop()` returns the same diagnostics without changing the
running state when resources remain. On success it closes completion endpoints,
discards queued callbacks and timers, and collects unreachable state. It does not
destroy the Isolate; the owner releases its runtime storage before destroying it.
Runtime roots use `RootOwner::Runtime`; ordinary host `RootRange` instances are
external by default. Internal retained handles are reserved for runtime ownership,
and do not remove the host's obligation to release its own borrows.

`Isolate::FreezeBindings()` captures the fixed native registration set and prevents
further registration with `NativeRegistrationError::BindingsFrozen`. Constructing a
new Isolate from that `NativeBindings` snapshot shares the same immutable binding
implementations and signatures while creating independent script state.

Script callable values refer to function indices in their producing Program.
Invocation requires the producing code identity, shared across REPL submissions; independently compiled
programs cannot invoke each other's callable values received through native Future payloads. Such
invocations fail with a type error. Retaining a callable's heap value preserves
its type identity; retain the producing Program or FunctionHandle to execute its code.

## Source mappings and errors

Every compiled instruction retains its source identity, line and column through
optimization and register allocation. Position mappings store only changes, and
functions share one immutable source-path table. Lookup uses binary search and
only runs when a trace or tooling requests it. Bytecode loading snapshots and
validates external mappings, including ordering, instruction bounds and source
identity. Bytecode without mappings remains a valid source-independent program.
Native declarations use logical position zero for their source mapping and
debugger callback; they have no executable bytecode instructions.

Runtime failures capture all live explicit frames, innermost first. Caller PCs
identify the call instruction rather than the following instruction. Trace text
owns its strings, so diagnostics survive destruction of the compiled program.
Compiler and runtime errors share `path:line:column: error: message`; runtime
errors append `at function (path:line:column)` frames. Bytecode without symbols
reports the instruction offset. Successful executions allocate no trace.

Implementation references: Dart's external-function declaration model
([language specification](https://spec.dart.dev/DartLangSpecDraft.pdf)), Luau's
explicit-frame line lookup (`C:/Users/zoy/Documents/luau/VM/src/ldebug.cpp`), and
its protected execution (`VM/src/ldo.cpp`). Vela keeps C++ RAII unwinding intact
by returning explicit failures instead of jumping across host stack frames.
