# Vela runtime

The runtime provides the value, object, memory, and execution contracts shared by JIT and AOT. Build targets are defined centrally in [`BUILD.gn`](BUILD.gn); `//runtime:runtime`,
`//runtime:vela_runtime`, and `//runtime:runtime_tests` own the runtime sources, distributable static
library, and C++ tests. See [Architecture and Contracts](../.agents/architecture.md) for ownership
and module boundaries.

## Directory responsibilities

| Directory | Responsibility |
| --- | --- |
| `value` | Tagged values, object layouts, callables, and value operations and output. |
| `heap` | Heap allocation, GC, compact element and instance storage, and rooted host values. |
| `isolate` | Isolate lifecycle, layout instantiation and transactions, member tables, and external resources. |
| `async` | Futures, completion, composition, scheduling, cancellation, and execution contexts. |
| `collections` | Array, byte, and collection operations and index ranges. |
| `dynamic` | Dynamic values, arrays, calls, and asynchronous dispatch. |
| `strings` | String storage, Unicode operations, and temporary search indexes. |
| `json` | JSON encoding and decoding. |
| `regexp` | Regular expressions and match storage. |
| `library` | Builtin dispatch, standard native bindings, numeric parsing, dates, and random numbers. |
| `exceptions` | Error values and exception payload and trace contracts. |
| `process` | Process entry, shutdown diagnostics, and exit values. |
| `native` | Native ABI and API, host bindings, program instances, dispatch, async execution, and debugging. |
| `testing` | Source-test bridges, value inspection, runtime integration tests, and shared test support. |

Module tests live beside their implementations. Generated native build identity and Unicode data
headers remain in the generated `runtime/` directory. Distribution packages preserve the source
header hierarchy beneath `include/runtime`, including dependencies used by generated C and C++
export headers. Consumers include headers by their owning directory, such as
`runtime/isolate/isolate.h` and `runtime/native/native_api.h`.

## Heap and value contracts

Each `Isolate` owns one `Heap`; there is no process-global heap or locking on the
allocation path.

Closed instances have fixed layouts; reloadable instances retain stable object addresses with
replaceable field storage. Each instance has a `ClassLayout`. The layout records the fields that hold
references, so marking visits reference fields directly instead of inspecting
every field with a dynamic type lookup. Closed-layout field storage is contiguous after the header.

The collector is a precise, non-moving, incremental mark-and-sweep collector.
Roots are registered as `RootRange` objects around native shadow roots, globals, or
host handles. Field writes go through `Heap::SetField`, which applies the write
barrier while marking. The gray work list is intrusive and does not allocate
while a collection is running.

Any object that must survive an allocation safepoint must already be reachable
through a registered root range or another reachable object. Raw `HeapObject*`
and `StringObject*` values are deliberately not implicit roots; generated native code keeps managed values in
registered shadow roots before it calls the allocator.

Small objects use size-class free lists. `Value` is currently a general 16-byte
tagged value for host boundaries and class fields. Small arrays store elements inline; larger arrays use a separately accounted backing buffer.
Array elements use native contiguous storage: booleans occupy one byte; integers,
doubles, and enum ordinals eight bytes; and reference elements one pointer. The full
allocation contributes to heap accounting and collection thresholds.

Each array has an Isolate-owned `ArrayLayout` that identifies its element kind
and, for class or nested-array elements, the exact child layout. Element stores
check bounds and storage type, and reference stores apply the GC write barrier.
Marking visits object- and string-pointer arrays; primitive arrays require no
element scan. Class reference-field metadata includes array and string fields.
Immutable strings store their header and bytes in one accounted allocation and
are reclaimed by the same collector. `AllocateString` copies one or two input
views directly into that allocation; GC-backed inputs must remain rooted across
the call. Strings expose byte views without requiring a trailing NUL byte.
String inputs must be valid UTF-8; native input boundaries can validate with
`InvalidUtf8Offset`. `JoinStrings` computes total size and copies all parts into
one allocation. GC-owned string builders account for their growing native
buffers and release them during sweeping. Their snapshots own independent
immutable storage; see [string contracts](../docs/en/strings.md).

`RootRange` accepts `Value` slots or fixed string-pointer spans. Its
storage and owning Isolate must outlive registration. Hosts and benchmarks can request a full
collection with `Heap::Collect()`, or drive an incremental cycle with `StartCollection`, `Step`,
and `FinishCollection`. Roots are rescanned before
sweep; registration and field/array stores preserve references during both mark
and sweep. Hosts must root returned native execution values before the next allocation or
collection. Unreachable small blocks remain cached for reuse; `allocated_bytes`
counts live allocations, while `committed_bytes` also includes cached blocks.

`Heap::GetElement` returns a tagged value; `Heap::SetElement` stores its
native payload. Both report distinct non-array, bounds, and type errors through
`std::expected`. Raw pointers to element `Value` slots no longer exist.

Double values retain their own `ValueKind::Double` tag; numeric operations and
array stores never convert integer payloads implicitly. Double arithmetic does
not allocate heap objects. `WriteValue` provides the common runtime/CLI output path
and uses shortest round-trip formatting for double values.

The heap is isolate-local and single-threaded. Host producers use explicit completion and safepoint endpoints; ordinary
heap operations and roots remain on the owning Isolate thread.
