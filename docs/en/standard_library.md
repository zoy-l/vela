# Standard library and command-line exits

[**English**](standard_library.md) | [简体中文](../zh-CN/standard_library.md)


`Math`, `Random`, `performance`, `Date`, `DateParts` and `Error` are default global declarations;
they require no import. Native calls remain statically checked and linked.
Only referenced standard objects contribute declarations and native functions;
ordinary scripts do not load the entire standard environment. These global
names are reserved, while local bindings can shadow them. Internal declaration
sources participate in compilation-cache fingerprints and cannot be supplied
by source overlays. User imports resolve local files; the old `std:` scheme is
removed. Hosts can supply application APIs through the typed native registry;
the CLI clock and isolate-local random implementations are its default
host bindings.

```typescript
const result = Math.sqrt(9.0);
const values: int[] = [3, 1, 2];
values.sort((left, right) => left - right);
const sum = values.reduce((total, value) => total + value, 0);
```

## Output

Output uses `print(value)`, which writes to the
isolate output stream and adds a newline. Interpolation formats primitive values
and null automatically. No built-in `Console` object or input API is provided.

Hosts can call `isolate.SetOutput(output)` with a borrowed C++ output stream,
which must outlive isolate execution. The default is `std::cout`.

## Time

`performance.now(): double` reads the monotonic clock in milliseconds, retaining
fractional milliseconds. Subtract two readings for elapsed time; its origin is
unspecified and equal consecutive readings are valid. `Date.now(): int` reads
integer milliseconds since the Unix epoch; wall time may jump when the system
clock is adjusted. These names and units follow JavaScript's
[performance.now](https://developer.mozilla.org/en-US/docs/Web/API/Performance/now)
and [Date.now](https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/Date/now).
Vela does not expose the full Date or Performance browser interfaces.
The old `Time` object and microsecond APIs are removed. Numeric types remain exact:
use `0.0` in a comparison with `performance.now()`, or explicit numeric conversion.

`Date.parse(source)` and `Date.toISOString(timestamp)` convert strict ISO
date-time text and Unix milliseconds. `Date.fromUTC(year, month, day, ...)`
constructs an instant, and `Date.toUTC(timestamp)` returns a read-only DateParts
snapshot. Calendar conversion supports years 1..9999 and uses explicit UTC,
independent of the host time zone. See [UTC date and time API](time.md) for the
accepted grammar, defaults, ranges, and error codes.

## Numeric parsing

`int.parse` / `int.tryParse` and `double.parse` / `double.tryParse` parse complete
strings without implicit conversion. Invalid text throws a domain Error or
returns null with tryParse; an invalid explicit radix still throws. Integer
results must fit signed int64. See [numeric parsing](numeric_parse.md).

## Byte storage

`ByteArray(length: int)` allocates fixed, zero-initialized raw bytes.
`ByteArray.from(values: int[])` and `bytes.slice(...)` explicitly copy storage.
`ByteData.view(bytes, ...)` retains and shares its owner; typed accessors check
view-relative bounds and integer ranges, including at unaligned offsets.
Multi-byte methods default to `Endian.big`; little and host order are explicit
enum choices. See [byte storage and typed views](byte_data.md).

## Regular expressions

`RegExp(source, ...)` compiles an immutable PCRE2 Unicode pattern. Search methods
return bool, nullable text/matches, or eager typed match arrays. `RegExpMatch`
retains its input and pattern and exposes code-point spans and numbered/named
captures; unmatched captures are null. `RegExp.escape` produces literal pattern
fragments. See [regular expressions](regexp.md) for flags, limits and Error codes.

## Random

`Random.seed(value: int): void`, `Random.nextInt(bound: int): int`, and
`Math.random(): double` use isolate-local xoroshiro128++ 1.0 with SplitMix64
seeding. Every isolate starts with seed zero, and identical seeds and sequences
of calls reproduce results. All 64 seed bits are retained, including negative
int bit patterns. Hosts can seed `isolate.random()` independently. This is a
simulation generator, not a cryptographic API.

The implementation follows the public-domain
[xoroshiro128++ reference](https://prng.di.unimi.it/xoroshiro128plusplus.c) by
David Blackman and Sebastiano Vigna and
[SplitMix64 reference](https://prng.di.unimi.it/splitmix64.c) by Sebastiano Vigna.
It keeps 16 bytes of state and uses unsigned arithmetic and rotations.
`nextInt` samples `[0, bound)` with rejection to avoid modulo bias, and rejects
nonpositive bounds. `Math.random` uses 53 random bits and returns `[0, 1)`.
Tests verify independent reference vectors, reseeding and extreme bounds.

## Math

All math inputs and numeric results are `double`; use explicit `toDouble()` or
`toInt()` where needed. Static members of `Math` are `PI`, `E`, `random`, `sqrt`, `log`, `exp`, `sin`, `cos`,
`tan`, `floor`, `ceil`, `round`, `trunc`, `abs`, `pow`, `min`, `max`, `clamp`,
`isFinite` and `isNaN`. Binary functions take two arguments; clamp takes
`(value, lower, upper)`. `round` rounds halfway cases away from zero. Basic
transcendentals use the platform C++ math library and may vary in their last bits.

Negative sqrt, nonpositive log, and unordered/NaN clamp bounds raise catchable
errors. Other domain/overflow results retain IEEE NaN/infinity, allowing explicit
`isNaN`/`isFinite` checks. min/max and clamp use fmin/fmax semantics; if exactly
one operand is NaN, the numeric operand wins. No hidden numeric promotions occur.
`Math`, `Random`, `performance` and `Date` expose static APIs and cannot be constructed;
the math constants cannot be assigned by scripts. A host's explicit native
registration takes precedence over a default binding and must match its declared
static signature.

## Collections

Arrays are growable typed `T[]` with contiguous primitive or reference
storage. These methods preserve the element type and nullable contract:

- `slice(start?: int, end?: int): T[]` shallow-copies `[start, end)`; defaults to the whole array.
- `fill(value: T, start?: int, end?: int): T[]` fills a range and returns the receiver.
- `reverse(): T[]` reverses in place and returns the receiver.

Range endpoints count from the end when negative and are clamped to `[0, length]`.
An end before start selects an empty range. Mutation methods are documented in
[growable arrays](arrays.md). `copy`, `insert` and `removeAt` are removed;
use `slice()`, `splice()`, `shift()` and `unshift()`.

Copies use one final allocation and contiguous copying; fill/reverse select
the storage type once and run typed loops. Rooting and write barriers cover
reference elements. Copies of class arrays share the class instances while
owning separate array storage. Zero-length operations are valid.

Array algorithms are instance methods. The element type comes from the receiver;
`reduce` infers its accumulator type from the required initial value:

- `forEach(callbackFn: (T, int, T[]) => R): void`, ignoring callback results.
- `some(callbackFn: (T, int, T[]) => bool): bool`
- `every(callbackFn: (T, int, T[]) => bool): bool`
- `findIndex(callbackFn: (T, int, T[]) => bool): int`, returning -1 if absent.
- `reduce(callbackFn: (R, T, int, T[]) => R, initialValue: R): R`
- `map(callbackFn: (T, int, T[]) => R): R[]`, allocating an independent result array.
- `sort(compareFn?): T[]`, modifying in place and returning the receiver.

Callbacks can omit unused trailing parameters. The index is zero-based and the
array parameter is the original receiver. Unlike JavaScript, reduce always
requires an initial value and traverses from index zero. An empty receiver returns
that value without invoking the callback.

Callbacks use the same [contextual typing](type_inference.md) as ordinary function
arguments: `values.sort((left, right) => left - right)`,
`values.some(value => value > 0)`, and `values.reduce((sum, value) => sum + value, 0)`
need no repeated annotations. Reduction derives its accumulator type from `initialValue`,
including when named arguments reorder the arguments.

`Set<T>.union(other: Set<T>)`, `intersection(other: Set<T>)` and
`difference(other: Set<T>)` return an independent new `Set<T>` in left-side
insertion order, followed by unseen right elements for union. They traverse
the hash tables directly without allocating temporary key arrays.

Callbacks are evaluated in traversal order; some/every/findIndex stop as soon as
the result is known. Sorting is stable, with worst-case O(n log n) comparisons
and O(n) auxiliary storage. It sorts a snapshot with original positions as
tie-breakers, then writes back to the receiver. The comparator returns a
negative/zero/positive int or double; NaN compares equal. Without a comparator,
primitive elements are ordered by their string representations using UTF-16 code
units. Object and nested-array elements require a comparator. Callbacks can throw and use normal VM
unwinding through ordinary VM frames; no C++ reentrant VM callback is involved.
Receiver and explicit arguments evaluate left to right once before traversal.
Traversal captures the initial length and visits indices in order. Changes to later
elements are visible to subsequent callbacks. Structural changes to the receiver
from a callback throw Error, even if its original length is restored. Reserving
capacity is permitted. See [growable arrays](arrays.md) for mutation APIs.
Map/Set lookup, mutation and ordered snapshot APIs remain documented
in [P1 contracts](core_language.md#ordered-set-and-map).

`map` allocates one result array at the receiver's initial length, stores each
callback result directly, and preserves the original array. Its element type
is inferred from the callback result, including nullable, reference, and nested
array types. `transform` is removed. Set/Map expose read-only `.size` and
`.delete(key)`/`.delete(value)`; arrays and strings retain `.length`.
Strings use `.includes(text)`.

## API naming conventions

The current names follow JavaScript where the operations have matching roles:

| Previous API | Current API | Contract |
| --- | --- | --- |
| `Time.monotonicMicros()` | `performance.now()` | `double` milliseconds, monotonic |
| `Time.unixMicros()` | `Date.now()` | `int` Unix milliseconds |
| `Math.pi`, `Math.e` | `Math.PI`, `Math.E` | Read-only double constants |
| `Random.nextDouble()` | `Math.random()` | Seeded isolate-local double in `[0, 1)` |
| `array.any(predicate)` | `array.some(predicate)` | Stops at the first true result |
| `text.contains(pattern)` | `text.includes(pattern)` | UTF-8 substring test |
| `set.remove(value)`, `map.remove(key)` | `set.delete(value)`, `map.delete(key)` | Returns whether an entry was deleted |
| `set.length`, `map.length` | `set.size`, `map.size` | Read-only entry count |
| `array.transform(operation)` | `array.map(operation)` | Returns a new `R[]`; infers R from the callback |
| `Console.write`, `Console.flush` | `print(value)` | Output adds a newline |
| `Console.readLine()` | Removed | No built-in input API |

The `Console` object is removed. `Random.seed()` and
`Random.nextInt()` remain explicit seeded-generator extensions. Old API aliases
are not registered. Mapping creates independent array storage; callback-returned
object references are stored directly, without deep copying.

Debug and Release passed 859 C++ tests and 58 tooling tests. Map coverage includes
type-changing callbacks, nullable elements, nested arrays, returned closures,
argument evaluation order, exception unwinding, and collection pressure. A
dedicated allocation check counts only the original array, callback closure, and
one final result array. Clock tests check native millisecond units and exact
int/double representations. Editor protocol tests verify new names and signatures;
real VS Code extension-host tests also passed.

## Process exits

`vela run` uses a successful `main` result as its process exit status: void/null
means zero, and an int in `[0, 255]` supplies that exact code. Other value types
and out-of-range ints are runtime command errors. The result is not printed
automatically; scripts print intended output explicitly. Embedded VM execution
continues to return the typed result without process side effects.

Command usage errors return 64, loading/compilation/entry errors return 65, and
unhandled runtime errors or invalid main exit values return 70. These follow
sysexits conventions on every platform. A script may intentionally return any
code in `[0, 255]`, including those codes. `vela check` returns zero on success.
