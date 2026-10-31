# Persistent JIT REPL

[**English**](repl.md) | [简体中文](../zh-CN/repl.md)

`vela repl` uses `DevelopmentSession` and `DevelopmentRepl` to compile and execute
native submissions in one live Isolate. Declarations, globals, closures, objects
and imports persist without replaying previously executed statements.

```text
> let value = 40;
> func add(value: int): int { return value + 2; }
> add(value)
42
> value = value + 1;
> value
41
```

## Interactive behavior

Each submission contains declarations or statements/expressions and follows the
ordinary static type and duplicate-declaration rules. Only new statements execute.
An expression without a trailing semicolon prints its non-void result once;
a semicolon suppresses printing. Unclosed delimiters prompt for continuation
lines. EOF with unfinished syntax reports a source error.

`.help` lists commands, `.source` prints accepted source, `.reset` rebuilds the
script state, and `.quit` exits. These are console controls, not language keywords.
Compiler errors reject the candidate. Once publication succeeds, a runtime error
preserves admitted declarations and prior side effects; failed lazy initializers
can be retried. Diagnostics remap interactive source to `<repl:N>`.

## Publication and identity

The frontend builds an immutable source candidate with stable declaration and
specialization identities. Shared native generation compiles its functions, then
the development session prepares runtime state and publishes the update at the
existing safe boundary. A compilation or preparation failure leaves the running
version intact.

Existing closures and objects retain the code and metadata they need. Global
slots and type identities remain stable across accepted submissions. Synthetic
statement entries are retired from later source lowering; captured nested functions
remain reachable. Repeated non-defining submissions do not grow the persistent
function table without bound.

Imported source is part of the accepted snapshot. The standalone console does not
watch files; reimport after a reset uses a fresh interactive session. In an embedded
development session, the source owner supplies file changes through explicit Load,
Hot Reload or Reload operations. Submitting interactive code does not implicitly
turn a disk edit into an accepted reload.

## Reset and ownership

Construct `DevelopmentRepl(session, path)` after loading the host
`DevelopmentSession`. The REPL borrows that session. Host bindings are fixed by
the session's binding snapshot.

`DevelopmentRepl::Reset(SourceUpdate)` removes the interactive module and uses
the session's Reload transaction. The source owner supplies the update; an
embedded caller can include refreshed project files. External roots, native
instances/entries/candidates, Futures or active host access may block replacement.
A rejected reset reports those resources and preserves the current session.

A successful reset closes old completion endpoints, retires old script state and
starts a new Isolate with the binding snapshot. Hosts must release their own
borrows before destroying a session. Pending work, retained callbacks and old
native code follow the [hot reload lifetime contract](../zh-CN/hot_reload.md).

Tests cover persistent globals and mutable captures, generic methods and
interfaces, import snapshots, errors, reset blockers, GC and code retirement.
