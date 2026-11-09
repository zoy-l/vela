# Vela benchmarks

Google Benchmark runs production JIT, AOT and shared-runtime workloads.
`//benchmarks:vela_benchmarks` contains source execution, runtime, tooling and
C++/LuaJIT comparisons. `//benchmarks:vela_aot_benchmarks` contains offline AOT,
reloadable JIT, generic, Map-literal and escape-analysis workloads.
Dependencies and versions are pinned in `DEPS`.

```powershell
python -m tools.vl build release benchmarks
python -m tools.vl bench -- --benchmark_list_tests
python -m tools.vl bench -- --benchmark_filter=^jit/function_calls$ --benchmark_repetitions=5
python -m tools.vl bench --compare
python -m tools.vl bench --native-compare
python -m tools.vl bench --aot-compare
python -m tools.vl bench --jit-compare
python -m tools.vl bench --escape-compare
python -m tools.vl bench --gc-report
```

The runner builds Release by default. Arguments after `--` are passed to Google
Benchmark. Report modes default to seven repetitions, a 0.2-second minimum
measurement and random interleaving. `--report=PATH.json` selects their JSON
destination; the matching Markdown report separates time and memory.

## Source execution and runtime workloads

[execution_benchmark.cc](execution_benchmark.cc) supplies `compile/<workload>`,
`first_execute/*` and `jit/*` cases for integer/double loops, calls, closures,
fields, arrays, collections, exceptions, strings and the standard library.

- Compilation includes source preparation, a fresh JIT engine, LLVM optimization
  and linking, but excludes source-file I/O.
- First execution uses a fresh native instance. Compilation, instantiation and
  destruction are excluded from the measured invocation.
- Steady execution compiles once, checks a warm-up, and invokes the same instance.
  Normal allocation and incremental GC remain timed.

Each execution checks its result. Heap counters distinguish live allocation,
committed storage, post-collection retention and object counts.
`register_bytes` is not a native-runtime metric.

[native_analysis_benchmark.cc](native_analysis_benchmark.cc) supplies the shared
compiler microbenchmarks `compile/native/repeated_leaf/*`,
`compile/native/mixed_component`, `compile/native/nonrecursive` and
`compile/native/recursive_over_budget`. They measure call planning or recursive
inlining analysis over prepared IR and check the resulting plan or unchanged
function. Repeated-leaf arguments select calls per caller and the number of
callers sharing one callee. Source preparation, code generation and linking are excluded. Run them
with `python -m tools.vl bench -- --benchmark_filter=^compile/native/`.
These standalone compiler measurements have no language comparison report.

[standard_value_benchmark.cc](standard_value_benchmark.cc) supplies the shared
native harness for string case conversion, numeric parsing, date/time and other
standard APIs. Byte data, JSON/dynamic calls and regular expressions add workloads
in their corresponding benchmark sources. Persistent globals may be established
during warm-up; temporary retained bytes are checked against that baseline.
Direct runtime kernels exclude source compilation and generated-call overhead.

[async_benchmark.cc](async_benchmark.cc) measures no-await calls, completed awaits,
suspend/resume, concurrent tasks and timers. It reports suspended frames and saved
state separately from the GC heap and checks that completed tasks release them.
[gc_strategy_benchmark.cc](gc_strategy_benchmark.cc) measures collection phases and
step distributions, reported separately through `--gc-report` below.

## Collector performance

`python -m tools.vl bench --gc-report` writes `out/benchmarks/gc_report.json` and
`gc_report.md`. It selects `gc/cohort/*` runtime workloads without a C++ reference
or AOT/JIT ratios. Each cycle allocates 4,096 new strings, retains either 64 or all
4,096, and optionally preserves an 8,192-object graph. Step budgets are 16 or 64.
The harness checks the surviving object count after every cycle.

Automatic collection is disabled during setup. Manual cycle time begins at
`StartCollection` and ends after the last `Step`, excluding graph construction,
percentile calculation and Isolate destruction. Per-step clock reads and sample
recording remain in the cycle measurement. The report shows median cycle time
and CV, collection-start time, steps per cycle, mean per-cycle p95 step time and
the maximum observed step across all repetitions. The maximum includes operating
system scheduling noise; incremental budgets do not imply hard real-time bounds.

Memory columns show the final cycle's pre-collection heap, live and committed
bytes, reclaimed bytes and reclaimed object count, summarized by median across
repetitions. They are not process RSS or peak heap measurements. The step p95 is
calculated per cycle, averaged within each repetition, then reported as the median
of those averages; it is not a pooled percentile of every step in the run.

## Representative C++ and LuaJIT comparisons

`--compare` runs representative workloads at two sizes with closed Vela JIT,
ordinary optimized C++, LuaJIT and the LuaJIT interpreter. The labels are
`compare/jit/*`, `compare/cpp/*`, `compare/luajit/*` and
`compare/luajit_interp/*`. The report checks LuaJIT trace evidence and rejects
compiled traces reported by the interpreter mode.

