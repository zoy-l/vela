# Vela benchmarks

Google Benchmark runs the four Vela workloads through the production compiler
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
  of the Isolate and compiled program. Source file I/O is excluded.
- `first_execute/*`: runs bytecode once in a fresh Isolate on every iteration.
  Google Benchmark pauses timing for compilation, setup, verification, and
  destruction; timer pause/resume has overhead, so use this case for the first
  invocation cost rather than steady VM throughput.
- `vm/*`: compiles once, verifies one warm-up execution, then measures repeated
  execution in the same Isolate and VM. Allocation and incremental GC remain
  inside the measurement. Execution status and integer result are checked on
  every iteration, and `DoNotOptimize` retains the result.

| Workload | Expected result |
| --- | ---: |
| `integer_loop` | 499500 |
| `function_calls` | 1000 |
| `field_access` | 1000 |
| `allocation_gc` | 1000 |
| `symbol_tables/32`, `symbol_tables/256`, `symbol_tables/1024` | 11 |

`compile/symbol_tables/*` generates enums, classes, methods, functions, and long
identifier names before timing. It validates a compiled execution before the
measurement loop, then times complete compilation and resource destruction.
Each case reports source size in bytes. Calls target the first free function so
the workload size does not exceed the current bytecode's 8-bit call operand.

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
