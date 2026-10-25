# String performance and memory

Measured on 2026-10-24 on Windows x64 with the repository-pinned Clang Release
toolchain. Google Benchmark reports 24 logical CPUs at 4700 MHz. Times below
are median wall times in microseconds per complete workload, with three
repetitions and at least 0.1 seconds of calibration per repetition.

```powershell
python -m tools.vl bench -- --benchmark_repetitions=3 --benchmark_min_time=0.1s --benchmark_out=.cache/strings-final-benchmark.json --benchmark_out_format=json
```

All 54 registered cases completed without benchmark errors. Every execution
case checked its result. Source loading is outside timing; compile cases
include compilation and destruction. First execution uses a fresh Isolate;
steady execution reuses one compiled program and includes automatic GC. No
builds, tests, or lint ran concurrently with the final measurement. The earlier
exploratory report `.cache/strings-after.json` is superseded by this final run.

## String workloads

| Workload | Compile | First execution | Steady execution |
| --- | ---: | ---: | ---: |
| 1,000 short concatenations through calls | 10.528 | 88.665 | 70.263 |
| 1,000 six-part `+` expressions | 12.631 | 141.280 | 117.726 |
| 1,000 six-part interpolations | 12.438 | 103.900 | 88.297 |
| 1,000 six-part `join` calls | 10.327 | 106.114 | 89.717 |
| 512 one-byte appends using `+` | 10.261 | 33.707 | 24.192 |
| 512 one-byte appends using builder | 11.734 | 21.647 | 20.533 |
| 4,096 one-byte appends using `+` | 9.928 | 239.340 | 227.719 |
| 4,096 one-byte appends using builder | 10.556 | 133.635 | 126.107 |
| 1,000 Unicode API rounds | 11.679 | 248.160 | 249.336 |

The six-part cases create the same `vela-string:42!` text. Inputs are prepared
before the loop. `join` allocates its input array once outside the loop. The
`+` expression makes five strings per round; interpolation and `join` each make
one final string. In this run, interpolation took 25.0% less steady execution
time than `+`, and `join` took 23.8% less. Compiler allocation-count tests also
verify the single final allocation independently of timing.

The append cases include constructing an expected string by doubling and
checking the complete output. Builders took 15.1% less time at 512 appends and
44.6% less at 4,096. Increasing the count eightfold increased builder time
6.1 times and `+` time 9.4 times. Builders use geometric buffer growth; repeated
flat-string `+` still copies a quadratic total number of bytes. These timings
include interpreter overhead and are not isolated memory-copy throughput.

The Unicode case uses `Hello 中😀e\u0301` and validates code point length,
indexing, slicing, search position, and containment on every round. UTF-8
indexing and slicing scan to code point boundaries; the benchmark does not
claim constant-time random indexing. See [string contracts](strings.md).

## Accounted heap memory

Numbers are bytes, not kilobytes. The first two columns are sampled after the
single warm-up execution, before the timed loop. Committed storage after steady
execution includes reusable free-list blocks. For retained storage, the runner
first finishes any active incremental cycle, then starts a fresh collection
without VM frames. This excludes floating garbage marked by an earlier frame.
Both steps run outside timing; the remaining storage consists only of literal pools.

| Workload | Allocated after one execution | Committed after one execution | Committed after steady execution | Retained after GC |
| --- | ---: | ---: | ---: | ---: |
| Six-part `+` | 61,376 | 65,536 | 65,536 | 448 |
| Six-part interpolation | 64,448 | 64,448 | 65,536 | 448 |
| Six-part `join` | 64,640 | 64,640 | 65,664 | 512 |
| 512 appends with `+` | 33,920 | 140,672 | 158,592 | 128 |
| 512 appends with builder | 3,968 | 3,968 | 62,848 | 64 |
| 4,096 appends with `+` | 20,842 | 308,842 | 341,934 | 128 |
| 4,096 appends with builder | 20,960 | 20,960 | 54,720 | 64 |
| Unicode API rounds | 63,104 | 65,536 | 65,536 | 256 |

Allocated bytes include unswept garbage, so a smaller value at the end of an
execution may merely indicate that GC ran later in that workload. Committed
bytes describe the managed allocator and builder buffers, not peak process
RSS; VM registers, compiler metadata, and temporary native search tables are
outside these counters. Builder buffer capacity is included and released on
sweep. Its final 64 retained bytes are the `"x"` literal, not an abandoned
builder buffer. The two literals retained by `+` include its empty string.

Runtime tests check exact external buffer accounting, geometric growth,
snapshot independence, mark/sweep survival, and complete buffer reclamation.
Compiler tests exercise builders through fields, nested arrays, functions,
aliases, temporary conversions, and interpolation at a one-byte GC threshold.
Raw VM tests cover destinations that overwrite rooted inputs and reject
invalid builtin IDs or argument ranges.

## Existing workloads

The baseline `.cache/strings-before.json` was captured before the string changes
with the same Release configuration, repetitions, duration, and workload
boundaries. The short concatenation helper was renamed when the original global
`join` builtin was introduced; its operations and result were unchanged. The
current API is the array instance method `parts.join(separator)`; the global
builtin and its reserved declaration name have been removed.

