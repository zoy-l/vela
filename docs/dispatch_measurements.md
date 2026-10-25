# P2 VM dispatch experiment

Decision: retain the current switch dispatcher in P2. This is a measured code-size
and maintenance tradeoff, not a claim that computed goto is slower. The experiment
shows an opportunity worth revisiting for a dedicated interpreter optimization goal.

Measurements used the repository's Clang/LLVM Windows x64 Release build on
2026-10-25, five repetitions and a 0.2-second minimum per case. Wall-time medians
below are nanoseconds per complete workload; each run verified its actual result.
No builds, lint or regression tests ran concurrently with the benchmark process.

| Workload | switch | computed goto | change |
| --- | ---: | ---: | ---: |
| Integer loop | 19,528 | 19,215 | -1.6% |
| Direct calls | 29,742 | 26,279 | -11.6% |
| Closure calls | 55,359 | 46,275 | -16.4% |
| Interface calls | 50,156 | 42,831 | -14.6% |
| Field access | 27,561 | 22,698 | -17.6% |
| Caught exceptions | 95,453 | 88,486 | -7.3% |
| Allocation and GC | 46,080 | 46,561 | +1.0% |

`llvm-objdump --disassemble --demangle` confirmed that the non-debug `RunImpl`
grew from **5,607 to 21,742 bytes** (3.88x); indirect jumps increased from 29 to 59.
Thus this was an actual replicated computed-goto dispatch experiment, not an extra
switch hidden behind a jump table. The host reports a 32 KiB L1 instruction cache.
The code-size observation alone does not prove instruction-cache regressions in
real host applications; those mixed host/script workloads have not been measured.

The [reproduction patch](experiments/computed_goto.patch) replaces only `RunImpl`.
Apply it to the P2 switch implementation, run the command below, then reverse the
patch. It is an experimental artifact, not a second supported production path.
Clang label-address and indirect-goto extensions are intentional in this artifact.

```powershell
python -m tools.vl bench -- --benchmark_filter=^vm/ --benchmark_min_time=0.2s --benchmark_repetitions=5 --benchmark_out=out/dispatch.json --benchmark_out_format=json
```

The implementation follows the replicated dispatch points in
[Luau `lvmexecute.cpp`](https://github.com/luau-lang/luau/blob/master/VM/src/lvmexecute.cpp).
A generated implementation driven by the single opcode enum was also compiled and
passed 19 targeted exception/debugger/REPL tests. It still requires a deliberate
exception to the current function-complexity policy for the generated dispatch body.
No analysis policy was weakened for this experiment.

Two macro-free tail-call alternatives were also actually built and measured:

| Workload | `musttail`, normal ABI | `musttail`, `preserve_none` |
| --- | ---: | ---: |
| Integer loop | 23,975 | 18,660 |
| Direct calls | 38,565 | 32,216 |
| Closure calls | 62,432 | 54,046 |
| Field access | 33,718 | 39,948 |
| Allocation and GC | 53,237 | 72,679 |

`musttail` guarantees bounded native stack use, but these implementations did not
retain computed-goto's broad gains. The losing implementations, generator and build
dependencies were removed. Production retains the original always-inline execution
boundaries and a single switch implementation. Adoption should compare broader
opcode mixes and host instruction-cache pressure and explicitly review generated
dispatch policy; it should not substitute the slower tail-call implementation.

Raw local reports: `.cache/p2-{computed-goto,switch-paired,tailcall,tailcall-preserve-none}.json`.
These are samples from one host/compiler, not cross-platform performance guarantees.
