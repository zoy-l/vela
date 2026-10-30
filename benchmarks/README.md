# Vela benchmarks

Google Benchmark runs Vela workloads through the production compiler
and VM. Versions are pinned in `DEPS`; `//benchmarks:vela_benchmarks` links the
upstream `benchmark_main` and is separate from the language executable.
The [16 ported dasProfile workloads](dasprofile/README.md) are checked into
`benchmarks/dasprofile/` with Vela and C++ implementations. AOT and VM execute
the same Vela source. Run
`python3 -m tools.vl bench --aot-compare` to include them in the existing AOT
comparison executable and report. The port requires no daScript installation.
`python -m tools.vl bench --aot-compare` builds offline native objects through
`vela build` and runs `vela_aot_benchmarks` against the same VM sources and C++
workloads. It covers arithmetic, calls, fields, reserved arrays, host bindings,
allocation/collection, async continuations, and spectral norm. The report separates steady and
first native execution and records heap, frame, shadow-slot, saved-state and
process resident-memory costs. Resident snapshots share the benchmark process;
run backend filters in separate processes when comparing their RSS. Generated
`out/release/gen/benchmarks/aot_native/build_metrics.json` records the production
CLI compilation/publication time, object size, target and build identities.
The async C++ baseline uses the same scheduler and two queued observation
boundaries per element; its locals use ordinary C++ ownership.
The [rolling aggregate evidence](results/rolling_summary.json) records each
saved suite's cases, repetitions, source identities, host and measurement
settings. Read those boundaries before comparing its latest and previous runs.
Native frame bytes describe the declared LLVM frame
aggregate before optimization; root-store counts describe planned safepoint
writes. They do not measure the final machine stack or executed store count.
Compilation includes production object/header/manifest publication. Separate
link/archive probes use a checked arithmetic library and a precompiled C host;
their timings exclude compilation and package creation. Variation, exact
commands, identities, package sizes and system dependencies remain in the
corresponding summary entry. C++ can collapse arithmetic and field loops and
uses different ownership and arithmetic checks. These workload timings do not
isolate the cost of a cross-language call.
`vm/standard_api` covers default global APIs, sorting and reduction without imports.
`vm/array_algorithms` covers sorting, repeated reduction and mapping; both
use the normal compiler and VM and report the existing heap/register counters.
`vm/set_operations` builds two overlapping sets of 256 integers, then computes
their union, intersection and difference through the native ordered hash tables.
Native benchmark sources use `*_benchmark.cc`. Generated symbol-table workloads
also measure compilation at 32, 256, and 1024 declaration groups.

The representative suite contains nine algorithm/application workloads at two
sizes, each implemented in Vela, optimized C++, LuaJIT with JIT enabled, and
LuaJIT with JIT disabled. Run `python -m tools.vl bench --compare` to save raw
samples and a Markdown comparison under `out/benchmarks/`. The report separates
steady execution, first invocation, compilation and memory snapshots. It shows
Vela/C++, Vela/LuaJIT and Vela/Lua-interpreter ratios, medians and variation.
`array_growth` and `array_growth_reserved` compare typed push/read/pop workloads
with geometric growth versus a single reserve; see [typed arrays](../docs/en/arrays.md).

```powershell
python -m tools.vl bench
python -m tools.vl bench -- --benchmark_list_tests
python -m tools.vl bench -- --benchmark_filter=^vm/function_calls$ --benchmark_repetitions=5
python -m tools.vl bench -- --benchmark_min_time=0.2s --benchmark_out=out/benchmark.json --benchmark_out_format=json
```

The runner builds Release by default. Google Benchmark controls iteration
calibration, timing, repetitions, aggregates, filtering, and JSON/CSV output.
For a fixed execution count, use `--benchmark_min_time=3000x`; repetitions are
separate samples controlled by `--benchmark_repetitions`.

`--compare` defaults to seven repetitions, at least 0.2 seconds per case, and
random interleaving. It queries the executable's filtered benchmark registry
before execution and requires every selected case and repetition in the report;
missing backends, missing cases, duplicate repetitions, runtime errors and
checksum mismatches fail the command. An explicit filter selects the requested
subset without inventing results for excluded backends. For example:

