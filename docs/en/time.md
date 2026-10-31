# UTC date and time API

[**English**](time.md) | [简体中文](../zh-CN/time.md)

Vela represents instants as signed int Unix timestamps in milliseconds. The
Date static class provides four conversion methods; DateParts is a
read-only UTC calendar snapshot. Calendar conversion reuses the Abseil Time
implementation already included in the repository's build source list.

```vela
func main(): void {
  const timestamp: int = Date.parse('2026-10-26T12:30:45.123+08:00');
  print(Date.toISOString(timestamp)); // 2026-10-26T04:30:45.123Z

  const parts: DateParts = Date.toUTC(timestamp);
  const restored: int = Date.fromUTC(
    year: parts.year,
    month: parts.month,
    day: parts.day,
    hour: parts.hour,
    minute: parts.minute,
    second: parts.second,
    millisecond: parts.millisecond,
  );
  print(restored == timestamp); // true
}
```

The first version covers UTC calendar conversion and a strict, common ISO 8601 /
RFC 3339 date-time subset with explicit numeric offsets. It adds no DateTime or
Duration type, system-local conversion, named time zone, daylight-saving rules,
locale formatting, arbitrary format strings, or calendar arithmetic. Timers and
scheduling belong to the separate [async contract](async_tasks.md). The API is
independent of dynamic, JSON, and regular expressions.

## Public API

| API | Contract |
| --- | --- |
| `Date.now(): int` | Existing wall-clock Unix timestamp in milliseconds |
| `performance.now(): double` | Existing monotonic reading in milliseconds; subtract readings to measure elapsed time |
| `Date.parse(source: string): int` | Parse the supported explicit-offset date-time format and return Unix milliseconds |
| `Date.toISOString(timestamp: int): string` | Format a supported timestamp as canonical UTC text |
| `Date.fromUTC(year: int, month: int, day: int, hour?: int, minute?: int, second?: int, millisecond?: int): int` | Construct Unix milliseconds from validated UTC fields; omitted time fields are zero |
| `Date.toUTC(timestamp: int): DateParts` | Return a new read-only UTC calendar snapshot |

Date remains a non-constructible standard static class. Date and DateParts need
no import and follow existing reserved-name and local-shadowing rules. The
standard environment must include DateParts when Date declarations refer to it,
even if user code never writes the DateParts name. Internal standard sources
participate in overlay protection and compilation-cache fingerprints.

Optional parameters mean omission, not null. Calls retain ordinary source-order
evaluation and positional/named argument checking, including named time fields
with preceding optional fields omitted. No argument is inferred from the current
date, host time zone, or an earlier call. All numeric parameters and fields are
int; int/double conversion remains explicit.

The clock contracts are unchanged: wall-clock adjustments may move Date.now
forward or backward, while performance.now has an unspecified origin and is
for measuring elapsed time. Equal consecutive readings are valid. A monotonic
reading is not a Unix timestamp and is not an input to the calendar API.

## Calendar, precision, and supported range

Use the proleptic Gregorian calendar, years 1 through 9999, with one-based months
and days. Year 99 means 0099, not 1999. Gregorian leap-year rules apply across
the entire supported range. Each Unix day has 86,400 seconds; leap seconds are
not represented.

| Field | Valid range |
| --- | --- |
| `year` | 1..9999 |
| `month` | 1..12 |
| `day` | 1..the actual number of days in that year/month |
| `hour` | 0..23 |
| `minute` | 0..59 |
| `second` | 0..59 |
| `millisecond` | 0..999 |

No field is normalized into another: month 13, February 30, hour 24, second 60,
negative components, and millisecond 1000 are errors. Validate before narrowing
int arguments to backend field types. Abseil civil-time normalization must not
silently change an invalid requested date.

The four conversion methods support the inclusive timestamp range
`[-62135596800000, 253402300799999]`, corresponding to
`0001-01-01T00:00:00.000Z` through `9999-12-31T23:59:59.999Z`. The normalized UTC
instant must be in range, including after applying an input offset. The existing
Date.now contract still returns an int wall-clock reading; it does not impose
this calendar range on a host clock binding.

Negative timestamps are supported. Date.toUTC(-1) represents
`1969-12-31T23:59:59.999Z`; subsecond extraction must use floor-based calendar
decomposition, not a negative remainder. Ordinary int arithmetic does not
automatically check the calendar range; each conversion validates its own input.

