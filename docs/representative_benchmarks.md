# Representative Vela / C++ benchmarks

This suite includes nine complete workloads at two sizes, with paired C++ kernels,
independent fixed checksums, production compilation, and production VM execution.
It complements the existing small instruction/API benchmarks. Sources are in
[`benchmarks/workloads/`](../benchmarks/workloads/), native kernels in
[`native_workloads.cc`](../benchmarks/native_workloads.cc), and the Google Benchmark
harness in [`representative_benchmark.cc`](../benchmarks/representative_benchmark.cc).

## Research and workload selection

Reviewed on 2026-10-25:

- [Luau's benchmark collection](https://github.com/luau-lang/luau/tree/master/bench)
  separates application/algorithm workloads, microbenchmarks and GC workloads.
  [qsort](https://github.com/luau-lang/luau/blob/master/bench/tests/qsort.lua)
  verifies the sorted output, and
  [BinaryTree](https://github.com/luau-lang/luau/blob/master/bench/gc/test_BinaryTree.lua)
  mixes short-lived trees with a retained tree.
- [Luau's harness](https://github.com/luau-lang/luau/blob/master/bench/bench_support.lua)
  repeats measurements and supports timed inner work separately from setup.
  Its default removal of the four slowest runs is deliberately not used here:
  this suite retains all samples and reports both medians and variability.
- [Are We Fast Yet?](https://github.com/smarr/are-we-fast-yet) compares objects,
  closures, arrays and strings across language implementations.
  Its [guidelines](https://github.com/smarr/are-we-fast-yet/blob/master/docs/guidelines.md)
  require deterministic, well-typed inputs, matching algorithm structure, fixed
  array semantics and checked results. These are useful comparison rules even
  when the baseline is C++.
- [Octane](https://github.com/chromium/octane) combines application-oriented
  JavaScript workloads, but is retired. It is a historical reference, not a
  current performance target.

The Vela sources are independently written workloads inspired by these workload
families, not literal ports. For example, Luau's named sieve uses coroutines;
Vela's sieve uses an integer flag array. There are no Luau, LuaJIT, V8 or Dart
measurements here, and these results cannot establish rankings against them.

| Workload | Sizes | Timed work and coverage |
| --- | --- | --- |
| `sieve` | 10,000 / 100,000 integers | Allocate int64 flags, mark composites, count primes; arithmetic, branches, checked indexing |
| `mandelbrot` | 64 / 128 pixels per side | Escape-time computation with at most 50 iterations per pixel; double arithmetic, nested loops |
| `quicksort` | 1,024 / 8,192 integers | Map deterministic input, in-place quicksort, verify ordering and weighted checksum; recursion, array reads/writes |
| `binary_trees` | depths 6 / 9 | Retain one full tree while constructing/traversing 16 temporary trees; nullable fields, recursion, allocation, GC |
| `particles` | 128 / 1,024 objects | Allocate particles, update 64 frames with boundary reflection, checksum final positions; method calls, double fields, object arrays |
| `word_count` | 256 / 2,048 log lines | Format 64 event names, find delimiters, slice keys, update ordered string-key counts, project frequency array; strings, hashing, allocation |
| `closure_pipeline` | 1,024 / 8,192 elements | Return an escaping mutable accumulator closure, map input through it and reduce the output; captured cells, indirect calls, array algorithms |
| `array_growth` | 1,024 / 65,536 integers | Push with geometric growth, read/checksum all elements, pop and verify reverse order |
| `array_growth_reserved` | 1,024 / 65,536 integers | The same push/read/pop workload, reserving capacity once before appending |

## Measurement contract

Input generation runs once outside timing on both sides. The common fixture
contains integers `0..size-1`. Vela creates an `int[]`, reserves capacity and pushes
the input elements; C++ uses a vector and iota. Construction is outside timing.
The initial measurements below predate growable arrays and used `Set<int>.values()`
for the same untimed input preparation. Current measurements are recorded with
the [growable-array implementation](dynamic_arrays.md).

Each timed call starts fresh algorithm state. QuickSort creates its transformed
array, particles creates its objects, word counting creates its strings and map,
and the closure pipeline creates its captured state and output array. These
allocations remain timed. Neither implementation sorts already-sorted leftovers
or accumulates state across calls. Pure numerical work is also repeated fully.

Both sides use the same constants, iteration limits, partition decisions and
checksums. Integer data use 64-bit storage, including sieve flags; the C++ case
does not replace flags with a bitset or quicksort with a different sort.
QuickSort recurses on the smaller partition and loops over the larger one on
both sides, bounding stack depth to O(log n). The deterministic pivot still
allows input-dependent, potentially quadratic running time.

C++ kernels compile in their own translation unit with the project's normal
Release optimization, without forced `volatile`, disabled inlining or artificial
allocation. An input compiler barrier and a result barrier surround the native
call. Vela executes validated compiled bytecode. Each side checks the fixed
expected checksum on every invocation, including warm-up.

C++ retains normal optimizer advantages: lambdas may inline and capture state
may stay on the stack; short strings may use SSO. It uses Abseil flat hash lookup
plus a frequency vector for insertion order. Particle objects and tree nodes are
individually allocated with RAII ownership; particles are not replaced by an
unrelated contiguous SoA implementation. This is an optimized C++ implementation
of the matching workload, not a claim about the fastest possible C++ algorithm.

Registration and timing boundaries:

- `compare/cpp/NAME/SIZE/real_time`: prepare input and warm once outside
  timing; measure complete native calls, including local resource destruction.
- `compare/vm/NAME/SIZE/real_time`: compile, prepare input, warm once and clean
  up the preparation garbage outside timing. Measure repeated execution in the
  same Isolate and VM, including automatic incremental GC.
- `compile/representative/NAME/SIZE/real_time`: load both source texts outside
  timing; measure compilation, optimization, validation and resource destruction
  in a fresh Isolate per invocation. Compilation is never added to the VM/C++
  execution ratio.

GC reclamation is amortized in the VM measurement; a final complete collection
for retained-memory reporting is outside timing. RAII destruction is inside
each native call. Therefore the tree result compares the actual runtime
lifetime policies and is not a pure allocator or collector comparison.

Google Benchmark owns clocks, iteration calibration, repetitions and statistics.
Comparison mode defaults to seven repetitions, at least 0.2 seconds per timed
sample, real/wall time and randomized repetition interleaving. No slow samples
are discarded. CV is sample standard deviation divided by sample mean, reported
separately for C++ and Vela. One-sample smoke runs show no CV estimate.

The comparison command rejects benchmark errors even if Google Benchmark exits
successfully, rejects missing pairs and invalid times, and does not emit a
success summary from stale output. JSON retains all individual samples and
Google Benchmark aggregates. There is no aggregate language score.

## Running

```powershell
python -m tools.vl bench --compare
python -m tools.vl bench --compare --report out/benchmarks/my-run.json
python -m tools.vl bench --compare -- --benchmark_filter='^compare/(cpp|vm)/particles/' --benchmark_repetitions=9
```

The default output is `out/benchmarks/comparison.json` and
`out/benchmarks/comparison.md`. Use `--report` to choose another JSON path.
Comparison mode requires Release; normal `vl bench --variant debug` remains
available for correctness checks. Existing benchmark commands are unchanged.

Each workload also has a `main(): void` that prepares the smaller fixture and
prints its checksum:

```powershell
.\out\release\vela.exe run benchmarks/workloads/particles.vela
# 8411000
```

Printing and the independent CLI process startup are not in the paired timing.

## Measured results: 2026-10-25

Windows x64, AMD Ryzen 9 7900X (12 cores / 24 hardware threads), repository-pinned
Fuchsia Clang 23.0.0git (`80743bd43fd5b38fedc503308e7a652e23d3ec93`),
C++23, `/O2 /MD /DNDEBUG`, and Google Benchmark 1.9.5.
The C++ kernels and VM use the same toolchain and Release executable. This run
does not pin CPU affinity or disable normal Windows scheduling/power behavior;
the variation columns describe the observed sample spread.

Command:

```powershell
python -m tools.vl bench --compare --report out/benchmarks/representative-2026-10-25.json
```

Raw samples: [JSON](../out/benchmarks/representative-2026-10-25.json).
Generated summary: [Markdown](../out/benchmarks/representative-2026-10-25.md).
These are local generated artifacts; the measured tables below remain in version
control. All 42 cases produced seven successful samples, for 294 samples total.

Median wall time per complete workload; compilation and input preparation are excluded
from execution. Ratio = Vela VM / optimized C++; lower is better. Compile is separate.

| Workload | Size | Samples C++/VM | C++ (us) | Vela (us) | Ratio | CV C++/VM | Compile (us) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| binary_trees | 6 | 7/7 | 57.833 | 207.323 | 3.58x | 5.5%/4.6% | 120.754 |
| binary_trees | 9 | 7/7 | 479.512 | 1807.739 | 3.77x | 7.1%/6.1% | 117.530 |
| closure_pipeline | 1024 | 7/7 | 2.378 | 153.708 | 64.64x | 2.0%/5.3% | 126.734 |
| closure_pipeline | 8192 | 7/7 | 18.885 | 1214.614 | 64.31x | 1.9%/7.8% | 119.922 |
| mandelbrot | 64 | 7/7 | 160.344 | 5329.375 | 33.24x | 1.7%/7.2% | 129.548 |
| mandelbrot | 128 | 7/7 | 665.995 | 21305.207 | 31.99x | 3.5%/8.0% | 130.522 |
| particles | 128 | 7/7 | 10.592 | 962.967 | 90.91x | 9.2%/6.8% | 194.901 |
| particles | 1024 | 7/7 | 85.784 | 7392.816 | 86.18x | 9.8%/4.4% | 210.068 |
| quicksort | 1024 | 7/7 | 10.157 | 905.311 | 89.13x | 6.5%/6.0% | 208.524 |
| quicksort | 8192 | 7/7 | 102.274 | 6121.851 | 59.86x | 5.8%/8.2% | 214.321 |
| sieve | 10000 | 7/7 | 6.783 | 1200.915 | 177.05x | 5.0%/4.4% | 125.188 |
| sieve | 100000 | 7/7 | 77.840 | 14026.680 | 180.20x | 6.4%/10.9% | 114.250 |
| word_count | 256 | 7/7 | 6.865 | 67.949 | 9.90x | 3.9%/6.0% | 134.430 |
| word_count | 2048 | 7/7 | 44.851 | 513.051 | 11.44x | 2.5%/8.6% | 131.406 |

VM heap snapshots in bytes (medians; committed is an end-of-run snapshot, not a peak).
Baseline and retained include the rooted input and program constants; retained is after GC.
These are Vela heap counters, not process RSS or C++ allocator measurements.

| Workload | Size | Baseline | After GC | Committed | Registers |
| --- | ---: | ---: | ---: | ---: | ---: |
| binary_trees | 6 | 128 | 128 | 66688 | 306 |
| binary_trees | 9 | 128 | 128 | 649344 | 414 |
| closure_pipeline | 1024 | 8240 | 8240 | 26576 | 153 |
| closure_pipeline | 8192 | 65584 | 65584 | 131680 | 153 |
| mandelbrot | 64 | 1024 | 1024 | 1152 | 117 |
| mandelbrot | 128 | 2048 | 2048 | 2176 | 117 |
| particles | 128 | 2048 | 2048 | 66304 | 108 |
| particles | 1024 | 8240 | 8240 | 327072 | 108 |
| quicksort | 1024 | 8240 | 8240 | 41776 | 576 |
| quicksort | 8192 | 65584 | 65584 | 131424 | 792 |
| sieve | 10000 | 80048 | 80048 | 160352 | 99 |
| sieve | 100000 | 800048 | 800048 | 1600352 | 99 |
| word_count | 256 | 4288 | 4288 | 64128 | 90 |
| word_count | 2048 | 16624 | 16624 | 67184 | 90 |

All 98 VM samples returned to their original retained-byte baseline after the
final collection. This verifies the checked loads' post-GC retained state; it
does not establish a peak-memory bound or prove the absence of all leaks.

The largest current execution gaps are in sieve, particle updates, recursive
sorting and callbacks. Use these as profiling targets for numeric/indexing
dispatch, field/method execution and callback invocation. The measurements alone
do not isolate any one of these costs. Allocation-heavy tree workloads show a
smaller ratio, so a conclusion that GC is the dominant general bottleneck is
not supported by this run.

## Validation

- All 42 new cases ran successfully in Debug with one execution per case.
- Final Release run: 294 samples, fixed checksum checks throughout, zero errors.
- All seven standalone CLI scripts printed their expected small-case checksum.
- 80 tooling tests passed, including report pairing, unit conversion, median
  calculation, failed-run handling and user-supplied repetition options.
- New C++ sources and header passed project lint with zero warnings.