```sh
python3 -m tools.vl bench --compare -- --benchmark_filter='^compare/(cpp|vm|luajit|luajit_interp)/sieve/'
```

Run `gclient sync --no-history` after pulling the LuaJIT dependency addition.
LuaJIT's immutable revision, version, architecture, GC64 and numeric mode are
included in the JSON context and Markdown report. The pinned library is embedded
in `vela_benchmarks`; the Vela runtime and CLI do not link LuaJIT.

The Lua kernels use ordinary tables and the same generated input values,
algorithm, loop counts and checksums as their Vela/C++ counterparts. Lua arrays
use one-based positions; stored input values still start at zero. The reserved
growth case uses LuaJIT's built-in `table.new`; it shares the growth kernel and
passes the capacity explicitly. No FFI arrays or native algorithm replacements
are used. Integer division is translated explicitly with `math.floor` for the
nonnegative operands in these kernels. All integer intermediates and checksums
fit within the exact integer range of binary64.

`compare/luajit_interp/*` disables the JIT engine and checks that no traces exist.
`compare/luajit/*` enables JIT and validates at least 128 warm-up calls, in batches
of 16, until trace count and live code bytes remain stable for two batches.
Warm-up fails after 1,024 calls if stability is not reached, or if no compiled
trace exists. The trace limit is explicitly 1,000. The inspector itself has JIT
disabled. Preparation, warm-up and the following full collection are outside
steady timing; normal GC and any later tracing remain timed. Warm/final trace
counts and code sizes expose subsequent compilation instead of assuming it cannot
happen. Every invocation checks the common expected result.

`first/vm/*`, `first/luajit/*` and `first/luajit_interp/*` run the workload once in
a fresh state without workload warm-up. Source compilation, library/input setup
and destruction are excluded; JIT compilation triggered by that first invocation
is included. Timer pause/resume overhead makes these startup measurements
different from steady throughput. `compile/representative/*` measures Vela's
complete compilation/bytecode preparation in a fresh Isolate;
`compile/luajit/*` measures source-to-Lua-bytecode loading in a fresh Lua state.
The report keeps those distinct compiler boundaries separate from execution.

Lua heap counters come from the default allocator's GC accounting, not a custom
allocator. Baseline, end-of-execution and post-collection heap sizes include
rooted input, libraries and program objects. Trace inspection happens after the
end-of-execution heap snapshot. JIT code bytes sum the machine code returned by
`jit.util.tracemc`; they exclude unused reserved code pages. Vela heap/register
counters retain their existing definitions. None of these is process RSS, and
Lua heap plus trace bytes must not be presented as complete process memory.

## Reloadable JIT

`python -m tools.vl bench --jit-compare` uses the AOT comparison executable to
run the eight maintained native workloads and 16 ported dasProfile workloads against C++,
VM, closed AOT, reloadable baseline JIT and optimized JIT. It also runs the `hot/`
lifecycle cases. The runner checks every selected registry entry and repetition,
checks paired execution results and rejects unretired temporary state. Output
defaults to `out/benchmarks/jit_comparison.json` and a Markdown report; `--report`
selects another JSON path. Defaults are seven repetitions, 0.2 seconds per case
and random interleaving.

Both JIT tiers use LLVM O2, checked operations, precise roots, current named
dispatch, external instance fields and safepoints. The optimized tier also emits
version-dependent direct calls/inlining and original-version baseline recovery.
Closed AOT keeps its normal fixed layouts and direct-call policy. Results are
reported per workload and as geometric mean JIT/AOT ratios, with a separate
aggregate excluding arithmetic and fields; they do not promise
that the optimized tier is faster. C++ arithmetic that simplifies as a whole is
shown separately and does not establish a general JIT performance advantage.

