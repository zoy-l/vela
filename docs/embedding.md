# Native embedding and runtime diagnostics

Vela uses explicit VM frames and recoverable error results. C++ exceptions,
`longjmp`, and implicit numeric coercions are not part of this boundary.

## Typed native functions

Register functions on the owning isolate before compilation:

```cpp
auto Add(vela::runtime::NativeContext&, std::int64_t left,
         std::int64_t right) noexcept -> std::int64_t {
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

An explicitly nullable string return uses `std::optional<std::string>` (or an
expected containing it), and the declaration must return `string?`. Other types
are not promoted to satisfy that return contract. An optional explicit symbol
supports namespaced host registries without language namespace syntax:
`external func readLine(): string? = "std:console.readLine";`.

`external func` has no body or generic parameters. Its symbol is the declared
name before module qualification. Declarations follow ordinary module visibility
and aliases; the host registry is isolate-wide. Compilation links each declaration
to an owned immutable binding and diagnoses unregistered symbols and signature
mismatches. Ordinary source checking can check external declarations without
executing or registering host code. Native function values support indirect calls.
Duplicate registrations fail rather than replacing existing contracts.

The VM executes a native invocation inside its own explicit frame, so failures
show both the external declaration and the script call site. Name lookup happens
at linking; a VM-owned argument buffer is reused and grows only for a newly larger
native call window. Arguments are checked at the
native input boundary and a single intrusive root range protects the borrowed
argument window while the host runs. Returned strings are validated as UTF-8 and
copied into the isolate heap. Invalid text is an error, not replacement text.

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

## Source mappings and errors

Every compiled instruction retains its source identity, line and column through
optimization and register allocation. Position mappings store only changes, and
functions share one immutable source-path table. Lookup uses binary search and
only runs when a trace or tooling requests it. Bytecode loading snapshots and
validates external mappings, including ordering, instruction bounds and source
identity. Bytecode without mappings remains a valid source-independent program.

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
