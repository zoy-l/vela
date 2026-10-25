# P2 acceptance and performance

This report records the initial P2 acceptance snapshot on the repository's
Windows x64 Clang/LLVM toolchain. Counts and measurements below describe that
snapshot; later changes are validated in their task logs. It records actual
execution, including costs and limits, and does not claim that every workload
became faster. Existing P1 changes remain in the working tree.

## Delivered paths

Modules, typed errors and stack traces, native bindings and host
roots, standard libraries and exit codes, register storage and optimizations,
validated bytecode and content caches, formatter, LSP/editor integration,
debugger, persistent REPL and documentation generation are connected through
their production producers and consumers. The [implementation ledger](p2_implementation.md)
links their detailed contracts. There are no added language file APIs, implicit
conversions, C++ namespace declarations, global PATH changes or VerseGame dependencies.

The REPL preserves globals, closure targets/cells, interface selectors, runtime
layouts and accepted module snapshots. Only new input executes. Compilation
failures roll back IDs, snapshots and allocated layouts; runtime failures retain
completed side effects. A single entry slot is reused across hundreds of prompts.

Acceptance found and fixed a Windows concurrent-rename race. Publication now uses
POSIX replacement flags and properly terminated UTF-16 names, while readers share
deletion and retain complete snapshots. Filename alignments and concurrent readers/
writers were exercised repeatedly. Source/package loading uses that same read boundary.

## Verification

- Debug: **762 C++ tests and 26 process/tool tests passed**.
- Release: **762 C++ tests and 26 process/tool tests passed**.
- VS Code client: **3 Node tests passed**, including a real LSP server session.
- Static analysis: all **257 build-listed C++ sources/headers** were scanned;
  findings were fixed and affected files/headers rechecked with zero remaining
  warnings. `.clang-tidy` rules were not weakened and no `NOLINT` was added.
  The documented Windows SDK prerequisite include retains architecture/type setup;
  direct declaring SDK headers are still included.
- All **36 examples** checked; **32 entry points** actually executed without source
  or runtime errors. Programs returning integer demonstration results retain their
  defined exit codes. Eight older examples now print results from a void `main`.
- Formatting was idempotent on **67 example/benchmark sources**, without rewriting
  other source files. Exported API documentation was actually generated.
- The VSIX was packaged; image compile/exec, corruption handling, Unicode paths,
  debugger stepping/breakpoints and REPL recovery were tested as real processes.
- UTF-8/LF, final newlines, local documentation links and `git diff --check` passed.

Final test logs and generated API output are under `.cache/p2-final-*.log` and
`.cache/p2-api.md`. Coverage includes encoding/loading bounds, GC pressure, native
signature mismatches, exception/finally behavior, cache invalidation and failed
layout transactions. The rename reproducer passed 30 repetitions; the expanded
file/snapshot tests passed 20 repetitions before the final full regressions.

## Release performance

Google Benchmark ran **109 cases with no errors**: three repetitions and 0.1-second
minimum per case. After the final numeric conversion optimization, **all 30 VM
cases** were rerun with five repetitions and a 0.3-second minimum, also with no
errors. No builds, tests or lint ran alongside benchmark execution.

The original P2 baseline contains 87 matching cases. Their medians and report
provenance are preserved in [the complete comparison CSV](measurements/p2_summary.csv).
The final VM rerun supplies VM rows; the full-suite run supplies other rows.
These runs used one host. Short runs showed substantial variance, so suspicious
cases were also measured with seven repetitions and 0.3-second minimums.

Median wall time, microseconds per complete workload:

| VM workload | Initial P2 | Final | Change |
| --- | ---: | ---: | ---: |
| Integer loop | 19.55 | 18.36 | -6.0% |
| Direct calls | 28.61 | 30.01 | +4.9% |
| Closure calls | 54.24 | 53.66 | -1.1% |
| Field access | 26.46 | 26.18 | -1.1% |
| Allocation/GC | 42.90 | 46.07 | +7.4% |
| Explicit numeric conversions | 32.04 | 34.87 | +8.9% |
| Unicode string workload | 201.37 | 224.26 | +11.4% |