`hot/steady/` excludes compilation and one verified warm-up invocation. Its
counters separately record file capture, engine creation, source loading,
semantic checking (including inference and specialization), Typed IR, LLVM lowering/optimization, machine code,
linking, instantiation, initial publication and first execution. Each JIT case
compiles the entire corresponding source suite. dasProfile prepares its inputs
after loading and before the first measured workload call; that preparation is
excluded from both first-workload and steady timing. Process startup and OS cache
misses are outside these fresh-session measurements.

`hot/baseline/` and `hot/optimized/` measure complete lifecycle operations with
manual wall timing. `edits/8` and `edits/32` repeatedly change a function;
`closures/128`, `instances/1` and `instances/4096`, and `continuations/1` and
`continuations/1024` keep old state alive across eight edits. Instance fields
expand on the first edit, so the report includes maximum pause as well as
per-edit averages. Continuations await a shared host-created gate and must finish
their original body while calling the current callee. `active` commits at an
active frame's entry safepoint and verifies optimized deoptimization; `rebuild`
creates a fresh Isolate and verifies that its entry reruns. Stage counters
separate compilation, safepoint waiting, validation/GC, state preparation,
deoptimization, commit, stop and entry. No framework callback is installed;
the refresh counter is consequently zero.

Lifecycle memory snapshots collect before sampling and show deliberately
retained old state separately from state after release. The cases verify that
the last old reference releases its code batch and leaves one current batch.
Heap, external fields, saved continuations and linked executable/read-only/
writable block contents are distinct counters. Logical constant/debug payloads
describe part of the linked image, including source text and locations; they
overlap block totals and do not include allocator padding, deduplication or OS
debug registration. Compilation peak is the process lifetime resident high-water
mark sampled after compilation. Run a single workload in a fresh process to
attribute that peak; a combined run can retain the high-water mark of an earlier
case. Resident snapshots include LLVM and incremental compilation state.

Use a registry filter for a focused measurement:

```sh
python -m tools.vl bench --jit-compare -- --benchmark_filter='^(aot/(native|vm|cpp)/|hot/steady/)'
python -m tools.vl bench --jit-compare -- --benchmark_filter='^hot/(baseline|optimized)/'
```

Aggregate results follow the repository's two-entry
[`rolling_summary.json`](results/rolling_summary.json) retention contract.

## Native C++ call boundaries

Run `python3 -m tools.vl bench --native-compare` to compare direct C++ calls,
opaque C++ function-pointer calls, VM native calls and VM loop controls. It uses
the same Release/repetition policy as `--compare` and writes
`out/benchmarks/native_comparison.json` plus a Markdown report. Use `--report`
to choose a different JSON path; Google Benchmark options still follow `--`.

The four kernels cover a void call, integer input/result, double input/result,
and a borrowed `std::string_view` whose length and first byte are read without
copying or returning a new string. Each batch performs 64 or 4,096 calls and
checks its checksum. Kernels live in a separate translation unit with inlining
disabled. The empty kernel contains a compiler memory barrier to retain the
call; it performs no application work. C++ baselines use the same kernels and
`NativeContext`. The indirect target is hidden from the compiler before timing.

VM cases use the production compiler, external declaration, native binding and
execution path. Registration, compilation and one warm-up batch are excluded.
Normal execution, loop work, checksum accumulation, host entry/exit and native
binding adapters remain timed. Controls replace the native operation with the matching
script arithmetic, or the known kernel result for the string case. Reported
nanoseconds divide the complete batch time by its call count. `VM minus control`
subtracts independent medians; it includes changes in bytecode and register
work and must not be described as an isolated ABI cost. A negative estimate
remains visible instead of being clamped to zero. The controls perform no
native calls; their normalized times are per loop iteration.

The memory table records live GC heap bytes/objects before and after timing,
plus register capacity after warm-up. Register growth after warm-up fails the
case. These snapshots do not count cumulative allocations or host allocator
calls. Measure those separately with allocator instrumentation; equal snapshots
do not prove zero allocations. Direct native calls borrow the caller's rooted
register window without a callee register file or intermediate argument buffer;
dynamic argument binding has its own window. Native diagnostic frames and payload
checks remain part of execution. See [native embedding](../docs/en/native_embedding.md)
for the invocation, rooting and reentry contracts.

