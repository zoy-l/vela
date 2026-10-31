# Async Functions and Future Semantics

[**English**](async_tasks.md) | [简体中文](../zh-CN/async_tasks.md)

> Status: implemented. This document defines the async/Future language, runtime, host, and CLI contracts.

## Goals

Vela keeps the familiar `async` / `await` syntax and uses `Future<T>` for asynchronous results. It provides Future-style composition APIs without claiming to match every detail of JavaScript `Promise` or Dart `Future`. A Future is a strongly typed value that can be observed more than once and completes once; failures use Vela's exception contract.

The design covers async functions, Future composition, error propagation, async main, host async operations, cancellation signaling, timers, GC lifetime, and performance validation. Async/await is single-threaded cooperative execution; it does not create parallel work by itself.

## API Example

```vela
class User { name: string; }

external func fetchUser(id: int): Future<User>;

async func loadUsers(ids: int[]): Future<User[]> {
    let first = fetchUser(ids[0]);
    let second = fetchUser(ids[1]);
    return await Future.wait([first, second]);
}

async func main(): Future<int> {
    try {
        let users = await loadUsers([7, 8]);
        print(users[0].name);
        return 0;
    } catch {
        print("Could not load users");
        return 1;
    }
}
```

## Syntax and Types

- Async functions, methods, and closures use the `async` modifier. Their explicit return type is `Future<T>`, and a call expression also has type `Future<T>`.
- An async body may return a `T` or another `Future<T>`; the outer Future adopts that Future's result. An ordinary function may also explicitly return `Future<T>`.
- Function values express their actual call result, for example `(int) => Future<User>`. `async` does not change the function value type, so async functions and ordinary functions returning the same Future type share the same callable contract.
- `await e` is valid only inside an async function. If `e` is `Future<T>`, the result is `T`; for a plain value, the result type is unchanged. Awaiting a nullable Future that is null produces null.
- Async lambdas, instance methods, static methods, and interface implementations follow the same rules. Constructors, static initializers, and module initializers cannot be async; module top level does not allow await.
- `Future<T>` is a built-in generic reference type that can be stored, passed, returned, and captured. A Future adopts only another Vela Future; it does not inspect an arbitrary object's `then` member.

## Future API

```vela
const producer = Completer<int>();
const result = producer.future.then((value) => value + 2);
Timer.after(0, (): void => { producer.complete(40); });
const answer: int = await result;
```

The example runs inside an async function. `Completer<T>()` also accepts a contextual `Completer<T>` type. Its read-only properties are `future: Future<T>` and `isCompleted: bool`. `isCompleted` becomes true when completion is requested, including adoption of a pending Future. `complete(value)` accepts `T` or `Future<T>`; `Completer<void>.complete()` takes no argument. `completeError(error)` requires a non-null class or interface instance. Duplicate completion throws `Error` with code `FutureAlreadyCompleted`; direct or indirect self-adoption throws code `FutureSelfAdoption`.

`then` infers its result from the callback's return type. For a `Future<void>` source, the success callback takes no value parameter. Error callbacks take exactly one non-null class or interface parameter; an unannotated lambda parameter defaults to `Error`. The callback handles matching payload types, and other errors propagate unchanged. `catchError` must return the source value type or a Future of that type. `whenComplete` accepts a zero-argument action returning `void` or `Future<void>`; an asynchronous action finishes before the result is forwarded. Optional callbacks may be null. Erased receivers use the same scheduler and validate retained callback signatures before registering continuations. Callback results that enter a concrete Future payload are checked before completion; see [dynamic values](json.md).

`Future<T>` is covariant in its result type; `Completer<T>` remains invariant. Future result types are flattened: factories and callback APIs adopt returned Futures, and nested Future payload type arguments are rejected. `Future.wait` returns `Future<T[]>` for value tasks. For `Future<void>[]`, it returns `Future<void>` because Vela has no `void[]` value type; a cleanup callback for this case takes no arguments and is never invoked. Explicit factory type arguments and contextual result types are checked against payload and callback result types.

