# dasProfile: Vela AOT and C++

This directory contains all 16 workloads ported from
[dasProfile](https://github.com/borisbat/dasProfile/tree/436d58d2840ff839550d44d32f98e10a15b57bf2),
revision `436d58d2840ff839550d44d32f98e10a15b57bf2`. It runs Vela AOT and C++ with
matching inputs and checked results. The sources, adapters and build rules are
self-contained in this repository.

```sh
python3 -m tools.vl bench --aot-compare
```

The command builds Release, compiles [workloads.vela](workloads.vela) through
the production `vela build --emit=obj` path, and links the object with
[the C++ workloads](workloads.cc) into the existing `vela_aot_benchmarks`
executable. Its 32 AOT/C++ cases join the existing benchmark registry as
`aot/native/dasprofile.*` and `aot/cpp/dasprofile.*`. The complete registry has
24 workloads and 64 cases, including the original VM and first-call cases. It
uses seven repetitions with random interleaving, and writes
`out/benchmarks/aot_comparison.json` plus its Markdown report. Each
invocation checks its result. Runtime errors, incorrect checksums, missing
cases and incomplete repetitions fail the comparison command.

To reproduce the five-repetition measurement settings:

```sh
python3 -m tools.vl bench --aot-compare -- \
  '--benchmark_filter=^aot/(native|cpp)/' \
  --benchmark_min_time=0.5s --benchmark_repetitions=5
```

Google Benchmark filters can select a subset:

```sh
python3 -m tools.vl bench --aot-compare -- \
  '--benchmark_filter=^aot/(native|cpp)/dasprofile\.(interop|sort)/'
```

Compilation, input preparation, native instantiation and one warm-up call are
outside steady timing. Loop work, result validation, ordinary allocations and
automatic Vela GC remain timed. The AOT lane uses the runtime native invocation
entry; it does not measure the public generated C++ facade. The `interop` case
calls the same function in [host_kernel.cc](host_kernel.cc) through a Vela binding
and directly from C++. That function is in a separate translation unit and has
inlining disabled.

[cases.json](cases.json) owns the exported names and reference results. Enum
values in [workloads.h](workloads.h) identify the corresponding input preparation
branch in Vela `prepare`; the complete comparison validates that mapping.

| Workload | Input per invocation | Reference result |
| --- | --- | ---: |
| sha256 | 32 hashes of 1,024 `.` bytes | 2048 after digest verification |
| dictionary | 500,000 deterministic string keys | 13 |
| nbodies | 5 bodies, 33,000 steps, dt 0.01 | -0.16907715681664656 |
| spectralNorm | Dimension 500, 2 power iterations | 1.274224115941234 |
| mandelbrot | 64 × 64 grid, at most 255 iterations | 107699 |
| expLoop | 1,000,000 exponential terms | 1000015.4709149387 |
| string2float | 10,000 decimal strings, 4 rounds | 199999998 |
| particles | 50,000 particles, 10 steps | 150060 on the first call; +60 per later call |
| queen | 8 × 8 board, recursive search | 92 |
| interop | 1,000,000 calls to add one | 1000000 |
| primes | Trial division through 14,000 inclusive | 1652 |
| tree | Treap operations for indices 1 through 19,999 | 4998 |
| sort | 100,000 deterministic keys, stable descending sort | -719320850322 |
| fibonacciLoop | 6,511,134 iterations, explicit uint32 wrap | 1781508648 |
| float2string | 10,000 doubles, 4 rounds | 351112 output characters |
| fibonacciRecursive | Recursive Fibonacci of 31 | 1346269 |

Both lanes use binary64 for floating-point work. Formatting uses shortest
double text, parsing uses decimal strings, and the tree uses the same
deterministic priority generator. Sorting uses int64 elements containing signed
int32 noise values and preserves stability on both sides. SHA retains the
upstream block-compression workload without standard SHA-256 padding; its
expected hexadecimal digest is checked in both implementations. SHA arithmetic
and iterative Fibonacci explicitly wrap at 32 bits.

These choices adapt upstream float32, formatting, random-priority and sorting
contracts to Vela. Vela retains checked int64 arithmetic, managed containers and
GC object lifetimes. C++ uses ordinary values and standard library containers;
the reported ratio includes those representation and ownership differences.
The C++ port uses the project Release flags and disables floating-point
contraction in its source. Refactored C++ storage and harness validation differ from the
earlier temporary experiment, so its historical ratios are not a direct
performance regression baseline for this executable.
