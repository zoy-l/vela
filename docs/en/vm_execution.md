# VM execution contracts and measurements

[**English**](vm_execution.md) | [简体中文](../zh-CN/vm_execution.md)


This report records the compiler/VM execution refactor measured on Windows x64
on 2026-10-24, using the repository's pinned Clang Release toolchain. It follows
the [basic language milestone](language_basics.md).

## Findings

The encoder checks register widths, signed immediates, and jump offsets before
narrowing. Pool, call-target, selector, and field indexes use an index-extension
word when they exceed their compact operand. The resulting index is uint32;
no global 256-specialization restriction is needed. Jumps, exception ranges,
and source mappings are remapped to final word offsets. Program validation
rejects truncated or misplaced prefixes and jumps into extended instructions.

The other findings were actionable: virtual registers accumulated without
reuse, execution revalidated all functions and allocated a maximum-sized
register file, folding depended on adjacent producers, and two opcode enums
required a hand-maintained mapping with a silent `LoadNull` fallback.

## Compilation and allocation

The production path is now:

```text
checked AST -> typed IR -> basic-block constant propagation
            -> unreachable-block and dead-value elimination
            -> CFG liveness -> physical register allocation
            -> checked encoding -> owned, validated Program -> execution
```

IR virtual register IDs are 32-bit. Backward liveness uses a predecessor
worklist. Dead-value analysis excludes discarded pure definitions from its
transfer, including across block boundaries. Folding tracks integer, boolean,
and null constants through definitions and moves within each basic block;
facts do not cross block boundaries. Known branches are simplified. Failing
arithmetic, indexed reads, calls, allocation, and stores remain observable even
when their results are unused. Computations producing integers outside the
current immediate range retain their arithmetic instructions: `20000 + 20000`
still returns `40000`.

The bytecode allocator uses conservative live intervals and linear scan. A
read phase precedes each instruction's write phase, allowing dying inputs to
share their slot with a result. Parameter positions remain fixed. Call,
builtin, and interpolation arguments retain contiguous windows. Deleting
instructions remaps jump targets; allocation also removes self-moves.

This allocator has no spills or interval splitting. Conservative intervals and
argument-window fragmentation can exceed the slot limit even when an optimal
allocation could fit. Diagnostics report that limit at a source location.
The shared IR optimizer implements jump threading, basic-block copy propagation,
and redundant move elimination. Full SSA propagation and optimal coloring are
not part of the current bytecode implementation.

The IR and VM use one opcode definition in `base/opcode.h`. The old conversion
switch and its `LoadNull` fallback have been removed. IR and runtime operands
still have different contracts; the encoder performs the conversion.

## Program ownership and validation

`vm::Program::Create` takes ownership of mutable builder storage, validates it,
then publishes shared immutable storage. Builders must relinquish mutable
aliases when transferring ownership. `Program::Load` copies raw borrowed
`vm::Function` inputs before validation. It copies string pointers before
registering their stable root range, so registering the pool observes all
references during an incremental collection.

Validation checks function register/parameter counts, opcode operands, pool
references, argument windows, relative jump targets, and terminal fallthrough.
Even unreachable instructions must be structurally valid. Dynamic values and
heap layouts retain runtime type, field, bounds, arithmetic, and allocation
checks; structural validation is not a substitute for those checks.

`FunctionHandle` retains the program, bytecode, metadata, and rooted string
constants. Copies of programs share storage; there is no mutable parallel
`functions/views` representation or view-rebuilding copy path. A default or
moved-from handle is invalid. Its metadata accessors require `IsValid()`;
`Execute` rejects an invalid entry. It also rejects parameterized entry points
and a program belonging to a different Isolate. It does not rescan bytecode.

The Isolate owns heap objects and class/array layouts and must outlive programs,
handles, and roots. Raw loader input must contain live pointers from that
Isolate. Loading, execution, root changes, and destruction use the Isolate's
thread. Returned object/string values are borrowed and must be rooted before
the next allocation or GC. Sharing immutable storage does not make Isolate
operations thread-safe.