The original small workloads use these timing boundaries:

- `compile/*`: compiles already loaded source, including creation and destruction
  of the Isolate and compiled program, IR optimization, physical register
  allocation, and one-time bytecode validation. Source file I/O is excluded.
- `first_execute/*`: runs bytecode once in a fresh Isolate on every iteration.
  Google Benchmark pauses timing for compilation, setup, verification, and
  destruction; timer pause/resume has overhead, so use this case for the first
  invocation cost rather than steady VM throughput.
- `vm/*`: compiles once, verifies one warm-up execution, then measures repeated
  execution in the same Isolate and VM. Allocation and incremental GC remain
  inside the measurement. Execution status and integer result are checked on
  every iteration, and `DoNotOptimize` retains the result.

Steady cases report `register_bytes`: native register capacity retained after
warm-up, excluding fixed VM metadata and the GC heap. Register storage is reused
by call depth; bytecode validation is included in compilation, not execution.
See [execution measurements](../docs/en/vm_execution.md) for the current results.
The array and double timing tables below predate that execution refactor.

| Workload | Expected result |
| --- | ---: |
| `integer_loop` | 499500 |
| `switch_dispatch`, `if_dispatch` | 3500 |
| `nullable_calls` | 1000 |
| `function_calls` | 1000 |
| `field_access` | 1000 |
| `allocation_gc` | 1000 |
| `array_traversal` | 120000 |
| `array_allocation` | 499500 |
| `double_loop` | 1000 |
| `double_array` | 16000 |
| `string_concat` | 1000 |
| `string_growth` | 512 |
| `string_parts_concat`, `string_interpolation`, `string_join` | 1000 |
| `string_builder` | 512 |
| `string_growth_large`, `string_builder_large` | 4096 |
| `string_unicode` | 1000 |
| `numeric_conversions`, `set_membership`, `map_lookup` | 499500 |
| `closure_calls`, `interface_calls` | 1000 |
| `match_dispatch` | 3500 |
| `unicode_graphemes` | 4000 |
| `symbol_tables/32`, `symbol_tables/256`, `symbol_tables/1024` | 11 |

`compile/symbol_tables/*` generates enums, classes, methods, functions, and long
identifier names before timing. It validates a compiled execution before the
measurement loop, then times complete compilation and resource destruction.
Each case reports source size in bytes. Calls target the first free function so
the workload size does not exceed the current bytecode's 8-bit call operand.

`array_traversal` reads a 16-element integer array over 1,000 rounds (16,000
indexed reads). `array_allocation` creates 1,000 such arrays, overwrites element
zero, and sums that element. Both use the normal checked access and GC paths;
the latter includes array backing storage in GC allocation pressure.

`switch_dispatch` and `if_dispatch` perform the same eight-way selection 1,000
times. `nullable_calls` adds a nullable return and explicit guard to the same
1,000 calls as `function_calls`. These cases measure production control flow;
neither nullable integers nor their guards allocate GC objects.

`double_loop` performs 1,000 double additions and verifies the result is `500.0`.
`double_array` performs 16,000 reads and additions from native double array
storage, verifies a sum of `120000.0`, and returns the number of indexed reads.

`string_concat` creates and checks 1,000 short concatenations through function
calls. `string_growth` appends 512 bytes one at a time and verifies the result
against a string built by doubling. Both include automatic string GC. Steady
cases report `retained_objects` and `retained_bytes` after a final collection
outside timing, in addition to the heap counters sampled before collection.
See [string contracts](../docs/en/strings.md) and [garbage collection](../docs/en/garbage_collection.md).
The added string workloads compare six-part `+`, interpolation, and `join`,
compare builders against repeated `+` at 512 and 4096 appends, and exercise
Unicode APIs.

`string/length_scan_*` and `string/length_cached_*` compare counting UTF-8
bytes on every read with the stored code point count. `string/index_from_start_*`
uses the former indexing algorithm; `string/index_cursor_forward_*` and
`string/index_cursor_reverse_*` use the current string position cache. Arguments
256 and 1024 are four-code-point block counts (1024 and 4096 code points).
ASCII blocks are `abcd`; mixed blocks are `A中😀e`. Every case checks its count
or offset sum. These runtime measurements exclude compilation, VM dispatch and
allocation of the one-code-point strings returned by language indexing.

