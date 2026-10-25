# Growable typed arrays

`T[]` is a dense, mutable, growable array. Its element type remains invariant and
exact; there is no separate `List<T>`, mixed-element array, implicit numeric
conversion, sparse index assignment or writable `length` property.

```vela
const values: int[] = [];
values.reserve(1000);
const alias = values;
for (let index = 0; index < 1000; index = index + 1) {
  values.push(index);
}
values.insert(0, 42);
const first: int = alias.removeAt(0);
const last: int? = values.pop();
values.clear();
```

`const` prevents rebinding, not mutation. All aliases see length and element
changes, including across growth. `copy()` and `slice()` have independent storage
but continue to share referenced objects. The runnable example is
[`examples/dynamic_arrays.vela`](../examples/dynamic_arrays.vela).

## API and errors

| Member | Result | Behavior and complexity |
| --- | --- | --- |
| `length` | `int` | Read-only live element count; O(1) |
| `capacity` | `int` | Read-only allocated element capacity, at least length; O(1) |
| `push(value: T)` | `int` | Append one element; return the new length; amortized O(1) |
| `pop()` | `T?` | Remove the last element, or return null when empty; O(1) |
| `insert(index: int, value: T)` | `void` | Insert before index; require `0 <= index <= length`; shift suffix in O(n) |
| `removeAt(index: int)` | `T` | Remove and return indexed element; require `0 <= index < length`; shift suffix in O(n) |
| `clear()` | `void` | Set length to zero in O(1); retain capacity for reuse |
| `reserve(capacity: int)` | `void` | Ensure at least the requested nonnegative capacity without changing length; O(n) on growth |

`T?` is flattened: popping `int?[]` returns `int?`, not `int??`. An empty array
and a removed null element both produce null. `removeAt()` diagnoses an invalid
index, so it can distinguish absence from a stored null.

Invalid indices and negative reserves throw `Error`; allocation/capacity failures
use the VM's recoverable out-of-memory status. The current representation limits
length/capacity to `uint32_t`, and checks larger requests before allocation.
An invalid type is diagnosed during compilation; failed operations leave the
array's contents, length and mutation version unchanged. Existing checked indexed
access stays bounded by length, even when capacity is larger.

Capacity does not shrink on removal or clear. Exact spare capacity is an
implementation detail. Reserving a smaller capacity is a no-op. Reserve does not
create initialized elements or holes. The backing buffer is freed when the array
is collected or its Isolate is destroyed.

## Representation, GC and performance

The GC header and array identity never move. A small descriptor after the header
owns a data pointer, a 32-bit capacity and a 64-bit structural mutation counter.
Arrays with up to four initial elements use four inline slots in the same
allocation; larger initial arrays use an external contiguous buffer. Growth
migrates the buffer, preserving all array references. Other object headers are
unchanged.

Int/enum elements occupy 8 bytes, doubles 8 bytes, booleans one native bool and
reference elements one native pointer. Nullable reference elements keep the same
pointer width; nullable primitive elements use tagged `Value` slots. Capacity is
measured in elements, independently of element width.

Push and insertion double capacity when needed, with checked length arithmetic.
Only the live prefix is copied. Explicit reserve requests the required capacity
directly. Suffix shifts use `memmove`; they do not box every moved primitive.
Default initialization is limited to live elements of newly created arrays.
Clear need not clear an entire backing allocation because GC never scans spare
slots. Popping/removing clears the vacated last slot.

Both the fixed object allocation and external buffer are included in
`allocated_bytes()` and `committed_bytes()`. Reallocation accounts for replacing
the old external buffer; inline space remains part of the fixed object. Small
object blocks retain the normal GC free-list policy. These counters are not
process RSS or peak memory measurements.

Growth is a GC allocation boundary. The array and a pending inserted reference
are rooted before collection can run; the reference is published with a write
barrier. Marking scans only `[0, length)`, using typed reference spans. Removed
elements and the abandoned inline payload do not keep objects alive. Sweeping
and Isolate destruction release an owned external buffer exactly once.

Host `IntegerElements()` / `StringElements()` spans borrow the current buffer.
The array must stay rooted, and these spans must not cross growth or structural
mutation. VM algorithms keep rooted array values and reacquire elements instead
of retaining raw buffer pointers across callbacks.

## Callbacks and structural mutation

`map`, `forEach`, `reduce`, `some`, `every`, `findIndex` and `sort` capture the
receiver and its structural mutation counter before invoking callbacks. Each
callback result is checked before further array accesses or short-circuit return.
Push, nonempty pop/clear, insertion and removal change the counter. Even a
push/pop pair restoring the original length is detected.

Structural mutation raises `Error` with status `ConcurrentModification` and uses
ordinary VM unwinding. The mutation itself remains applied; there is no rollback.
Callbacks may modify existing elements, mutate another array or reserve capacity.
Later element changes remain visible during traversal. Exceptions do not leave
the array locked and the VM remains reusable.

The check's receiver/version argument window is prepared once and reused across
iterations. No callback performs per-iteration argument-window allocation or
creates extra GC objects for this check. Type checking, editor completion, hover
and named argument completion consume the same array-method signatures.

## Benchmark coverage

The [paired suite](representative_benchmarks.md) now prepares inputs using typed
`reserve`/`push`, replacing its former Set projection. Preparation remains outside
execution timing on both sides.

`array_growth` and `array_growth_reserved` each push 1,024 or 65,536 integers,
read/checksum the array and pop/verify every element. The latter reserves once;
the former grows geometrically. Their C++ counterparts use `std::vector<int64_t>`
with the same operation sequence. Each returns `size * (size - 1)` after checking
pop order. Ordinary compiler/VM, nullable-result checks, allocation and automatic
GC stay inside execution timing.

```powershell
python -m tools.vl bench --compare --report out/benchmarks/dynamic-arrays-2026-10-25.json
```
