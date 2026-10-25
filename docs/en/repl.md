# Persistent REPL

[**English**](repl.md) | [简体中文](../zh-CN/repl.md)


Vela provides an interactive read-eval-print loop via `vela repl`. The interactive
session maintains a live isolate and validated program state where declarations,
global values, closures, class instances, and module imports persist across
submissions without re-executing historical code for side effects.

```text
> let value = 40;
> func add(value: int): int {
...   return value + 2;
... }
> add(value)
42
> value = value + 1;
> value
41
```

## Interactive Semantics

- **Strict Types**: The REPL enforces the same static types, syntax, and duplicate
  declaration errors as ahead-of-time compiled source files.
- **Incremental Submission**: Each submission parses either declarations or
  statements/expressions. Only the newly submitted statements and expressions
  execute; previously accepted code is not replayed.
- **Expression Echoing**: Top-level expressions without a trailing semicolon
  evaluate and print their non-void result once. A trailing semicolon suppresses
  printing, matching Julia's interactive convention. Void expressions never print.
- **Multiline Input**: Unclosed braces, parentheses, and brackets prompt for
  continuation lines (`... `). An unexpected EOF with unclosed constructs reports
  a syntax diagnostic.
- **Shell Commands**: `.help` lists shell commands, `.source` prints accepted
  input history, `.reset` clears the isolate and session state, and `.quit` exits.
  Commands are shell controls, not language keywords.
- **Error Handling**: Compile errors discard the submission without modifying
  the session or incrementing IDs. Runtime errors preserve admitted declarations
  and previous side effects; in-progress lazy initializers reset to uninitialized
  so they can be retried.
- **Provenance**: Compiler diagnostics and stack frames identify submissions
  using `<repl:N>` with line numbers relative to the original submission.

## Stable Identity and Runtime Continuity

Recompiling history and copying raw globals alone is insufficient: existing
closure instances store function and selector IDs, and existing heap objects
retain class layout pointers. The session preserves structural continuity across
submissions:

1. **Module Caching**: Successfully imported module snapshots, canonical path
   resolutions, and module prefixes are pinned until `.reset`. Editing an imported
   file takes effect in a fresh or reset session.
2. **Persistent IDs**: The compiler assigns function, global slot, interface,
   and selector IDs independently of AST traversal order. Function bodies are
   written to their assigned slots rather than relying on sequential push order.
3. **Lambda Identifiers**: Anonymous functions receive stable identities derived
   from lexical owner and source coordinates after generic specialization.
   Lambda targets use an explicit index map rather than assuming contiguous layout.
4. **Layout Preservation**: Runtime class, array, and collection layouts are
   reused by stable type identity within the session. Newly introduced interface
   methods materialize selectors before layouts escape, ensuring earlier instances
   can dispatch calls through interfaces implemented later.
5. **Transactional Staging**: Compiler state is staged in a working copy and
   committed only after successful bytecode verification. In the VM, new global
   slots are allocated and rooted before previous storage is released. If
   compilation or loading fails, newly allocated metadata is rolled back before
   escaping to the heap.

## Synthetic Entries and Source Provenance

- **Wrapper Functions**: Statement and expression submissions are lowered into
  a synthetic wrapper function. Prepared AST loading binds this root alongside
  imported modules. Generated identifiers use internal `$` prefixes to eliminate
  name collisions with user code.
- **Slot Reuse**: Old synthetic entry wrappers are retired from parent-body
  lowering once executed, while their nested lambdas retain stable compiled IDs.
  A single reserved entry function slot is reused across non-defining submissions,
  preventing unbounded function slot growth.
- **Echo Lowering**: Unsemicoloned expressions are lowered to evaluate once,
  print the result using standard runtime formatting, and release temporary values.

## Verification Contracts

The REPL implementation satisfies these verification requirements:
- Values, instance mutations, and initialization side effects persist without
  historical output replay.
- Closures and mutable capture cells remain valid across newly added functions,
  globals, and garbage collection cycles.
- Existing heap instances retain class identity, type casts, interface conformance,
  and collection memberships across submissions.
- Compiler errors roll back staged metadata, whereas runtime errors preserve
  prior session state and permit retrying failed operations.
