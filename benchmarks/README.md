# Vela benchmarks

Google Benchmark runs Vela workloads through the production compiler
and VM. Versions are pinned in `DEPS`; `//benchmarks:vela_benchmarks` links the
upstream `benchmark_main` and is separate from the language executable.
`vm/standard_api` covers default global APIs, sorting and reduction without imports.
`vm/array_algorithms` covers sorting, repeated reduction and mapping; both
use the normal compiler and VM and report the existing heap/register counters.
`vm/set_operations` builds two overlapping sets of 256 integers, then computes
their union, intersection and difference through the native ordered hash tables.
Native benchmark sources use `*_benchmark.cc`. Generated symbol-table workloads
also measure compilation at 32, 256, and 1024 declaration groups.

The representative suite adds nine
algorithm/application workloads at two sizes, each with an optimized C++ pair.
Run `python -m tools.vl bench --compare` to save raw samples, medians, VM/C++
ratios, variation and Vela heap snapshots under `out/benchmarks/`.
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
See [string contracts](../docs/en/strings.md) and [garbage collection](../docs/en/garbage_collection.md).
The added string workloads compare six-part `+`, interpolation, and `join`,
compare builders against repeated `+` at 512 and 4096 appends, and exercise
Unicode APIs.

`string_format` formats int/double/bool values directly in interpolation;
`string_format_explicit` produces the same result with three explicit
`toString()` calls per iteration. `number_join` joins six integers. Each checks
1,000 results.

Core language features include explicit conversions, escaping closures, Expected allocation, interface
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
