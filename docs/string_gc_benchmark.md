# String GC measurements

Measured on 2026-10-24 using the repository's Windows Clang Release toolchain:

```powershell
python -m tools.vl bench -- --benchmark_min_time=0.2s --benchmark_repetitions=5 --benchmark_out=.cache/string-gc-benchmark.json --benchmark_out_format=json
```

All 33 benchmark cases completed without `error_occurred`. Times below are
median wall times per complete workload in microseconds, including automatic
collection during execution.

| String workload | Compile | First execution | Steady execution |
| --- | ---: | ---: | ---: |
| 1,000 short concatenations, calls, and equality checks | 10.076 | 76.621 | 63.245 |
| 512 one-byte appends, doubling, and final equality check | 9.715 | 29.104 | 21.316 |

After a final collection outside timing, the short-concatenation workload
retains three literal strings (192 bytes); the growth workload retains two
(128 bytes). Committed heap storage, including reusable free blocks, is 65,536
and 158,592 bytes respectively. These counters describe the final heap, not
peak process memory. The stress test also checks that committed storage remains
constant across 100 repeated executions after warm-up and collection.

The growth workload includes copying existing prefixes. Repeated one-byte
appends still copy O(n²) bytes in total; collection reclaims obsolete strings
without changing flat-string concatenation complexity.

Existing VM workloads compared with `.cache/func-control-flow-benchmark.json`:

| Workload | Previous | String GC | Change |
| --- | ---: | ---: | ---: |
| Integer loop | 22.919 | 22.513 | -1.77% |
| Function calls | 28.783 | 28.498 | -0.99% |
| Object allocation and GC | 44.527 | 45.062 | +1.20% |
| Array allocation | 199.742 | 191.449 | -4.15% |

These are comparisons with a saved run on the same toolchain; small differences
include measurement variation. There is no prior string-workload timing report.
