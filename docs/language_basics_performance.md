# Basic language validation and performance

These measurements precede the register allocation and prepared-program
refactor. See [current execution measurements](vm_execution.md) for that change.

Measurements were collected on Windows x64 on 2026-10-24 with the repository's
Release Clang toolchain and Google Benchmark. Each time below covers the entire
script workload, not one instruction or one call. Semantics and storage
contracts are specified in [Basic language contracts](language_basics.md).

## Validation

- Debug: 324 GoogleTest cases passed.
- Release: 324 GoogleTest cases passed.
- Static analysis: all 70 GN-declared C++ source/header files checked. The one
  failing compiler source was fixed and rechecked; subsequent changes to the
  type checker, lowering, and tests also passed targeted checks. No warning
  suppression or analysis configuration changes were made.
- All 19 example sources passed checking; all 17 entry programs executed.
  `switch.vela` prints `close` and returns `121`; `nullable.vela` prints
  `label: Vela`, `empty`, and returns `42`.
- Both CLI commands correctly report an imported type error as
  `broken.vela:2:10`, rather than attributing it to the entry file.
- The timed benchmark suite ran 63 cases without errors. Final focused
  measurements ran 13 cases, followed by a separate field-access measurement.
  A final single-iteration run verified all 63 benchmark cases against the
  final compiler and runtime.

Tests cover fallthrough, shared labels, a default in the middle, nested loop
control, constant labels, complete returns, nullable parameters/returns/fields,
nested nullable arrays, short-circuit promotion, assignment invalidation,
evaluation-order snapshots, GC barriers during marking and sweeping, scope
boundaries, declaration order, parser recovery, and exact diagnostic locations.

## New workloads

The following are median elapsed times from five repetitions with a minimum
measurement time of 0.2 seconds per repetition:

| Workload | Time | Work performed |
| --- | ---: | --- |
| `vm/switch_dispatch` | 76.081 us | 1,000 eight-way selections using switch |
| `vm/if_dispatch` | 74.019 us | The same selections using an if/else chain |
| `vm/nullable_calls` | 37.837 us | 1,000 calls with a nullable result and guard |
| `vm/function_calls` | 28.304 us | 1,000 equivalent non-nullable calls |

Switch was about 2.8% slower than the if/else workload in this run. Its dispatch
currently uses O(n) typed comparisons and jumps. This measurement does not
justify claiming jump-table performance.

The nullable-call workload was about 33.7% slower than the non-nullable calls.
It executes an additional null comparison and control-flow branch per call;
the nullable value itself does not allocate a wrapper object. All four
workloads reported zero GC heap allocation after their first execution.
These counters exclude native VM register backing storage and compiler memory;
they do not mean that execution performs no native allocation.

## Existing workloads

The saved baseline used three repetitions of at least 0.05 seconds. The final
focused measurement used five repetitions of at least 0.2 seconds. Times below
are medians in microseconds:

| Workload | Baseline | Final focused | Observed change |
| --- | ---: | ---: | ---: |
| `compile/integer_loop` | 5.639 | 5.802 | +2.9% |
| `compile/function_calls` | 6.239 | 6.900 | +10.6% |
| `compile/field_access` | 8.242 | 8.926 | +8.3% |
| `compile/array_traversal` | 12.082 | 12.378 | +2.4% |
| `compile/double_loop` | 6.712 | 7.102 | +5.8% |
| `vm/integer_loop` | 22.882 | 22.550 | -1.5% |
| `vm/function_calls` | 30.607 | 28.304 | -7.5% |
| `vm/field_access` | 29.586 | 35.195 | +19.0% |
| `vm/array_traversal` | 452.650 | 415.460 | -8.2% |
| `vm/double_loop` | 36.981 | 29.759 | -19.5% |

There was material timing variance. The focused field-access run had 11.3%
elapsed-time coefficient of variation, and double-loop variation was 14.6%.
A separate field-access run with seven repetitions of at least 0.5 seconds
measured 28.872 us, with 3.6% variation, or -2.4% against the saved baseline.
The full batch measured field access at 28.538 us. The isolated result does
not erase the slower focused result; together they show that a stable 19%
regression was not reproduced.

Baseline and current binaries were not alternated under a controlled CPU
configuration. These are observed measurements, not causal speedup claims or
proof that every workload is regression-free. Compilation now performs more
static analysis, and the observed compilation increases remain visible.

An initial implementation stored and copied the checked type of every
expression, increasing compilation time by roughly 15%-21% in the first
comparison. The final implementation retains contextual type facts only for
local reads and array literals. Fixed operators keep their existing result
contracts. This removes duplicate type storage and copies without adding a
runtime fallback or weakening null checking.

## Memory behavior

Nullable locals and fields use the existing 16-byte tagged `Value` slots.
Nullable primitive arrays use contiguous tagged slots, without per-element
allocations. Nullable strings, objects, builders, and nested arrays retain
pointer-sized element storage. Non-nullable primitive arrays retain their
native compact storage. Primitive arrays require no element scan during GC,
including nullable primitive arrays.

Source locations add a source ID, while paths are stored once per program.
Only produced diagnostics own a path string. Checked enum ordinals are shared
with lowering rather than being rediscovered by scanning declarations.

## Reproduction and records

```powershell
python -m tools.vl test debug
python -m tools.vl test release
python -m tools.vl lint
python -m tools.vl bench -- --benchmark_min_time=0.1s --benchmark_repetitions=3
python -m tools.vl bench -- --benchmark_filter="^vm/(switch_dispatch|if_dispatch|nullable_calls|function_calls)$" --benchmark_min_time=0.2s --benchmark_repetitions=5
```

Local records are under `.cache/`:

- `p0-before.json` and `p0-vm-before.json`: measurements before implementation.
- `language-basics-debug.log` and `language-basics-release.log`: final tests.
- `language-basics-full-lint.log`: full repository check, including the compiler
  findings subsequently fixed.
- `language-basics-compiler-lint.log`, `language-basics-type-lint.log`, and
  `language-basics-final-type-lint.log`: successful checks of the changed files.
- `language-basics-final-benchmark.json`: timed full-suite results after
  reducing duplicate type metadata.
- `language-basics-focused.json`: final focused measurements.
- `language-basics-field-check.json`: longer field-access measurement.
- `language-basics-final-smoke.json`: final 63-case result verification.