- `Future.value(value)` and `Future.error(error)` create already-completed Futures with a value or error. If `Future.value` receives another Future, it adopts that Future's result; it does not inspect arbitrary objects for a `then` member.
- `Future.sync(computation)` invokes the computation immediately and converts its return value, returned Future, or thrown exception into a Future result.
- `Future.microtask(computation)` queues the computation on the microtask queue. `Future.delayed(milliseconds, computation)` invokes it after a timer; `sleep(milliseconds)` returns a Future that completes with `void` after the delay.
- `then(onValue, onError)`, `catchError(onError)`, and `whenComplete(action)` return new Futures. Callbacks may return a value or Future; a thrown callback error fails the new Future. Callbacks are always notified asynchronously, even when the source Future is already complete.
- `Future.wait<T>(values, eagerError: false, cleanUp: null)` collects successful values in input order; an empty input succeeds immediately with an empty array. By default it records the first error and waits for every input before failing; `eagerError` can fail the aggregate as soon as the first error arrives. With `cleanUp`, non-null values produced successfully are cleaned up when the aggregate fails; remaining inputs are still observed.
- `Future.race<T>(values)` adopts the first success or failure; an empty input remains pending, and losing inputs are not cancelled.
- `future.timeout(milliseconds, onTimeout)` adopts the source Future if it completes in time. Otherwise it adopts the value or Future returned by `onTimeout`, or fails with `TimeoutException`. Timeout does not cancel the source operation, which continues to be observed.
- `Completer<T>` creates and completes a Future for event callbacks and other non-async producers. `complete` accepts a value or Future, adopting the latter; `complete` / `completeError` may be called once, and an error must be a Vela-thrown object. A second completion violates the contract. Completion notifications are always asynchronous so listeners cannot reenter the producer.

A Future may have multiple waiters. Its value or error remains valid until all listeners consume it. Composition APIs observe every input result, so a later input error is not discarded when an aggregate Future fails early. Cleanup runs once for each non-null successful input when a wait fails, including successes arriving after an eager failure. A cleanup error follows the unhandled Future error rules and does not replace the aggregate's first error.

Wait and race inputs share one GC-owned slot object for aggregate references. Each input retains that object, so marking the aggregate scans its reference list once. The shared slots also keep cleanup callbacks and late results reachable after an eager failure.

`future.timeout(milliseconds, onTimeout: null)` returns the source value type. `TimeoutException` is an independent throwable class with `message: string` and `code: string` (default `Timeout`); use a typed catch or an error callback with a `TimeoutException` parameter. A source failure containing `TimeoutException` is forwarded normally and does not trigger `onTimeout`.

## Execution and Scheduling

Calling an async function immediately runs its body until it returns, throws, or reaches the first await, then returns a Future. Errors in the body become a failed Future rather than being thrown synchronously to the caller. `await` is an asynchronous boundary: even when the awaited Future is already complete or the expression is a plain value, the continuation resumes asynchronously after the current native execution slice exits.

Scheduling order is: finish or suspend the current native execution slice; run queued Future continuations in FIFO order; when the microtask queue is empty, process one timer or host event; then drain microtasks created by that event. One Future completion notifies waiters in registration order.

Both JIT and AOT reuse one result Future across repeated awaits in the same async invocation. A plain return value or uncaught error after resumption completes that result directly, without internal Future forwarding tasks; returning another Future still adopts its completion. Every await resumption and result observer notification runs through the microtask queue.

A Future has three observable outcomes: pending, completed with a value, or completed with an error. Its mutable state is not exposed. A Future cannot be cancelled directly. A cancellable host operation accepts a separate `CancellationToken`; the operation decides how to handle the request, and its Future still completes with a normal value or Vela exception.

## Errors and Lifetime

Throws, runtime errors, and await propagation in async functions use Vela's existing exception objects and source traces. An error not caught in the function becomes its Future's failed result. Awaiting a failed Future rethrows the original exception at the continuation, preserving existing catch/finally behavior. Async diagnostics retain the original throw location and await boundaries.

