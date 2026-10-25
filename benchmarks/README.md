# Vela benchmarks

Google Benchmark runs twenty-eight Vela workloads through the production compiler
and VM. Versions are pinned in `DEPS`; `//benchmarks:vela_benchmarks` links the
upstream `benchmark_main` and is separate from the language executable.
Native benchmark sources use `*_benchmark.cc`. Generated symbol-table workloads
also measure compilation at 32, 256, and 1024 declaration groups.

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

Each workload registers three cases:

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
See [execution measurements](../docs/vm_execution.md) for the current results.
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
| `numeric_conversions`, `expected_results`, `set_membership`, `map_lookup` | 499500 |
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
See [string GC measurements](../docs/string_gc_benchmark.md) for results.
The added string workloads compare six-part `+`, interpolation, and `join`,
compare builders against repeated `+` at 512 and 4096 appends, and exercise
Unicode APIs. See [string performance](../docs/string_performance.md) for
current results and measurement boundaries.

P1 adds explicit conversions, escaping closures, Expected allocation, interface
dispatch, match, Unicode graphemes, and ordered Set/Map workloads. Final P1
timings, memory counters, and changes against the preserved baseline are in
[P1 performance](../docs/p1_performance.md).

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

The source directory is supplied by GN, so the benchmark executable can run
from any working directory while the source checkout remains available. To add
a workload, place its `.vela` file here and register its three cases with an
expected result in `vm_benchmark.cc`.