## Frames and memory

The VM retains register capacity separately for each call depth. Entering an
inactive depth allocates or grows only that function's required slots. Later
calls reuse the storage after clearing it to null. Each active frame registers
only its actual slot span as a GC root range. Return and error unwinding detach
all frame roots; inactive cached slots are not roots.

The current call-depth limit is 1024. Frame metadata and register files keep
stable addresses as the stack grows. The first frame is inline; deeper frames
are allocated on demand. Native JIT/AOT entries also check the current thread's
physical stack boundary, reserving 64 KiB for failure reporting and unwinding.
They can return `StackOverflow` before reaching 1024 logical frames if the host
thread has insufficient stack space.

This replaces a 262,144-byte maximum register allocation on every execution
(`64 * 256 * sizeof(Value)`, with `sizeof(Value) == 16` on this build). The new
`register_bytes` benchmark counter sums retained native slot capacities after
warm-up. It excludes fixed VM/frame metadata, allocator overhead, program
storage, and GC heap memory. It is neither process RSS nor total VM memory.
The twenty scripts in that measurement retained 48–224 bytes of native register slots.
Capacity persists until VM destruction; peak recursive workloads can still
reach the configured maximum.

Explicit frames avoid recursive C++ interpreter calls. Async suspension,
scheduler ownership, Future completion, cancellation notifications, timer
ordering, and GC roots across suspension are implemented; see
[async contracts](async_tasks.md). Incremental GC work budgets do not establish
a hard real-time latency bound;
root visits, object tracing, and allocation costs still need measurement.

## Encoding limits

| Resource | Current contract |
| --- | --- |
| Physical slots per function | 1–256; register indexes 0–255 |
| Parameters per function | At most 256, subject to slot/window allocation |
| Direct call target | uint32 function index; compact byte operand or index prefix |
| Entry point | 32-bit index; index 256 is tested and supported |
| Class, array-layout, collection, type, selector, global and field indexes | uint32; index prefix when needed |
| Integer, double and string constant indexes | uint32; compact uint16 operand or index prefix |
| Array literal length and interpolation part count | At most 255 per instruction |
| Integer literals | Signed 16-bit immediate or full int64 constant pool |
| Relative branch | Signed 16-bit offset from the following instruction |
| Active call depth | At most 1024 frames; overflow returns `StackOverflow`; native backends also enforce the host thread's physical stack boundary |

Programs can call and instantiate declarations beyond index 255. Register
pressure, literal lengths, and branch ranges retain their separate checks.
Current P2 register/constant representation and
architecture is documented in [VM storage](vm_storage.md).

## Validation and correctness contracts

Validation and regression coverage require:
- Boundary conditions covering byte indexes, immediate and jump ranges, 256 parameters, register allocation limits, and image loading.
- Control flow semantics covering evaluation order, branches, loops, and terminal fallthrough.
- Numeric correctness covering full int64 range, overflow semantics, and IEEE binary64 doubles.
- Execution safety covering observable failures, input snapshot ownership, program/handle retention, cross-isolate execution rejection, and error root cleanup.

## Value and type representation decisions

`Value` remains a tagged variant with full int64 and IEEE binary64 semantics.
NaN-boxing cannot store every int64 plus every double and all other tagged
values in one 64-bit word without restricting values or adding another
representation, such as boxed large integers. It is not an automatic universal
speedup. Typed or unboxed slots need their own compiler, GC, and benchmark
contracts before adoption. This change reduces actual slot counts and removes
unnecessary allocation without changing numeric semantics.

P1 adds structural source type syntax and explicit semantic relationships for
array elements, function signatures, and generic arguments; resolved type
identities still use interned names. Runtime `ArrayLayout` links describe
physical storage and GC scanning and remain necessary. Integer `TypeId`
interning is a possible later representation change. See
[P1 contracts](core_language.md) for the implemented model.
