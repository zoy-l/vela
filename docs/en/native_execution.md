# Native execution contracts

[**English**](native_execution.md) | [简体中文](../zh-CN/native_execution.md)

Vela executes native code generated from shared Typed IR. JIT provides in-process
compilation; AOT emits linked artifacts. Both use the same values, Isolate, heap,
host bindings, error propagation and asynchronous scheduler.

```text
source -> checked AST -> Typed IR -> shared native generation
                                  -> JIT object linking -> NativeInstance
                                  -> AOT object/linking -> native application
```

## Compilation and tiers

IR optimization propagates constants, removes unreachable blocks and discarded
pure values, and preserves observable failures and side effects. Native lowering
uses the resulting types and liveness to select machine representations and
precise roots. LLVM performs machine-level optimization and register allocation.

`run`, source tests and the standalone debugger compile closed snapshots.
`dev` and the REPL use reloadable development sessions. Baseline and optimized
JIT are native code tiers, with the same language semantics. The optimized
reloadable tier retains baseline recovery entries and state needed for
deoptimization. `NativeOptimization::Release` selects LLVM optimization
independently of the tier; baseline does not mean unoptimized machine code.

Compilation failures remain explicit failures of that request. Shared runtime
objects do not provide an alternative execution backend.

## Instances and calls

`jit::CompileSource/Modules/File` returns a compiled image without creating an
Isolate. The image owns code and immutable layout/source metadata. A
`NativeInstance` creates its receiving Isolate's constants, globals and host
import bindings. Instantiation verifies target/ABI and host signatures before
publishing the instance. Separate instances start with independent global state.

`ExecuteNativeFunction` and `ExecuteNativeCallable` return a value or explicit
execution status with owned diagnostic text and resolved trace frames. The
returned `Value` is borrowed; managed results require a host root before another
allocation. Exported entry invocation additionally enforces the public export
and argument contract. Instance operations use the owning Isolate thread.

Native images are compiler artifacts with native pointers and target-specific
code. They are not an arbitrary-input instruction format or a sandbox. See
[native embedding](native_embedding.md) and [AOT](aot.md).

## Frames, roots and limits

Logical frames preserve source call sites, error propagation and the 1,024-frame
call-depth limit. Native calls also check the current thread's physical stack
boundary and reserve 64 KiB for reporting failure and unwinding. A call can return
`StackOverflow` before the logical limit if the host stack is smaller.

The compiler uses typed frame storage and precise shadow roots for managed values.
Native frame aggregates larger than 4 KiB use fallible runtime storage; this
threshold is not a limit on script local variables. Typed argument storage has
a separate 4 KiB budget checked by code generation. All of these limits are
distinct from source-level integer and array semantics.

There is no 256-register or 256-specialization limit. IR value and metadata
indices use 32-bit identities. Large argument lists, live-value sets and branch
distances are tested through native generation rather than narrowed to byte-sized
operands.

Suspension retains required live values in heap-owned activations and releases
the physical call stack. Completion retires those activations; ordinary calls do
not retain an interpreter register cache. GC remains non-moving and incremental.
Its work budget does not establish a hard real-time latency bound.

## Diagnostics and verification

Runtime errors use immutable source metadata from the executing code version.
Caller positions identify the call instruction. Resolved diagnostic strings
survive code retirement. Stop requests return `ExecutionStopped` without
becoming catchable script errors; normal exceptions follow catch/finally semantics.

Language regression tests cover full int64 and IEEE binary64 behavior, evaluation
order, GC roots, recursion, closures, interfaces, native calls and async completion.
JIT tests additionally cover both tiers, reload publication and retirement.
AOT acceptance compares source programs against JIT and exercises standalone hosts.
Benchmark frame/root metrics describe compiler plans, not measured physical stack
usage or total process memory.