`string_format` formats int/double/bool values directly in interpolation;
`string_format_explicit` produces the same result with three explicit
`toString()` calls per iteration. `number_join` joins six integers. Each checks
1,000 results.

Core language features include explicit conversions, escaping closures, interface
dispatch, match, Unicode graphemes, and ordered Set/Map workloads; see the
[core language specification](../docs/en/core_language.md).

Array results measured on 2026-10-24 with the pinned Windows Clang Release
toolchain, `--benchmark_min_time=0.2s --benchmark_repetitions=5`, are below.
Values are median wall times per complete workload, in microseconds; VM times
include loop dispatch, checked element access, allocation, and collection.

| Measurement | `array_traversal` | `array_allocation` |
| --- | ---: | ---: |
| Compile | 10.684 | 9.513 |
| First execution | 405.753 | 202.045 |
| Steady execution | 412.916 | 197.193 |

The JSON report is `.cache/typed-arrays-benchmark.json`. All 21 benchmark cases
completed without `error_occurred`; every execution case checked its result.

Double results from the same Release toolchain and repetition settings on
2026-10-24, in median wall-clock microseconds per workload:

| Measurement | `double_loop` | `double_array` |
| --- | ---: | ---: |
| Compile | 10.636 | 21.114 |
| First execution | 32.624 | 422.543 |
| Steady execution | 24.155 | 420.799 |

The report is `.cache/double-benchmark.json`; all 27 cases completed without
errors. The double loop reports zero heap allocation. The array measurement
includes normal checked indexing, loop dispatch, allocation, and collection.

To compare compiler container changes with repetitions and a JSON report:

```powershell
python -m tools.vl bench -- --benchmark_filter=^compile/ --benchmark_min_time=0.2s --benchmark_repetitions=5 --benchmark_out=out/compile-benchmark.json --benchmark_out_format=json
```

Missing sources, compile errors, missing entry points, runtime errors, and
incorrect results are reported with `SkipWithError`. Steady VM cases also report
final heap object counts, allocated bytes, committed bytes, and the result.
The console `Time` column is wall time; `CPU` is CPU time, both in nanoseconds by
default. Compare equivalent Release configurations and measurement boundaries.

The `async/` suite compiles and executes real Vela programs. It measures a call
without await, a completed await, a pending microtask and its continuation,
1,000 calls without await, and the first call in a fresh Isolate. Concurrent
tasks retain an array across a shared pending Future; the `suspended_gc` cases
collect before resuming. Concurrent tasks and zero-delay timers run at 1, 100,
and 1,000 instances. Each case checks its result; steady cases also check that
the final collection leaves no suspended frame or additional retained heap value.

`saved_state_bytes` includes frame metadata and the capacities of saved values
and register indices; `saved_bytes_per_frame` divides it by the number of
suspended frames. `first_async_bytes` measures GC-accounted allocation before
the first pump. These counters exclude allocator bookkeeping and native Future
control blocks. `async/first_call` excludes compilation and destruction. Compare
`vm/integer_loop`, `vm/function_calls`, and `vm/field_access` separately for
synchronous regression. Aggregate evidence is retained in
[`results/rolling_summary.json`](results/rolling_summary.json).

The source directory is supplied by GN, so the benchmark executable can run
from any working directory while the source checkout remains available. To add
a small workload, place its `.vela` file here and register its applicable cases
with an expected result in `vm_benchmark.cc`. Paired workloads belong in
`workloads/`, with matching kernels and size/checksum records.

`standard_value_benchmark.cc` measures the production compiler/VM paths for
`case/*`, `parse/*`, `date/*`, and `clock/*`. Casing covers unchanged ASCII,
changing ASCII, simple Unicode, expanding mappings, and contextual runs at
8, 256, and 4096 input units. Parsing covers decimal/hexadecimal integers,
floating point, and an invalid tryParse. Calendar cases separate strict parsing,
canonical formatting, UTC construction, and decomposition. Clock cases isolate
the existing wall and monotonic APIs.

