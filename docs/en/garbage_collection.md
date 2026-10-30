# Garbage collection

[**English**](garbage_collection.md) | [简体中文](../zh-CN/garbage_collection.md)


Vela uses a precise, non-moving, incremental mark-and-sweep garbage collector.
The runtime cooperatively advances collection alongside allocations to avoid
long stop-the-world pauses, while maintaining a predictable single-threaded
execution and memory footprint.

## Collector Architecture

The collector design adopts an incremental mark-and-sweep model inspired by
systems such as Luau's incremental GC:

- **Precise Rooting**: The collector tracks all live references precisely.
  Roots include VM registers in active call frames (`RootRange`), global/module
  variable tables, string and constant pools, and host-managed root handles.
  There is no conservative stack scanning.
- **Tri-color Marking**: Objects are conceptually white (unvisited/unreachable),
  gray (visited but references unscanned), or black (visited and references scanned).
- **Incremental Steps**: Instead of running full mark and sweep phases in one
  uninterruptible stop-the-world pause, the collector divides cycles into small
  `Step` increments executed during mutator allocations.
- **Write Barriers**: Mutator writes to heap references trigger write barriers
  (forward or backward) to preserve the tri-color marking invariant when black
  objects are updated to point to white objects. Primitive values (integers,
  doubles, booleans) do not trigger write barriers.
- **Non-moving Allocation**: Objects remain at stable heap addresses throughout
  their lifetimes. This eliminates pointer-forwarding overhead and simplifies
  C++ embedding and host handle interop.

## Generational Considerations and Tradeoffs

Vela deliberately avoids a generational nursery and remembered sets in its
baseline collector:

- Generational collection provides benefits when workloads exhibit the weak
  generational hypothesis (most objects die very young). However, in workloads
  with high survival rates or large long-lived graph mutations, generational
  overheads—such as card tables, remembered sets, old-to-young write barriers,
  and minor/major collection pacing—can exceed the cost of incremental tracing.
- Adding a nursery requires age tracking on all allocations, promotion policies,
  and more complex mutator write barriers.
- Retaining a unified non-generational incremental collector minimizes object
  header overhead, avoids dual-mode collector complexity, and provides consistent,
  predictable latency bounds across varied script workloads.

## Step Budgeting and Scheduling

Collection work is budgeted and scheduled cooperatively:

- **Budget Units**: `Step` budgets count discrete objects marked or swept rather
  than relying on CPU timestamps or elapsed wall-clock nanoseconds.
- **Pacing**: As allocation pressure increases, the VM schedules incremental GC
  steps proportional to allocated bytes. This ensures collection completes before
  memory limits are exceeded without halting execution.
- **Indivisible Operations**: Root rescan at the end of the mark phase and
  scanning large individual array buffers are indivisible within a single step.
  Large sequential structures must be partitioned if sub-millisecond step
  latencies are strictly required.
- **Thread Affinity**: All GC steps, root scans, and sweeping execute strictly
  on the owning `Isolate` thread. Multi-threaded hosts communicate across isolates
  via message queues rather than shared GC pointers.

## Write Barriers and Field Stores

Native optimization may remove unobservable local class storage while preserving
constructor arguments, initialization, failures and side effects in their original
order. `OutOfMemory` applies to actual storage requests; each source constructor
need not allocate, and allocation failure need not occur at the same source point
in the VM and native backends. Heap limits account for actual requests. Allocation
and collection counts are implementation observations. See the
[escape and materialization contract](../zh-CN/escape_analysis.md#13-分配失败与资源观测).

- **Reference Stores**: Storing a heap reference into an object field, array
  element, or closure capture triggers the GC write barrier. The barrier shades
  either the target object or the assigned value to maintain tri-color invariants.
- **Primitive Stores**: Integer, floating-point, boolean, and null writes bypass
  the barrier completely, allowing direct register-to-memory stores on hot paths.
- **Frame Recycling**: Reused execution frames clear reference tags before
  registration, preventing stale values from prior invocations from retaining
  objects across collection cycles.

ByteArray stores its header and fixed raw bytes in one accounted heap allocation
and is traced as a leaf. ByteData stores a strong owner reference initialized
before publication; tracing visits that owner even for an empty view. No byte
content is scanned for references. Views, including those reached from fields,
closures, typed containers or suspended async frames, retain the entire
backing allocation. See [byte ownership](byte_data.md#ownership-mutation-and-gc).

RegExp traces its original source and owns its PCRE2 program. RegExpMatch traces
both its input and pattern; capture spans live inline in the match allocation.
PCRE2 callbacks allocate through accounted native blocks, including compiler
contexts, compiled code, match data and backtracking frames. Boundary maps and
capture staging use the same accounting. Work buffers are operation-owned;
compiled code is released with the pattern. Allocation safe points retain all
borrowed values and partial arrays, using the existing root/publication barriers.

## Verification Contracts

Correctness and memory invariants are enforced by validation suites:
- Exact survivor assertions: synthetic and functional graphs verify that live
  object counts match theoretical reachable topologies after each cycle.
- Stress mode: running incremental steps on every allocation validates root
  completeness and write barrier coverage.
- Host integration: verifying that host-held handles, exception unwinding, and
  VM context tear-down correctly release or trace GC resources.
