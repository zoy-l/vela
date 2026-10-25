# P2 GC strategy decision

Vela retains its precise, non-moving, incremental mark-and-sweep collector.
P2 does not add a nursery, remembered set or a second collection mode. This is
not evidence that generational collection is slower; no generational collector
prototype is presented as a measured competitor.

The reference implementation examined was
[Luau `lgc.cpp`](https://github.com/luau-lang/luau/blob/master/VM/src/lgc.cpp),
which explicitly describes incremental, non-generational, non-moving collection,
allocation assists and forward/backward barriers. Lua 5.4's generational mode
addresses a different tradeoff; adding it requires age tracking, old-to-young
barriers and major/minor pacing, not just changing an allocation threshold.

## Workloads and measurement boundaries

`gc/cohort/OLD/SURVIVORS/BUDGET/manual_time` constructs and collects a retained
object graph, then allocates 4,096 young strings. It keeps either 64 (1.56%) or
4,096 (100%) young objects reachable. Retained graph nodes also receive references
to surviving new strings, exercising ordinary reference stores. Collection counts
are verified against the exact expected survivor count. The benchmark owns its
roots and isolates throughout each cycle.

The timer covers `StartCollection` and all `Step` calls in one cycle. Graph setup,
allocation, percentile sorting and destruction are outside the manual-time interval.
Step timestamps and sample bookkeeping are inside it; these instrumented cycle
times must not be confused with uninstrumented allocation/GC VM throughput.
`p95_step_ns` is the average of each cycle's 95th percentile. `maximum_step_ns`
includes OS descheduling and is unsuitable as a hard collector pause bound.

Separate `vm/allocation_gc`, string, closure and collection cases measure normal
allocation, barriers and automatic GC without the step instrumentation. Their
final retained-object counters are checked after a full collection.

```powershell
python -m tools.vl bench -- --benchmark_filter=^gc/ --benchmark_min_time=0.1s --benchmark_repetitions=3 --benchmark_out=out/gc-strategy.json --benchmark_out_format=json
```

## Decision and limits

The initial sample found about 18 microseconds per collection with no retained
graph and about 176 microseconds with 8,192 retained objects plus 64 surviving
strings (budget 16). The latter keeps 1,056,768 live bytes in 1,572,864 committed
bytes. Budget 64 reduced the instrumented total cycle cost, but each call can
perform more work. Final acceptance records both throughput and step percentiles
before choosing an automatic-work budget. Final acceptance retained budget 16:
the 8,192-object/64-survivor profile measured a 0.21 us p95 step at budget 16 and
0.73 us at budget 64, with instrumented cycle times of 159.94 and 150.43 us.
See the [complete acceptance report](p2_acceptance.md) for final samples and limits.

Low-survival cohorts with a large stable graph demonstrate a plausible nursery
opportunity. A 100%-survival cohort demonstrates why that opportunity cannot be
assumed for all scripts. These synthetic cohorts are sensitivity measurements,
not captured game/UI application lifetime distributions. P2 retains the existing
policy rather than adding age metadata and barriers without a demonstrated host
workload benefit. A future generational implementation must beat the baseline
on retained-graph workloads while checking high-survival workloads, mutations,
host roots, exceptions and memory pressure.

`Step` budgets count objects, not bytes or nanoseconds. Root rescans and scanning
one large array, collection or object are indivisible today. The collector is
synchronous and does not promise zero stop-the-world time or hard real-time
pauses. Expensive object scanning should be made incremental before making such
a latency claim. Existing precise roots and write barriers remain required.
