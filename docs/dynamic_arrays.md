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
values.unshift(42);
const first: int? = alias.shift();
const removed: int[] = values.splice(0, 2, 40, 42);
const last: int? = values.pop();
values.clear();
```

`const` prevents rebinding, not mutation. All aliases see length and element
changes, including across growth. `slice()` and the removed elements from `splice()`
have independent storage but continue to share referenced objects. The runnable example is
[`examples/dynamic_arrays.vela`](../examples/dynamic_arrays.vela).

## API and errors

| Member | Result | Behavior and complexity |
| --- | --- | --- |
| `length` | `int` | Read-only live element count; O(1) |
| `capacity` | `int` | Read-only allocated element capacity, at least length; O(1) |
| `push(...items: T[])` | `int` | Append zero or more elements; return the new length; amortized O(k) for k items |
| `pop()` | `T?` | Remove the last element, or return null when empty; O(1) |
| `unshift(...items: T[])` | `int` | Prepend items in argument order; return the new length; O(n + k) |
| `shift()` | `T?` | Remove the first element, or return null when empty; O(n) |
| `splice(start?: int, deleteCount?: int, ...items: T[])` | `T[]` | Replace a range and return removed elements; O(n + k) |
| `slice(start?: int, end?: int)` | `T[]` | Independent shallow copy, defaults to the whole array; O(n) |
| `fill(value: T, start?: int, end?: int)` | `T[]` | Fill a range and return the receiver; O(n) |
| `reverse()` | `T[]` | Reverse in place and return the receiver; O(n) |
| `join(separator?: string)` | `string` | Default comma separator; null contributes empty text |
| `clear()` | `void` | Set length to zero in O(1); retain capacity for reuse |
| `reserve(capacity: int)` | `void` | Ensure at least the requested nonnegative capacity without changing length; O(n) on growth |

`T?` is flattened: popping `int?[]` returns `int?`, not `int??`. An empty array
and a removed null element both produce null. The length of the array returned
by `splice()` distinguishes no removal from removal of a stored null.

`...items` documents positional rest arguments: `push(1, 2, 3)` passes three
values; it does not add general rest-parameter declaration or spread syntax.
All explicit arguments evaluate once, left to right, before the operation.
Named fixed parameters remain supported; rest items must be positional.

`slice` and `fill` default to `[0, length)`. Negative endpoints count from the
end; endpoints clamp to `[0, length]`. An end before start selects an empty range.
`splice` normalizes start the same way and clamps deleteCount to the remaining
length; negative deleteCount means zero. `splice(start)` removes through the end,
while `splice()` removes nothing. `splice(start, 0, ...items)` only inserts.

Invalid indexed access and negative reserves throw `Error`; allocation/capacity failures
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

Push and insertion grow geometrically when needed, with checked length arithmetic.
Only the live prefix is copied. Explicit reserve requests the required capacity
directly. Batch insertion grows once and shifts the suffix once with `memmove`;
it does not box every moved primitive. A single-item push keeps its direct path
without allocation when capacity is sufficient. Multiple rest items are rooted
in a typed array after argument evaluation; removed elements are copied before mutation commits.
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
Push, nonempty pop/shift/clear and nonempty splice changes increment the counter. A
push/pop pair restoring the original length is detected.

Structural mutation raises `Error` with status `ConcurrentModification` and uses
ordinary VM unwinding. The mutation itself remains applied; there is no rollback.
Callbacks may modify existing elements, mutate another array or reserve capacity.
Later element changes remain visible during traversal. Exceptions do not leave
the array locked and the VM remains reusable.

Callbacks receive `(value, index, receiver)`, or
`(accumulator, value, index, receiver)` for reduce, and can omit unused trailing
parameters. `forEach` ignores callback return values. `reduce(callbackFn, initialValue)`
requires an initial value, traverses from index zero and returns the initial value
without invoking the callback when the array is empty.

`sort(compareFn?)` returns the receiver and is stable. Comparators can return int
or double; NaN means equal. The default comparator orders primitive string
representations by UTF-16 code units. Sorting uses a rooted snapshot and original
positions to break ties, then writes the sorted values back. It requires O(n log n)
comparisons and O(n) auxiliary storage. Comparator changes to existing receiver
elements do not change the snapshot. Structural changes still throw Error.

The check's receiver/version argument window is prepared once and reused across
iterations. No callback performs per-iteration argument-window allocation or
creates extra GC objects for this check. Type checking, editor completion, hover
and named argument completion consume the same array-method signatures.

## Deliberate differences from JavaScript

- T[] remains invariant, dense and strongly typed, with checked indexed access
  and read-only length. There is no undefined, sparse slot or implicit coercion.
- Empty pop/shift return null. fill requires a value of T; predicates return bool.
- reduce requires initialValue, even for a nonempty array. Its callback remains
  the first argument; the initial value determines the accumulator type.
- Callback structural mutation throws Error, including a push/pop pair that
  restores the original length. This avoids sparse results and missing values
  incompatible with T[]. Closures replace JavaScript's dynamic thisArg.
- join and default sort accept primitive/nullable elements. Object and nested
  array sorting requires an explicit comparator; join does not invoke implicit
  object conversion. Numeric text uses JS conventions for -0, NaN, Infinity and
  decimal/exponent notation, while int retains Vela's exact 64-bit representation.
- capacity, reserve and clear remain explicit performance extensions. Removed
  copy/insert/removeAt names have no compatibility aliases; use slice()/splice().

This migration aligns existing APIs and adds shift/unshift/splice. It does not
add filter, find, includes, concat, flat or the copying sort/reverse methods.

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
