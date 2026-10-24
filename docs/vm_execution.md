# VM execution contracts and measurements

This report records the compiler/VM execution refactor measured on Windows x64
on 2026-10-24, using the repository's pinned Clang Release toolchain. It follows
the [basic language milestone](language_basics.md).

## Findings

The earlier encoder already checked every non-jump `instruction.index` against
the 8-bit range before any narrowing. Calling function index 256 therefore
produced a diagnostic; it did not silently wrap to function zero. The same
guard covered constant, layout, and field indexes. Signed immediate and jump
offset checks also existed. New regression tests exercise those boundaries.

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
Full SSA propagation, jump threading, general move coalescing, and optimal
coloring are not implemented.

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

This replaces a 262,144-byte maximum register allocation on every execution
(`64 * 256 * sizeof(Value)`, with `sizeof(Value) == 16` on this build). The new
`register_bytes` benchmark counter sums retained native slot capacities after
warm-up. It excludes fixed VM/frame metadata, allocator overhead, program
storage, and GC heap memory. It is neither process RSS nor total VM memory.
The current twenty scripts retain 48–224 bytes of native register slots.
Capacity persists until VM destruction; peak recursive workloads can still
reach the configured maximum.

Explicit frames avoid recursive C++ interpreter calls, but do not by themselves
implement async. Suspension, scheduler ownership, completion, cancellation,
timer ordering, and GC roots across suspension remain separate work. Likewise,
incremental GC work budgets do not establish a hard real-time latency bound;
root visits, object tracing, and allocation costs still need measurement.

## Encoding limits

| Resource | Current contract |
| --- | --- |
| Physical slots per function | 1–256; register indexes 0–255 |
| Parameters per function | At most 256, subject to slot/window allocation |
| Direct call target | Function index 0–255 |
| Entry point | 32-bit index; index 256 is tested and supported |
| Class, string, array-layout, double, field indexes | 0–255 in encoded instructions |
| Array literal length and interpolation part count | At most 255 per instruction |
| Integer literal immediate | Signed 16-bit; integer arithmetic remains checked int64 |
| Relative branch | Signed 16-bit offset from the following instruction |
| Active call depth | At most 64 frames; overflow returns `StackOverflow` |

Unreferenced declarations do not require a blanket 256-function restriction.
Programs with more functions can execute an entry beyond the call operand
range, provided their encoded direct calls remain representable. Limits are
diagnosed before narrowing; widening the instruction format and generalizing
the integer constant pool remain future work.

## Validation evidence

- Debug and Release each passed all 353 GoogleTest cases, with no compiler
  warnings. After the final validator-only refactor, the VM suite was rebuilt
  and all 29 cases passed again in each configuration.
- The full lint run covered 85 C++ sources/headers. Its last two findings were
  corrected; the affected sources and final validator changes passed a targeted
  three-file recheck with zero failures. No analysis rules were weakened.
- All 19 example sources passed CLI checking; all 17 entry programs executed.
  Switch output remains `close\n121\n`; nullable output remains
  `label: Vela\nempty\n42\n`. Imported errors preserve `broken.vela:2:10`.
- Boundary tests cover byte indexes, immediate and jump ranges, 256 parameters,
  true register pressure, and raw loading with 256 versus 257 register slots.
  A straight-line function with 1,000 calls returns `500500` in at most eight
  registers. A 256-field object initializes and reads in at most three.
- Tests also cover evaluation order, branches, loops, int64 arithmetic,
  observable unused failures, input snapshot ownership, program/handle
  retention, wrong-Isolate execution, error root cleanup, and register reuse.

## Release measurements

Google Benchmark 1.9.5 ran with `--benchmark_min_time=0.1s` and three
repetitions. The baseline contains fifteen matching cases from before the
refactor; the after run contains all 63 current cases. All completed without
`error_occurred`; first and steady execution verify status and result. No
tests, builds, or lint ran concurrently with timed benchmark execution.

Values below are median wall-clock microseconds per complete workload.

| Workload | Compile before → after | First execute before → after | Steady before → after | Steady change | Native register bytes after |
| --- | ---: | ---: | ---: | ---: | ---: |
| Integer loop | 5.886 → 10.991 | 29.148 → 19.152 | 24.182 → 17.966 | −25.7% | 48 |
| Function calls | 7.136 → 11.354 | 34.951 → 26.299 | 29.131 → 26.159 | −10.2% | 64 |
| Field access | 8.414 → 15.175 | 32.773 → 25.027 | 29.920 → 24.451 | −18.3% | 64 |
| Array traversal | 12.672 → 27.400 | 412.987 → 399.974 | 410.622 → 397.719 | −3.1% | 80 |
| Double loop | 6.977 → 14.566 | 27.048 → 20.643 | 25.355 → 20.197 | −20.3% | 48 |

Compilation is more expensive: these cases increased by 59–116%, including
optimization, CFG liveness, physical allocation, and validation. That cost is
paid at compilation/loading. First execution now excludes bytecode validation
and includes initial register allocation; steady execution reuses capacity.
These are the complete production pipelines, not isolated measurements of any
one change. The baseline and current runs were sequential, not interleaved
A/B, so the percentages describe these runs rather than guaranteed speedups.
The after run's largest wall-time coefficient of variation was 9.84% in
`compile/symbol_tables/1024`; short timing samples warrant care.

The baseline JSON is `.cache/execution-before.json`; the after report is
`.cache/execution-after.json`. Reproduce the complete after suite with:

```powershell
python -m tools.vl bench -- --benchmark_min_time=0.1s --benchmark_repetitions=3 --benchmark_out=.cache/execution-after.json --benchmark_out_format=json
```

For the original fifteen cases add
`--benchmark_filter='^(compile|first_execute|vm)/(integer_loop|function_calls|field_access|array_traversal|double_loop)$'`.
The same machine and Release configuration are required for a useful comparison.

## Value and type representation decisions

`Value` remains a tagged variant with full int64 and IEEE binary64 semantics.
NaN-boxing cannot store every int64 plus every double and all other tagged
values in one 64-bit word without restricting values or adding another
representation, such as boxed large integers. It is not an automatic universal
speedup. Typed or unboxed slots need their own compiler, GC, and benchmark
contracts before adoption. This change reduces actual slot counts and removes
unnecessary allocation without changing numeric semantics.

Frontend array identity currently uses interned names and recorded checked
types; runtime `ArrayLayout` links describe physical element storage and GC
scanning. Those runtime layouts remain necessary even with structural types.
A structural `TypeId`/interner is a future evolution for generics and types such
as `Task<T>`, not implemented here. No new compatibility type layer was added.