The 28 matching VM cases have a **3.9% higher geometric-mean time**. This remains
a measured cost, not an all-workload speedup. Compiler medians also increased:
the 1,024-group symbol workload measured 17.78 ms versus 16.96 ms (+4.8%), and
the small integer-loop compilation measured 16.70 us versus 14.17 us (+17.8%).
The complete short-run CSV includes larger spikes. For example, interface compilation
showed +104.9% in the short full run, but its longer seven-repetition audit measured
33.28 us versus 31.29 us (+6.4%). The audit is retained separately rather than
silently replacing an inconvenient measurement. Set/Map/match compilation audits
showed +9.0%, +14.8% and +9.3%. Further profiling is needed to separate remaining
feature costs from machine/run variability; no blanket no-regression claim is made.

Two concrete optimizations were made during this investigation. Token layout now
occupies **64 bytes rather than 72** on this Release ABI; the seven-repetition symbol
compile sample fell from 23.06 ms to 17.88 ms. Numeric conversion now bypasses the
generic argument array/dispatch while sharing the exact runtime checks; its paired
audit fell from 38.40 us to 34.06 us. Fixed-size payload copies compile to native
loads/stores. The non-debug execution body was 5,310 bytes in the inspected
byte-storage build versus 5,607 in the earlier switch sample.

Register payload/tag storage is **9 bytes per slot versus 16**, a **43.75% reduction**.
Integer-loop retained register storage fell from 48 to 27 bytes; call and field
workloads fell from 64 to 36. After forced collection, retained object counts and
bytes matched the baseline for **all 28 common VM cases**.

## Caches and REPL

| Boundary | Median |
| --- | ---: |
| In-memory validated bytecode load | 2.16 us |
| Bytecode serialization | 1.15 us |
| Small file compile / content-cache hit | 148.77 / 94.10 us |
| Linked standard-library compile / content-cache hit | 552.34 / 112.75 us |
| REPL submission after 0 / 32 prior inputs | 19.10 / 95.34 us |
| REPL submission after 128 / 512 prior inputs | 316.51 / 1,040.16 us |

The linked-library cache hit was about **4.9x faster** than uncached file compilation.
Cache hits include file/content validation and isolate/program lifetime costs;
in-memory image loading is a different boundary. The scalar REPL workload retained
two functions at every history size, zero GC-object bytes after collection and
zero GC allocator blocks. Compilation is linear in history, not incrementally checked.

A separate actual Release process kept an integer and rooted string, accepted up to
2,048 assignments, verified the string, reset and quit. Windows
`GetProcessMemoryInfo(PROCESS_MEMORY_COUNTERS_EX)` sampled these process-wide values:

| Prior assignments | Private bytes (MiB) | Working set (MiB) |
| --- | ---: | ---: |
| 0 | 0.79 | 5.58 |
| 32 | 0.87 | 5.66 |
| 128 | 1.04 | 5.82 |
| 512 | 3.89 | 8.67 |
| 2,048 | 8.77 | 13.52 |
| After reset | 8.77 | 13.53 |

These include compiler transient allocations and CRT/OS caching. Reset releases
language state and the isolate, but does **not** promise that process memory returns
to its initial value. This is one sample, not a leak proof or memory ceiling.

## Dispatch and GC decisions

The [computed-goto experiment](dispatch_measurements.md) showed 12–18% gains on
several workloads but a 3.88x larger execution body. Two measured tail-call
alternatives had regressions. P2 retains the single switch production path; the
reproduction patch was checked with `git apply --check`.

The [GC experiment](gc_strategy.md) retains non-moving incremental collection and
the automatic budget of 16. Final instrumented cycle/step samples were:

| Old objects / young survivors / budget | Cycle (us) | Mean per-cycle p95 step (us) |
| --- | ---: | ---: |
| 0 / 64 / 16 | 20.48 | 0.12 |
| 8,192 / 64 / 16 | 159.94 | 0.21 |
| 8,192 / 4,096 / 16 | 161.17 | 0.21 |
| 8,192 / 64 / 64 | 150.43 | 0.73 |

Budget 64 reduced the instrumented cycle time slightly but raised the step
percentile roughly 3.5x; it was not selected as the automatic budget. Timer and
sample overhead are included in cycles, and the clock resolution limits tiny-step
precision. Large objects/root rescans remain indivisible. A nursery remains a
future opportunity for stable large graphs with short-lived allocations; no
unimplemented generational collector is claimed as a measured competitor.

Raw local reports: `p2-before.json`, `p2-final.json`, `p2-final-vm.json`,
`p2-final-comparison.json`, `p2-regression-audit*.json`, `p2-token-layout.json`,
`p2-numeric-builtin-optimized.json`, and `p2-repl-memory.json` under `.cache/`.
