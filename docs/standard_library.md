# Standard library and command-line exits

`Math`, `Random`, `Time`, `Console` and `Error` are default global declarations;
they require no import. Native calls remain statically checked and linked.
Only referenced standard objects contribute declarations and native functions;
ordinary scripts do not load the entire standard environment. These global
names are reserved, while local bindings can shadow them. Internal declaration
sources participate in compilation-cache fingerprints and cannot be supplied
by source overlays. User imports resolve local files; the old `std:` scheme is
removed. Hosts can supply application APIs through the typed native registry;
the CLI clock, console and isolate-local random implementations are its default
host bindings.

```typescript
const result = Math.sqrt(9.0);
const values: int[] = [3, 1, 2];
values.sort((left: int, right: int): int => left - right);
const sum = values.reduce(0, (total: int, value: int): int => total + value);
```

## Console

`Console.readLine(): string?` reads validated UTF-8. It removes LF and an optional
preceding CR, preserves empty lines as `""`, and returns null at EOF. Invalid
UTF-8 and stream failures are catchable standard `Error` values. The last line
does not require a newline. `Console.write(text: string): void` writes without
adding a newline; `Console.flush(): void` flushes explicitly. Existing `print`
uses the same isolate output stream and adds a newline. Interpolation formats
primitive values and null automatically; `Console.write` takes a string, so
use `Console.write('${value}')` when formatting a number.

Hosts can call `isolate.SetConsole(input, output)` with borrowed C++ streams;
both streams must outlive isolate execution. Defaults are `std::cin/std::cout`.
These synchronous calls block on input. They do not introduce language file I/O,
an event loop, or asynchronous timers.

## Time

`Time.monotonicMicros(): int` reads the platform monotonic clock in microseconds;
subtract two readings for elapsed time. Its origin is unspecified. Clock
resolution can be coarser than a microsecond and equal consecutive readings are
valid. `Time.unixMicros(): int` reads wall-clock microseconds since the Unix
epoch; wall time may jump when the system clock is adjusted. There is no implicit
int/double conversion. These distinctions follow mature clock APIs such as
[Python monotonic](https://docs.python.org/3/library/time.html#time.monotonic).

## Random

`Random.seed(value: int): void`, `Random.nextInt(bound: int): int`, and
`Random.nextDouble(): double` use isolate-local xoroshiro128++ 1.0 with SplitMix64
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
nonpositive bounds. `nextDouble` uses 53 random bits and returns `[0, 1)`.
Tests verify independent reference vectors, reseeding and extreme bounds.

## Math

All math inputs and numeric results are `double`; use explicit `toDouble()` or
`toInt()` where needed. Static members of `Math` are `pi`, `e`, `sqrt`, `log`, `exp`, `sin`, `cos`,
`tan`, `floor`, `ceil`, `round`, `trunc`, `abs`, `pow`, `min`, `max`, `clamp`,
`isFinite` and `isNaN`. Binary functions take two arguments; clamp takes
`(value, lower, upper)`. `round` rounds halfway cases away from zero. Basic
transcendentals use the platform C++ math library and may vary in their last bits.

Negative sqrt, nonpositive log, and unordered/NaN clamp bounds raise catchable
errors. Other domain/overflow results retain IEEE NaN/infinity, allowing explicit
`isNaN`/`isFinite` checks. min/max and clamp use fmin/fmax semantics; if exactly
one operand is NaN, the numeric operand wins. No hidden numeric promotions occur.
`Math`, `Random`, `Time` and `Console` expose static APIs and cannot be constructed;
the math constants cannot be assigned by scripts. A host's explicit native
registration takes precedence over a default binding and must match its declared
static signature.

## Collections

Arrays remain fixed-length typed `T[]` with continuous primitive or reference
storage. These methods preserve the element type and nullable contract:

- `copy(): T[]` creates an independent shallow copy.
- `slice(start: int, end: int): T[]` copies `[start, end)`; invalid bounds fail.
- `fill(value: T): void` fills in place.
- `reverse(): void` reverses in place.

Copies use one final allocation and contiguous copying; fill/reverse select
the storage type once and run typed loops. Rooting and write barriers cover
reference elements. Copies of class arrays share the class instances while
owning separate array storage. Zero-length operations are valid.

Array algorithms are instance methods. The element type comes from the receiver;
`reduce` infers its accumulator type from the required initial value:

- `forEach(operation: (T) => void): void`
- `any(predicate: (T) => bool): bool`
- `every(predicate: (T) => bool): bool`
- `findIndex(predicate: (T) => bool): int`, returning -1 if absent.
- `reduce(initial: R, operation: (R, T) => R): R`
- `transform(operation: (T) => T): void`, modifying in place.
- `sort(compare: (T, T) => int): void`, modifying in place.

`Set<T>.union(other: Set<T>)`, `intersection(other: Set<T>)` and
`difference(other: Set<T>)` return an independent new `Set<T>` in left-side
insertion order, followed by unseen right elements for union. They traverse
the hash tables directly without allocating temporary key arrays.

Callbacks are evaluated in traversal order; any/every/findIndex stop as soon as
the result is known. Sorting uses in-place heapsort with worst-case O(n log n)
comparisons and O(1) auxiliary array storage; it is not stable. The comparator
returns a negative/zero/positive int. Callbacks can throw and use normal VM
unwinding through ordinary VM frames; no C++ reentrant VM callback is involved.
Receiver and explicit arguments evaluate left to right once before traversal.
Array length is fixed, traversal visits indices in order, and changes to later
elements are visible to subsequent callbacks. Map/Set lookup, mutation and ordered snapshot APIs remain documented
in [P1 contracts](p1_syntax_proposal.md#ordered-set-and-map).

## Release measurements for the default API migration

Measured on Windows with the repository's Clang Release build. Values below are
wall-time medians of three Google Benchmark repetitions. The new runs use
`--benchmark_min_time=0.2s`; pre-change baselines used `0.1s`. Background system
load was not controlled, so these samples are observations, not a speedup guarantee.

| Workload | Before | After | Managed heap after first execution | Managed heap retained after full GC |
| --- | ---: | ---: | ---: | ---: |
| Compile integer loop | 16.14 us | 17.58 us | — | — |
| Execute integer loop | 20.03 us | 19.05 us | 0 bytes | 0 bytes |
| Standard APIs, 3-element sort and reduction | — | 1.037 us | 256 bytes | 0 bytes |
| Sort 6 integers, reduce 100 times, transform | — | 35.53 us | 6,656 bytes | 0 bytes |
| Build two 256-element sets and perform three set operations | — | 61.24 us | 41,600 bytes | 0 bytes |

The integer-loop register storage stays at 27 bytes. New array workloads use 189
bytes of VM register storage, and the set workload uses 54 bytes. Managed-heap
counters exclude C++ compilation metadata and native bindings. The array workload
creates a fresh reduction callback in each iteration; its initial heap includes
those callback objects. The set workload holds exactly five set objects after
its first execution: two inputs and three results, with no snapshot arrays.

The file-compilation workload changed from 2,963 us to 357 us, but its baseline
wall-time coefficient of variation was about 91%; attributing that difference
to the migration would be unreliable. The unused-standard integer compilation
sample increased by about 1.44 us (9%), while VM execution did not increase in
these samples. Longer, controlled runs are needed to establish a compile-time
regression or a speedup.

Reproduce the current workloads with:

```powershell
python -m tools.vl bench -- --benchmark_filter='(integer_loop|standard_api|array_algorithms|set_operations|cache/library_file_compile)' --benchmark_min_time=0.2s --benchmark_repetitions=3
```

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