Steady time excludes source loading, compilation, input preparation and warm-up.
Normal GC and subsequent LuaJIT tracing remain timed. `first/jit/*` and the Lua
first-call cases use fresh state and include lazy work during that invocation.
`compile/representative/*` measures the native source pipeline with a reusable
JIT engine; `compile/luajit/*` loads source into Lua bytecode. These are different
compiler boundaries.

```powershell
python -m tools.vl bench --compare -- --benchmark_filter='^compare/(cpp|jit|luajit|luajit_interp)/sieve/'
```

Workloads include algorithms, arrays, closures, dynamic dispatch, JSON, byte views,
regular expressions, numeric parsing, casing and UTC conversion.
C++ uses its ordinary containers and ownership; it may inline or vectorize code.
Differences in checked arithmetic, dynamic dispatch, object representation and
GC are part of each complete workload, not isolated ABI overhead.
See [representative_benchmark.cc](representative_benchmark.cc) for checksums and
the LuaJIT warm-up contract.

## Offline AOT and reloadable JIT

`--aot-compare` compares offline native execution with C++ and separates
`aot/first_native/*` from `aot/native/*` steady time.
The build emits source through the production `vela build` interface.
Generated `out/release/gen/benchmarks/*_native/build_metrics.json` records
compilation/publication time, object size, target and build identities.

The basic arithmetic workload sums squares from a runtime-prepared `int64`
array at 1,000, 4,096 and 65,536 elements. C++ and Vela prepare the same bounded
sequence from seed 42 before timing. Each invocation reads the array; the input
cannot be replaced by a closed-form sum over loop indices. `calls` reduces the
same input through an increment function. Both compilers may inline that function
normally, so this case measures optimized script calls, not forced call latency.
The host-call case retains the shared, non-inlined C++ host function on both paths.

`temporaryObjects` constructs a temporary `Box`, formats an integer and consumes
the text length. C++ uses a local value without an object-pointer optimization
barrier. Both languages may eliminate objects and strings. Reports include Vela
Heap allocation counts and bytes per complete timed invocation, including
Heap-accounted external blocks. These counters exclude warm-up and the final
forced collection; end-of-run heap snapshots describe live storage instead.
Automatic GC during execution remains timed. This is a language workload, not a
collector-speed comparison; use `--gc-report` for collector performance.

Synchronous cases have a dedicated runner with scalar result consumption and
checksum validation. Future completion and scheduler work are compiled only into
the async runner. Benchmark-only `Sample` records and string error results are
not part of steady timing. Runtime status/type checks and Vela's generated entry,
overflow and GC contracts remain timed. Each iteration has a compiler memory
barrier on both sides so read-only input work cannot be reused across iterations.
There is no repeated-operation loop added solely to divide away entry overhead,
and no subtraction of independently timed wrappers.

The `aot_workload_contract=runtime-inputs/specialized-completion/v3` context marks
this boundary. The suite has 24 workload names and 26 workload/size pairs;
aggregate ratios weight each selected size separately. Earlier arithmetic
results used a closed-form sum and an asymmetric result wrapper, and earlier
`calls` results forced C++ calls to remain out of line. Those timings are not a
performance baseline for the revised workload contract.
The earlier `gc` workload forced C++ heap allocation while Vela could eliminate
the temporary objects and strings. Its ratios are not a baseline for
`temporaryObjects` or evidence of collector performance.

The [16 algorithm workloads](algorithms/README.md) use the same Vela source for AOT
and JIT. Input preparation is excluded from steady timing, every invocation checks
its result, and stateful particles advance between calls. No daScript installation
is required.

`--jit-compare` pairs C++, closed AOT and reloadable baseline/optimized JIT.
Both JIT tiers use LLVM Release optimization, checked operations, precise roots,
native dispatch and normal GC. The optimized tier also retains recovery state.
The report requires all four backends for each selected steady workload.
Its default filter selects these steady workloads. Reload lifecycle and scaling
measurements use `--benchmark_filter='^hot/(baseline|optimized|scaling)/'`.
Use `--benchmark_filter='^(aot/(native|cpp)/|hot/)'` to include both groups.

Each selected source suite/tier compiles once before any calibration or sampling.
Registrations, calibration attempts and repetitions share the immutable linked
image. Every sample still creates a fresh Isolate, publishes the image, prepares
its own input, checks a warm-up and collects the heap baseline. Globals, bindings,
GC state and suspended tasks belong to that Isolate. Timed calls resolve the
current declaration through native dispatch and invoke the production native
target; development-session source lookup and reload coordination are excluded.
Normal allocation, GC, checksum checks and the default repetition/interleaving
settings remain enabled.

The report separates one-time suite preparation from fresh-Isolate publication
and first workload execution. Suite preparation values are single observations,
not repeated cold-start samples. `load_ns` measures Isolate creation and initial
publication from an already prepared image. The `jit_steady_setup` context marks
this measurement contract; older fresh-session results include a different host
entry boundary and must be identified separately when comparing small workloads.
Listing benchmarks does not compile suites, and filters prepare only matching
suite/tier pairs. Process memory includes every prepared image in the process.