Compilation and setup are excluded from steady timing. Every iteration checks
its integer result; cases that return strings check output byte count, with
content correctness covered by the regression fixtures. `first_call_gc_bytes`
reports the first execution's GC-accounted allocation, `input_bytes` and
`output_bytes` describe casing size, and `register_bytes` reports retained VM
register capacity. Native allocator bookkeeping and temporary backend allocations
are excluded. A final collection verifies zero additional retained heap bytes.
The rolling summary retains aggregate timings, variation, these counters, and
the before/after synchronous and clock regression comparison.

`byte_data_benchmark.cc` uses the same checked steady VM harness for
`byte/allocate_zero`, `byte/copy`, `byte/index`, typed big/little/host access
at aligned and unaligned offsets, and GC pressure. Those cases include local
owner allocation and zeroing. Copy cases include allocating the source and its
independent slice; `copied_bytes` counts the slice payload. Typed cases perform
1,000 write/read pairs and check their sum. Compilation and setup stay outside
the timer, and first-call GC bytes include the owning and view objects.

`byte/runtime_view` measures runtime view allocation and GC alone, with a rooted
owner allocated and zeroed before timing. Its three owner sizes check that view
creation cost does not scale with backing length. All cases check results and
zero additional retained heap bytes after collection. GC byte counters exclude
allocator bookkeeping. The retained regression suite compares existing VM
integer loops, calls, fields, array traversal and array allocation with identical
Release settings. See [the rolling aggregate summary](results/rolling_summary.json)
for exact settings, variation, counters and comparison limits.

The `testing/` suite measures source-test startup including compilation, cached
loads of 1 and 16 suites, registration of 1,000 tests, 1,000 assertions, a
2,048-level deep match, a 3-by-3 overlapping matcher relation, and 1 or 4
concurrent suites. Execution cases exclude compilation. Every iteration checks
the final test statuses. The overlap case evaluates all nine candidate pairs
before finding a one-to-one assignment.

Heap counters sample GC-accounted allocated and committed bytes after execution;
`retained_bytes` follows collection. Concurrent counters sum independent Isolates
while all workers remain alive. `process_peak_bytes` is the process resident
high-water mark, including compilation and earlier cases; run a case in a fresh
process to attribute that counter. These are distinct from instantaneous matcher
stack usage. Measure the unchanged `async/` cases in the same Release build to
check the ordinary asynchronous path.

The `mock/` suite measures the production interface generator and `--check`,
generated-source compilation and content-cache hits, configured calls with 1,
16 and 128 matching rules, count and order verification, argument capture,
Future responses, call observation, and collection after 10,000 recorded calls.
It generates its interface adapters in an owned temporary directory from
[`mock_api.vela`](mock_api.vela) and executes
[`mock_workload.vela`](mock_workload.vela). The synchronous and Future baselines
use a handwritten implementation of the same interface.

Execution timings exclude compilation and include one selected test's setup,
body, verification and session cleanup. Most cases perform 1,024 calls; waiting
uses 128 and the resource case uses 10,000. `items_per_second` reports that batch
size, so its inverse is an amortized workload cost rather than an isolated VM
instruction cost. `custom_matcher_evaluations` counts user matcher calls in the
rule, verification, capture and observation cases; it excludes unconditional
built-in `any` matches. The script checks these counts against independent
expected values. Every case verifies its results; execution cases
also require post-collection `retained_bytes` to equal the warmed
`baseline_bytes` and require zero remaining suspended frames. Allocated and
committed heap counters are not process RSS.

```powershell
python -m tools.vl bench -- --benchmark_filter=^mock/ --benchmark_min_time=0.2s --benchmark_repetitions=7 --benchmark_enable_random_interleaving=true
```

Use the same Release build to measure `async/no_await`, `async/completed_await`
and `async/suspend_resume` when checking ordinary Future execution without mock
imports. Save complete aggregate evidence according to the
[retention rules](../.agents/benchmark_results.md).