At the end of a microtask checkpoint, the host is notified about unhandled Future errors; if a handler is attached later, the host may receive a handled notification. `main` may return `Future<int>`; its failure becomes the program's execution failure. Unhandled errors from background Futures are reported as runtime diagnostics.

Suspended locals, Future results, errors, waiters, and queued callbacks remain reachable to the GC. Continuations retain native program state, code ownership and diagnostic metadata and borrow their Isolate. The host must keep the Isolate alive until its native instances, ordinary Future handles and active host accesses are released. Native code runs only on the owning Isolate thread; external threads post completion messages for that thread to apply to Futures.

Future values, listeners, native activations, GC state, and timer queues belong to the Isolate thread. Host threads publish immutable completion messages through a lock-free MPSC mailbox; the Isolate applies one host event and then drains its microtasks. A semaphore parks the owner while waiting for a host event or timer. Completion reservation and cancellation state use atomics; closeable completion endpoints and timer cancellation serialize access to their scheduler pointer with shutdown. Future state inspection, listener registration, heap operations, and native-instance destruction stay on the owner thread. Native cancellation callbacks may run on the cancelling or subscribing thread; they must post script work through the scheduler.

`NativeContext::CreateCompleter()` registers a live host producer. Completing it, or dropping every producer handle, releases that event source. `NativeContext` binds returned native Futures to its scheduler; a Future already belonging to another Isolate is rejected. The external declaration's `Future<T>` payload is checked when the native Future completes. An incompatible value or a non-throwable error fails the checked Future with `TypeError`. Heap values posted from another thread must already belong to the Isolate and remain rooted until publication; allocation and encoding happen on the owner thread.

For a host async operation, obtain `completer.future()` and then transfer completion capability with `std::move(completer).Detach()`. The producer retains only the resulting `FutureCompletionEndpoint`. Endpoints are copyable and may outlive the Isolate. Scheduler shutdown closes them and releases their Future and callback references on the owner thread before discarding queued events and timers. Late completion returns `FutureCompletionError::Closed` and cannot enter a new run. An accepted completion request still follows the single-completion rule. Dropping the last pending endpoint handle schedules cleanup on the owner thread and unregisters its host event source. Ordinary Future, Completer, and Program handles must still be released before Isolate destruction; an endpoint does not extend the rooting lifetime of heap values.

For C++ `Future::Complete`, an owner-thread call checks Future adoption synchronously. A cross-thread call returns whether the completion request was accepted; the owner thread checks adoption when applying that request. A rejected adoption completes the Future with the corresponding diagnostic, including `FutureSelfAdoption` or `InvalidIsolate`. Cross-thread callers must observe the Future for its final outcome.

`FutureScheduler::PumpOne()` and `PumpUntil()` return idle when no immediately available work or timer can advance execution. Their `WaitForHost` mode also waits for registered native event sources. Suspensions retain live register values and trace global slots through the Future graph; unreachable pending tasks and cycles involving Future results remain collectible. REPL submissions share the same growing global storage, so continuations from earlier submissions observe and update current globals.

The CLI waits for the root main Future and all live host event sources. A pending Future with no producer does not keep the process alive; if the root Future remains pending with no work that can advance it, the CLI reports async idle/deadlock. An embedding pump returns idle so the host may resume it later.

## Host Events and Timers

Async result semantics do not depend on a clock; I/O and timers belong to the host event layer. The standard library provides `sleep(milliseconds)`, `Timer.after(milliseconds, callback)`, and `Timer.periodic(milliseconds, callback)`. Both Timer factories return a `Timer` handle canceled with `timer.cancel()`; `Timer.after` fires once and `Timer.periodic` fires repeatedly. Callbacks run on the Isolate thread. A Future returned by a timer callback does not block later ticks, and its error follows the normal unhandled Future error rules.

Timers use a monotonic clock. Intervals do not replay missed ticks, avoiding an unbounded callback backlog; events with the same deadline run in creation order. After each host event, its microtasks are drained before the next event is processed.

