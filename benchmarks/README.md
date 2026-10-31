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
```

The runner builds Release by default. Arguments after `--` are passed to Google
Benchmark. Comparison modes default to seven repetitions, a 0.2-second minimum
measurement and random interleaving. `--report=PATH.json` selects their JSON
destination; the matching Markdown report separates time and memory.

## Source execution and runtime workloads

[execution_benchmark.cc](execution_benchmark.cc) supplies `compile/*`,
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
step distributions. Incremental budgets do not imply hard real-time bounds.

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

The [16 dasProfile ports](dasprofile/README.md) use the same Vela source for AOT
and JIT. Input preparation is excluded from steady timing, every invocation checks
its result, and stateful particles advance between calls. No daScript installation
is required.

`--jit-compare` pairs C++, closed AOT and reloadable baseline/optimized JIT.
Both JIT tiers use LLVM Release optimization, checked operations, precise roots,
native dispatch and normal GC. The optimized tier also retains recovery state.
The report requires all four backends for each selected steady workload.

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

`--native-compare` measures direct C++, opaque C++ function pointers,
`native/jit/*` host calls and `native/jit_control/*` script loop controls.
Cases cover empty, integer, double and borrowed-string signatures at two batch sizes.
The host kernels are shared and compilation/registration/warm-up are excluded.

Per-call time divides complete batch time by its call count.
“JIT minus control” subtracts independent medians, not paired samples; generated
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