## Parsing and canonical output

Accept exactly these ASCII shapes:

```text
YYYY-MM-DDTHH:mm:ssZ
YYYY-MM-DDTHH:mm:ss.SZ
YYYY-MM-DDTHH:mm:ss.SSZ
YYYY-MM-DDTHH:mm:ss.SSSZ
```

Each final Z may instead be a numeric offset `+HH:mm` or `-HH:mm`. T and Z are
uppercase; date/time fields have fixed widths. The offset hour is 00..23 and its
minute is 00..59. Positive offsets are subtracted to obtain UTC. Z and +00:00
both specify UTC; -00:00 is rejected because RFC 3339 uses it for an unknown
local offset. Offset syntax does not load or retain a named time zone.

Fractional seconds are optional and, when present, contain one through three
digits, right-padded to milliseconds. A longer fraction is rejected even when
discarded digits would be zero. No truncation or rounding loses input precision.

Reject omitted offsets, date-only or time-only input, whitespace before/after
the input, lowercase separators, expanded/signed/year-zero dates, week/ordinal
dates, comma fractions, leap seconds, end-of-day 24:00, offset seconds, infinity,
embedded NUL, trailing data, and non-ASCII digits. Calendar fields must also
satisfy the preceding range rules. This is a documented subset, not a promise
to parse every ISO 8601 representation or JavaScript Date.parse convention.

Date.toISOString always returns the 24-character ASCII shape
`YYYY-MM-DDTHH:mm:ss.SSSZ`, including all three fraction digits and a four-digit
year. It does not depend on locale or machine time zone. For every supported
timestamp t, `Date.parse(Date.toISOString(t)) == t`; reconstructing all fields
from Date.toUTC(t) through Date.fromUTC also returns t. Formatting a parsed input
canonicalizes its offset and precision instead of preserving the original text.

## DateParts

| Read-only property | Type / value |
| --- | --- |
| `parts.year: int` | UTC year |
| `parts.month: int` | UTC month, 1..12 |
| `parts.day: int` | UTC day of month |
| `parts.hour: int` | UTC hour |
| `parts.minute: int` | UTC minute |
| `parts.second: int` | UTC second |
| `parts.millisecond: int` | Millisecond within that second, 0..999 |

DateParts is a fixed-layout standard class with seven int fields, not a Map or
an instant object. Its constructor is not public; Date.toUTC publishes a fully
initialized snapshot. Properties are not writable from Vela code.
It supports ordinary references, nullable annotations, fields, captures,
and typed containers, using the normal class/GC model. Each toUTC call returns
a new instance, and ordinary reference-identity equality applies.

There is no implicit conversion to string, timestamp, Map, or JSON. Persist an
instant explicitly as a timestamp or the result of Date.toISOString. DateParts
does not retain a time zone, native pointer, parser buffer, or reference to
backend calendar objects.

## Errors and allocation

| Failure | Contract |
| --- | --- |
| Invalid parse syntax, field, date, precision, or offset | Error with code `DateParseError` and a clear reason |
| Invalid fromUTC component, out-of-range timestamp, or normalized UTC result | Error with code `DateRangeError`, identifying the component or supported range |
| Wrong static argument, DateParts construction, or property write | Frontend diagnostic; checked builtin calls also validate runtime operands |
| Result/error allocation failure | Existing recoverable OutOfMemory path |

A parse input with valid in-range civil fields and an offset that moves the
UTC result outside the supported range produces DateRangeError. A malformed or
out-of-range input field itself produces DateParseError. Errors are catchable
through ordinary VM catch/finally, retain source traces, and do not become NaN,
null, zero, a normalized date, or a generic native failure. Human-readable message
wording is not the program-readable contract; Error.code is.

Parsing and field construction return integers and need no result wrapper.
Formatting returns an ordinary immutable string, and toUTC allocates a normal
class instance. Root source strings, arguments, and new results across allocation
safepoints, and publish destinations only after successful completion. A failed
operation does not expose partial strings or objects or modify its inputs.

## Backend and integration

Reuse [Abseil Time](../../third_party/abseil-cpp/absl/time/time.h), already listed
in the [managed build sources](../../third_party/abseil_sources.gni), including
its UTC civil-time conversion and fixed numeric parsing/formatting support.
The new methods perform no zone-database lookup, local-zone discovery, locale
lookup, or clock read. No additional time library is needed.

