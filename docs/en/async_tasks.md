# Async Functions and Future Semantics

[**English**](async_tasks.md) | [简体中文](../zh-CN/async_tasks.md)

> Status: design specification. Async tasks are not implemented; implementation must satisfy the complete language, runtime, host, and validation contracts in this document.

## Goals

Vela keeps the familiar `async` / `await` syntax and uses `Future<T>` for asynchronous results. It provides Future-style composition APIs without claiming to match every detail of JavaScript `Promise` or Dart `Future`. A Future is a strongly typed value that can be observed more than once and completes once; failures use Vela's exception contract.

The design covers async functions, Future composition, error propagation, async main, host async operations, cancellation signaling, timers, GC lifetime, and performance validation. Async/await is single-threaded cooperative execution; it does not create parallel work by itself.

## API Example

```vela
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

- `Future.value(value)` and `Future.error(error)` create already-completed Futures with a value or error. If `Future.value` receives another Future, it adopts that Future's result; it does not inspect arbitrary objects for a `then` member.
- `Future.sync(computation)` invokes the computation immediately and converts its return value, returned Future, or thrown exception into a Future result.
- `Future.microtask(computation)` queues the computation on the microtask queue. `Future.delayed(milliseconds, computation)` invokes it after a timer; `sleep(milliseconds)` returns a Future that completes with `void` after the delay.
- `then(onValue, onError)`, `catchError(onError)`, and `whenComplete(action)` return new Futures. Callbacks may return a value or Future; a thrown callback error fails the new Future. Callbacks are always notified asynchronously, even when the source Future is already complete.
- `Future.wait<T>(values, eagerError: false, cleanUp: null)` collects successful values in input order; an empty input succeeds immediately with an empty array. By default it records the first error and waits for every input before failing; `eagerError` can fail the aggregate as soon as the first error arrives. With `cleanUp`, non-null values produced successfully are cleaned up when the aggregate fails; remaining inputs are still observed.
- `Future.any<T>(values)` adopts the first success or failure; an empty input remains pending, and losing inputs are not cancelled.
- `future.timeout(milliseconds, onTimeout)` adopts the source Future if it completes in time. Otherwise it adopts the value or Future returned by `onTimeout`, or fails with `TimeoutException`. Timeout does not cancel the source operation, which continues to be observed.
- `Completer<T>` creates and completes a Future for event callbacks and other non-async producers. `complete` accepts a value or Future, adopting the latter; `complete` / `completeError` may be called once, and an error must be a Vela-thrown object. A second completion violates the contract. Completion notifications are always asynchronous so listeners cannot reenter the producer.

A Future may have multiple waiters. Its value or error remains valid until all listeners consume it. Composition APIs observe every input result, so a later input error is not discarded when an aggregate Future fails early.

## Execution and Scheduling

Calling an async function immediately runs its body until it returns, throws, or reaches the first await, then returns a Future. Errors in the body become a failed Future rather than being thrown synchronously to the caller. `await` is an asynchronous boundary: even when the awaited Future is already complete or the expression is a plain value, the continuation resumes asynchronously after the current VM execution slice exits.

Scheduling order is: finish or suspend the current VM execution slice; run queued Future continuations in FIFO order; when the microtask queue is empty, process one timer or host event; then drain microtasks created by that event. One Future completion notifies waiters in registration order.

A Future has three observable outcomes: pending, completed with a value, or completed with an error. Its mutable state is not exposed. A Future cannot be cancelled directly. A cancellable host operation accepts a separate `CancellationToken`; the operation decides how to handle the request, and its Future still completes with a normal value or Vela exception.

## Errors and Lifetime

Throws, runtime errors, and await propagation in async functions use Vela's existing exception objects and source traces. An error not caught in the function becomes its Future's failed result. Awaiting a failed Future rethrows the original exception at the continuation, preserving existing catch/finally behavior. Async diagnostics retain the original throw location and await boundaries.

At the end of a microtask checkpoint, the host is notified about unhandled Future errors; if a handler is attached later, the host may receive a handled notification. `main` may return `Future<int>`; its failure becomes the program's execution failure. Unhandled errors from background Futures are reported as runtime diagnostics.

Suspended locals, Future results, errors, waiters, and queued callbacks must remain reachable to the GC. The execution session retains the Isolate, Program, and diagnostic metadata needed until continuations finish. VM code runs only on the owning Isolate thread; external threads post completion messages for that thread to apply to Futures.

The CLI waits for the root main Future and all live host event sources. A pending Future with no producer does not keep the process alive; if the root Future remains pending with no work that can advance it, the CLI reports async idle/deadlock. An embedding pump returns idle so the host may resume it later.

## Host Events and Timers

Async result semantics do not depend on a clock; I/O and timers belong to the host event layer. The standard library provides `sleep(milliseconds)`, `setTimeout` / `clearTimeout`, and `setInterval` / `clearInterval`. Timer callbacks run on the Isolate thread. A Future returned by a timer callback does not block later ticks, and its error follows the normal unhandled Future error rules.

Timers use a monotonic clock. Intervals do not replay missed ticks, avoiding an unbounded callback backlog; events with the same deadline run in creation order. After each host event, its microtasks are drained before the next event is processed.

## Performance and Implementation Boundaries

The language contract does not prescribe how the VM stores suspended execution state. It requires ordinary synchronous functions to retain their existing fast path, async calls without an await to avoid suspension scheduling, and suspended execution to retain only the live state needed to resume. Resuming one Future or task must not scan the whole Program, and the GC must precisely trace values live across suspension.

Async semantics flow through the AST, type checker, Typed IR, optimizer, register allocator, bytecode encoding and validation, VM, debugger, host API, and CLI. Await can suspend and throw, so it is observable; optimizers cannot remove it, move operations across it, or reorder it incorrectly. Direct calls, closure calls, and interface dispatch expose the same Future result.

Required regressions cover synchronous effects before the first await, completion without await, asynchronous continuation of an already-complete Future, FIFO notification, multiple waiters, Future adoption, Future composition success and failure, timeout, Completer, catch/finally across await, all call forms, async main, unhandled errors, GC during suspension, host-thread completion, timer ordering, and idle/deadlock boundaries.

Release benchmarks separately measure async calls without await, awaiting a completed Future, suspend/resume, concurrent Future count, GC pressure from suspended state, and timer scale. Report synchronous VM regression, retained memory per suspended execution, and first async allocation cost.

## Implementation References

- Dart async/await and Future API: [language guide](https://dart.dev/language/async), [Future API](https://api.dart.dev/dart-async/Future-class.html), [Future implementation](https://github.com/dart-lang/sdk/blob/main/sdk/lib/async/future_impl.dart).
- QuickJS-NG async frames, await, and Promise jobs: [async state](https://github.com/quickjs-ng/quickjs/blob/master/quickjs.c#L21163-L21224), [await opcode](https://github.com/quickjs-ng/quickjs/blob/master/quickjs.c#L20868-L20878), [job queue](https://github.com/quickjs-ng/quickjs/blob/master/quickjs.c#L2488-L2552).
- QuickJS-NG keeps OS timers in its host standard library: [timers](https://github.com/quickjs-ng/quickjs/blob/master/quickjs-libc.c#L2471-L2609), [event loop](https://github.com/quickjs-ng/quickjs/blob/master/quickjs-libc.c#L4852-L4876).
- Luau coroutines use explicit `lua_State` values, independent VM stacks, and manual resume/yield; they do not provide Future/Promise scheduling: [thread state](https://github.com/luau-lang/luau/blob/master/VM/src/lstate.h#L285-L305), [resume/yield](https://github.com/luau-lang/luau/blob/master/VM/src/ldo.cpp#L814-L869).
