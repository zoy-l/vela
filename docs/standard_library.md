# Standard library and command-line exits

Standard modules are built into the source loader and use the same explicit
exports, module isolation, generic specialization, and typed native linker as
user modules. They work for files and in-memory compilation. Import aliases are
ordinary module prefixes. Unknown `std:` imports fail; source overlays cannot
replace standard modules and host registration cannot replace `std:` symbols.

```typescript
import "std:math" as math;
import "std:random" as random;
import "std:time" as time;
import "std:console" as console;
import "std:collections" as collections;
```

## Console

`console.readLine(): string?` reads validated UTF-8. It removes LF and an optional
preceding CR, preserves empty lines as `""`, and returns null at EOF. Invalid
UTF-8 and stream failures are catchable standard `Error` values. The last line
does not require a newline. `console.write(text: string): void` writes without
adding a newline; `console.flush(): void` flushes explicitly. Existing `print`
uses the same isolate output stream and adds a newline. Numeric output still
requires explicit `toString()` for interpolation and write.

Hosts can call `isolate.SetConsole(input, output)` with borrowed C++ streams;
both streams must outlive isolate execution. Defaults are `std::cin/std::cout`.
These synchronous calls block on input. They do not introduce language file I/O,
an event loop, or asynchronous timers.

## Time

`time.monotonicMicros(): int` reads the platform monotonic clock in microseconds;
subtract two readings for elapsed time. Its origin is unspecified. Clock
resolution can be coarser than a microsecond and equal consecutive readings are
valid. `time.unixMicros(): int` reads wall-clock microseconds since the Unix
epoch; wall time may jump when the system clock is adjusted. There is no implicit
int/double conversion. These distinctions follow mature clock APIs such as
[Python monotonic](https://docs.python.org/3/library/time.html#time.monotonic).

## Random

`random.seed(value: int): void`, `random.nextInt(bound: int): int`, and
`random.nextDouble(): double` use isolate-local xoroshiro128++ 1.0 with SplitMix64
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
`toInt()` where needed. Exports are `pi`, `e`, `sqrt`, `log`, `exp`, `sin`, `cos`,
`tan`, `floor`, `ceil`, `round`, `trunc`, `abs`, `pow`, `min`, `max`, `clamp`,
`isFinite` and `isNaN`. Binary functions take two arguments; clamp takes
`(value, lower, upper)`. `round` rounds halfway cases away from zero. Basic
transcendentals use the platform C++ math library and may vary in their last bits.

Negative sqrt, nonpositive log, and unordered/NaN clamp bounds raise catchable
errors. Other domain/overflow results retain IEEE NaN/infinity, allowing explicit
`isNaN`/`isFinite` checks. min/max and clamp use fmin/fmax semantics; if exactly
one operand is NaN, the numeric operand wins. No hidden numeric promotions occur.

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

`std:collections` exports generic functions with explicit type arguments:

- `forEach<T>(values: T[], operation: (T) => void): void`
- `any<T>(values: T[], predicate: (T) => bool): bool`
- `every<T>(values: T[], predicate: (T) => bool): bool`
- `findIndex<T>(values: T[], predicate: (T) => bool): int`, returning -1 if absent.
- `reduce<T, R>(values: T[], initial: R, operation: (R, T) => R): R`
- `transform<T>(values: T[], operation: (T) => T): void`, modifying in place.
- `sort<T>(values: T[], compare: (T, T) => int): void`, modifying in place.
- `union<T>`, `intersection<T>` and `difference<T>`, taking two `Set<T>` values
  and returning a new set in left-side insertion order, followed by unseen right
  elements for union.

Callbacks are evaluated in traversal order; any/every/findIndex stop as soon as
the result is known. Sorting uses in-place heapsort with worst-case O(n log n)
comparisons and O(1) auxiliary array storage; it is not stable. The comparator
returns a negative/zero/positive int. Callbacks can throw and use normal VM
unwinding. Map/Set lookup, mutation and ordered snapshot APIs remain documented
in [P1 contracts](p1_syntax_proposal.md#ordered-set-and-map).

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