Enforce the exact ASCII grammar and millisecond precision at the input boundary,
then use Abseil's validated conversion. Its broader ParseTime acceptance of
lowercase separators, arbitrary fractions, leap-second normalization, and
infinite times must not expand this API. For fromUTC, check numeric component
bounds, construct the backend civil value, and compare normalized fields with
the requested fields so an invalid calendar day fails. Range-check the resulting
instant before conversion to int64 milliseconds. Always pass an explicit UTC
zone to formatting; a fixed internal format such as
`%E4Y-%m-%dT%H:%M:%E3SZ` supplies canonical field widths. Compute subseconds
relative to the start of the UTC civil second, including for negative timestamps.
Use length-aware parsing, not C-string termination or regex dependencies.

Existing [clock native bindings](../../runtime/standard_library.cc) keep their
current responsibilities. Bind the four new conversions through the standard
intrinsic/builtin path so typed DateParts allocation and domain-specific errors
are owned by runtime/VM. The current [native codec](../../runtime/native_function.h)
does not return arbitrary class instances; do not introduce an unchecked Value
return or a general object-binding feature for this record. Carry the checked
DateParts class layout from its standard declaration to the runtime consumer
and validate it at compilation/loading, rather than guessing a layout at use.
No new primitive Value kind or calendar object representation is required.

The protected standard environment supplies the checked signatures and
DateParts dependency closure. Standard static methods carry intrinsic markers
through specialization and lowering into ordinary builtin wrapper functions.
LSP, documentation, and source tools consume the same checked signatures.
Method values have fixed full parameter lists; call-site omission supplies
zero defaults for the four optional time fields.

The Isolate owns the canonical DateParts layout. Compilation binds the checked
standard declaration to it; version 7 images encode and validate the standard
class role and shape. The VM validates the toUTC wrapper's class-pool operand
against this layout. Programs and REPL submissions borrow layouts from their
owning Isolate. See [bytecode images](compilation_cache.md) and the runnable
[`examples/time.vela`](../../examples/time.vela).

## Validation

Required regressions cover epoch zero, -1 millisecond, the earliest/latest
supported instants and one millisecond beyond, years below 100, month lengths,
Gregorian century leap rules, every default, named-argument evaluation order,
and all field/type errors. Parsing cases cover each precision, positive/negative
offsets including day/year changes, normalized-range failures, rejected unknown
offset, absent zone, whitespace/NUL/trailing text, excess precision, and leap
seconds. Verify both round-trip identities, DateParts immutability, fresh object
identity, aliased VM destinations, GC/rooting, domain-error propagation and VM
reuse, tooling, fresh-Isolate images, and persistent REPL lifetimes. Results must
be invariant under host time-zone and locale changes.

Release benchmarks separately measure parsing, canonical formatting, UTC
construction/decomposition, allocations, and regressions in existing clock
calls. See [measurement boundaries and retained results](../../benchmarks/README.md).

The recorded Windows x64 Ryzen 9 7900X Release run measured median steady
latencies of 276 ns for parsing, 438 ns for formatting, 280 ns for fromUTC,
and 338 ns for toUTC, including VM execution. Parse/fromUTC reported zero
first-call GC bytes, formatting 128, and toUTC 256; these are heap-accounted
byte deltas, not allocation counts, and exclude backend/native overhead.
All cases retained zero additional heap bytes after collection. In a separate
matching seven-repetition regression suite, wall/monotonic clock ratios were
0.929/0.891 and three synchronous VM ratios were 0.991..1.022 relative to the
baseline. These are workload-specific measurements, not latency guarantees;
exact variation and settings are retained in
[`rolling_summary.json`](../../benchmarks/results/rolling_summary.json).

## References

- [Existing standard declarations](../../compiler/frontend/builtins/standard_environment.cc) and [standard-library time contract](standard_library.md#time).
- [Abseil civil-time types and normalization](../../third_party/abseil-cpp/absl/time/civil_time.h).
- [RFC 3339](https://www.rfc-editor.org/rfc/rfc3339), including the unknown-local-offset convention; this API deliberately supports only the stated subset.
- Vela [exceptions](exceptions.md), [source tooling](source_tooling.md), and [compilation cache](compilation_cache.md).