| Steady workload | Before | Full batch after | Focused after | Focused change |
| --- | ---: | ---: | ---: | ---: |
| Integer loop | 22.351 | 24.197 | 22.188 | -0.73% |
| Function calls | 28.831 | 31.641 | 29.177 | +1.20% |
| Field access | 28.622 | 31.675 | 30.424 | +6.29% |
| Object allocation and GC | 53.259 | 50.182 | 51.280 | -3.72% |
| Array traversal | 407.769 | 423.212 | 422.855 | +3.70% |
| Array allocation | 197.356 | 224.231 | 204.607 | +3.67% |
| Double loop | 27.045 | 25.647 | 25.984 | -3.92% |
| Double array | 437.422 | 424.354 | 432.374 | -1.15% |
| Short string concatenation | 67.837 | 70.263 | 67.972 | +0.20% |
| String growth | 23.840 | 24.192 | 23.931 | +0.38% |

The full batch showed an apparent 13.62% increase in array allocation, while
an earlier batch's array traversal repetitions ranged from 398 to 541
microseconds. To investigate the variation, the current binary was measured
again without compile/first-execution cases, using five repetitions and a
0.2-second minimum. All 17 focused cases passed:

```powershell
python -m tools.vl bench -- --benchmark_filter=^vm/ --benchmark_repetitions=5 --benchmark_min_time=0.2s --benchmark_out=.cache/strings-steady-check.json --benchmark_out_format=json
```

The focused run measured interpolation at 84.729 us, join at 84.704 us, and
six-part `+` at 115.626 us. For 4,096 appends it measured builder at 156.299 us
and `+` at 233.162 us, a 33.0% time reduction. This is less than the full
batch's 44.6% reduction and shows why individual timings should not be treated
as fixed performance guarantees.

The largest focused increase against the saved baseline is 6.29% in field
access; the remaining observed differences are within 3.92%. The baseline was
not remeasured in an interleaved A/B experiment, so scheduling, CPU frequency,
and code layout are not separated from implementation effects. These results
verify actual behavior and costs; they do not establish a causal regression
or claim that existing workloads are completely unaffected. Apparent
improvements in allocation and double workloads are not claimed as string
optimization benefits.

## Validation

Debug and Release each pass 216 tests across five test executables. The repository
lint covers all 63 GN-declared C++ sources and headers with zero diagnostics.
No lint rules or suppressions were added. All 17 examples were checked, and the
15 programs with a `main` were executed. Checking and compilation also share
tests for Unicode import paths, duplicate dependencies, missing files, cycles,
and invalid imported escapes. Raw outputs are under `.cache/strings-*.log`; benchmark
JSON is `.cache/strings-final-benchmark.json`.

## String contexts and array join (2026-10-25)

Global `join` has been removed in favor of `parts.join(separator)`. Interpolation,
string `+`, and primitive array joins format int/double/bool/null directly, while
array element types, numeric operations, assignments, and ordinary arguments
remain strict. Formatting uses native scratch memory; it creates no intermediate
GC strings. String-only concatenation/interpolation and string-array join retain
specialized paths. Nullable primitive arrays use their existing tagged/pointer
storage and format null as `null`.

Windows x64 Release results below are median wall times in microseconds per
1,000 iterations, with three repetitions. The saved pre-change sample used
0.1 s minimum; the final sample used 0.5 s after builds and lint had finished.
These were sequential samples, not interleaved A/B runs.

| Existing workload | Before | After | Observed change |
| --- | ---: | ---: | ---: |
| Integer loop | 17.928 | 18.374 | +2.5% |
| Short string concatenation | 63.859 | 68.417 | +7.1% |
| Six-part string interpolation | 82.599 | 86.755 | +5.0% |
| Six-part string array join | 80.987 | 87.329 | +7.8% |

These increases are observed costs, not evidence of an unaffected hot path;
the experiment does not separate scheduling, frequency, or code layout effects.
Allocation, committed, retained, and register-byte counters for these existing
workloads are unchanged. In particular, join's first-execution allocated and
committed counters remain 64,640 bytes, its register storage is 54 bytes, and
post-collection retained storage is 512 bytes across eight constants.

| New workload | After | Retained bytes | Register bytes |
| --- | ---: | ---: | ---: |
| Direct int/double/bool interpolation | 191.683 | 128 | 99 |
| Equivalent explicit `toString()` interpolation | 214.688 | 128 | 99 |
| Six-integer array join | 178.861 | 128 | 54 |

Direct formatting took 10.7% less time than explicit conversion in this sample.
Compiler allocation-count tests verify that interpolation and each binary
concatenation allocate only their final string. A literal-array join allocates
the array plus the final string; joining an existing array allocates no converted
element strings. Heap counters sampled mid-GC are not cumulative allocation or
peak-memory measurements. Both formatting workloads retain the same two string
constants after collection.

Debug and Release each passed 797 C++ tests and 56 tooling tests. Relevant C++
lint passed without suppressions; the string example executed successfully.
Raw reports: `.cache/string-coercion-before.json`,
`.cache/string-context-verified.json`, `.cache/string-context-debug-final.log`,
and `.cache/string-context-release.log`.