The `entry/` component benchmarks in `vela_aot_benchmarks` measure native stack-bound
queries, execution construction/destruction, and instance borrow/release separately.
Those components use a closed image and exclude instance preparation. The
`entry/generated/scalar` and `entry/target/scalar` measure a scalar increment at
the generated AOT entry and the production target adapter. `entry/baseline_scalar`
and `entry/optimized_scalar` measure retained reloadable entries. They encode a
runtime scalar argument, check status/type and consume the result on every call,
while excluding the development-session lookup and activation lifetime.
`entry/cpp_direct/scalar` and `entry/cpp_indirect/scalar` call a non-inlined C++
scalar function with the same runtime value and checksum. C++ returns an integer
in a register; Vela uses its status/output ABI and runtime checks, so these
component times describe different entry contracts and are not algorithm ratios.
Run them after building
Release benchmarks with `out/release/vela_aot_benchmarks --benchmark_filter='^entry/'`.
These independent measurements help locate fixed costs; their times are not additive
and do not replace the complete reloadable invocation measured by `hot/steady/`.

```powershell
python -m tools.vl bench --jit-compare -- --benchmark_filter='^(aot/(native|cpp)/|hot/steady/)'
```

The `hot/` lifecycle cases measure edits, retained closures/instances,
continuations, rebuilds, active frames and virtual-object materialization.
They separate compilation stages, safepoint waiting, preparation, publication,
deoptimization, refresh and maximum runtime pause. Release checks require old code
batches and suspended frames to retire after deliberately retained values are
dropped. See [reload_benchmark.cc](reload_benchmark.cc).

`maps/` compares Map literals with explicit construction for empty, small, large,
duplicate-key, dynamic and nested-reference inputs. AOT and both JIT tiers run the
same cases; `maps/compile/` excludes reusable JIT-engine creation.
`generics/` separates frontend definitions/applications from baseline/optimized
execution and reports generated-code size. `--escape-compare` compares heap,
scalar-replacement and borrowed-storage modes, including virtual-frame reload.
All modes check results and the intended allocation/materialization evidence.

## Native host-call boundaries

`--native-compare` uses `vela_aot_benchmarks` to measure direct C++, opaque C++
function pointers, `native/jit/*` and `native/aot/*` host calls, and the corresponding
`native/jit_control/*` and `native/aot_control/*` script loop controls.
Cases cover empty, integer, double and borrowed-string signatures at two batch sizes.
JIT and AOT compile the same generated source and use the same complete runtime
entry. The host kernels are shared and compilation/registration/warm-up are excluded.
Every selected signature/batch pair requires all six backends in the report.

Per-call time divides complete batch time by its call count.
“Native minus control” subtracts independent medians, not paired samples; generated
loop and checksum differences remain part of that estimate. It is not a pure ABI
latency measurement. Heap snapshots are live counts, not cumulative allocation or
host malloc counts. See [native_call_benchmark.cc](native_call_benchmark.cc).

## Cache, tooling, tests and mocks

`cache/native_instantiate` measures fresh-Isolate instantiation from an existing
compiled image. Content-hit cases rerun the source pipeline with a reusable JIT
engine, verify an object-cache hit and skip LLVM optimization/object emission.
Uncached file cases use the same source pipeline without a cache.
See [native cache contracts](../docs/en/compilation_cache.md).

Symbol-table, liveness, generic specialization and completion cases measure their
named compiler stages. Symbol declaration count is independent of executed work.
`repl/submission` measures a native development submission after preparing the
requested history; it reports accepted source, function and heap state.

`testing/` measures native suite startup, registration, assertions, deep/overlap
matchers, cache batches and independent concurrent Isolates.
`mock/` separates generation, compilation, cache hits and selected SDK
interaction workloads. Both check test results and suspended-state cleanup.
Mock execution additionally checks matcher counts and retained heap state.
See [testing_benchmark.cc](testing_benchmark.cc) and [mock_benchmark.cc](mock_benchmark.cc).

## Metrics and retained evidence

Native frame bytes are the declared LLVM frame aggregate before optimization;
shadow-slot and root-store counters describe the compiler plan. They do not
measure the final machine stack or executed store count.
Linked code counters distinguish executable, read-only and writable contents.
Constant/debug payload counters overlap image storage and exclude allocator
padding. Process resident snapshots include the executable, LLVM and earlier work
in the same process; use separate backend processes when comparing footprints.
Compilation peak is the process lifetime high-water mark.

Checksums, missing cases, duplicate/incomplete repetitions, invalid counters and
retained temporary state are failures. `SkipWithError` is not a passing sample.
The console's Time column is wall time; CPU is CPU time.
A single-iteration smoke check verifies behavior, not performance.

Keep aggregate evidence only in [rolling_summary.json](results/rolling_summary.json)
under the [retention rules](../.agents/benchmark_results.md).
Promote only a complete intended suite, retaining exact source/worktree identity,
host, toolchain, command, repetitions, medians and variation.
Older backend measurements remain historical evidence and are not comparable to
native migration results without matching scope and measurement boundaries.