Delays are nonnegative integer milliseconds within the monotonic clock's representable range; periodic intervals must be positive. Invalid durations throw a catchable `Error`. Timer callbacks take no arguments and return `void` or `Future<void>`. Cancelling a timer is idempotent and releases its retained callback immediately on the owner thread. `CancellationToken()` exposes `isCancelled: bool` and idempotent `cancel()`; the typed native codec shares the same request state with the host.

## Performance and Implementation Boundaries

The language contract does not prescribe how native execution stores suspended execution state. It requires ordinary synchronous functions to retain their existing fast path, async calls without an await to avoid suspension scheduling, and suspended execution to retain only the live state needed to resume. Resuming one Future or task must not scan the whole program, and the GC must precisely trace values live across suspension.

Async semantics flow through the AST, type checker, Typed IR, optimizer, native code generation, debugger, host API, and CLI. Await can suspend and throw, so it is observable; optimizers cannot remove it, move operations across it, or reorder it incorrectly. Direct calls, closure calls, and interface dispatch expose the same Future result.

Required regressions cover synchronous effects before the first await, completion without await, asynchronous continuation of an already-complete Future, FIFO notification, multiple waiters, Future adoption, Future composition success and failure, timeout, Completer, catch/finally across await, all call forms, async main, unhandled errors, GC during suspension, host-thread completion, timer ordering, and idle/deadlock boundaries.

Release benchmarks separately measure async calls without await, awaiting a completed Future, suspend/resume, concurrent Future count, GC pressure from suspended state, and timer scale. Report synchronous native regression, retained memory per suspended execution, and first async allocation cost.

On 2026-10-26, the Windows x64 Release build with the pinned Clang toolchain on a
Ryzen 9 7900X passed all 14 async benchmark cases, with seven repetitions each.
Median wall times were:

| Workload | Time (µs) |
| --- | ---: |
| Async call without await | 0.351 |
| Await an already completed Future | 1.238 |
| Pending microtask and continuation | 1.964 |
| First async call in a fresh Isolate | 1.695 |

The first-call case excludes compilation and destruction and includes timing
pause/resume overhead. At 1,000 instances, concurrent tasks took 2.186 ms,
tasks with an explicit collection while suspended took 8.797 ms, and zero-delay
timers took 0.810 ms. All steady cases ended with zero suspended frames and no
additional retained GC heap values after collection.

Nonzero saved-state measurements ranged from 193 to 210 bytes per suspended
frame. The first call without await allocated 64 GC-accounted bytes before
pumping; these counters exclude native Future control blocks and allocator
bookkeeping, so they do not measure total process memory. The synchronous
`integer_loop`, `function_calls`, and `field_access` cases had median wall-time
ratios of 0.942, 0.776, and 0.833 against the original workspace snapshot. No
regression was observed in those three workloads. Full commands, source
fingerprints, variation, counters, and measurement limits are retained in the
[benchmark summary](../../benchmarks/results/rolling_summary.json).

## Implementation References

- Dart async/await and Future API: [language guide](https://dart.dev/language/async), [Future API](https://api.dart.dev/dart-async/Future-class.html), [Future implementation](https://github.com/dart-lang/sdk/blob/main/sdk/lib/async/future_impl.dart).
- QuickJS-NG async frames, await, and Promise jobs: [async state](https://github.com/quickjs-ng/quickjs/blob/master/quickjs.c#L21163-L21224), [await opcode](https://github.com/quickjs-ng/quickjs/blob/master/quickjs.c#L20868-L20878), [job queue](https://github.com/quickjs-ng/quickjs/blob/master/quickjs.c#L2488-L2552).
- QuickJS-NG keeps OS timers in its host standard library: [timers](https://github.com/quickjs-ng/quickjs/blob/master/quickjs-libc.c#L2471-L2609), [event loop](https://github.com/quickjs-ng/quickjs/blob/master/quickjs-libc.c#L4852-L4876).
- Luau coroutines use explicit `lua_State` values, independent VM stacks, and manual resume/yield; they do not provide Future/Promise scheduling: [thread state](https://github.com/luau-lang/luau/blob/master/VM/src/lstate.h#L285-L305), [resume/yield](https://github.com/luau-lang/luau/blob/master/VM/src/ldo.cpp#L814-L869).
