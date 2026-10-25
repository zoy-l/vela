# Persistent REPL implementation notes

The REPL contract is implemented by `compiler/repl_session` and the interactive
console. See [usage and limits](repl.md). This document records the identity and
transaction design used by the completed [P2 acceptance](p2_acceptance.md).

## Semantics

`vela repl` keeps one isolate and a current validated program. Declarations,
global values, class instances, function values, closures and imports persist
between submissions. Only the current submission executes. Existing source is
rechecked/recompiled, never replayed for side effects. Runtime reference values
remain precisely rooted while reading the next input.

The language keeps its normal strict static types, declaration syntax and duplicate
declaration errors. Expressions without a trailing semicolon display their result;
void expressions do not display a value. A trailing semicolon suppresses display,
following Julia's interactive convention. Multiline blocks and incomplete parses
use a continuation prompt. `.help`, `.source`, `.reset` and `.quit` are shell
commands, not language keywords. New global declarations initialize once when
admitted to the session; failed initialization retains the language's recoverable
lazy-initializer behavior. Runtime side effects are not transactional.

Imports pin successfully admitted source snapshots until reset, as normal
module caches do. Failed compilation does not commit a submission, stable IDs or
newly pinned inputs. Runtime failures occur after successful compilation and retain
the admitted declarations and earlier side effects.

## Stable identities

Recompiling history and copying globals alone is incorrect: existing closure
layouts contain function/selector IDs and existing objects carry layout pointers.
The implementation must preserve all of these identities:

1. A copyable frontend module cache retains immutable source snapshots,
   canonical source resolutions and persistent module binding prefixes. Ordinary
   compilation uses a fresh instance of the same loader contract.
2. Session-only persistent IDs assign functions, global slots, interfaces and
   interface method selectors independently of current traversal order. Ordinary
   compilation retains compact per-program numbering. Lowering writes functions
   into their assigned slots rather than assuming push order equals function ID.
3. Anonymous functions receive identities based on lexical owner and source point
   after generic specialization. The specialized owner differentiates identical
   source positions in different generic instantiations. Lambda targets use an
   explicit index map, replacing the current `lambda_base + index` assumption.
4. Runtime class, array and collection layouts are reused by stable type identity
   within the session. Different independent programs keep distinct identities.
   Persistent compilation materializes all declared interface method selectors
   before layouts escape, because later prompts may introduce the first call
   through an already implemented interface.
5. New validated global storage copies old values/states by stable slot while the
   prior program remains rooted. New slots are rooted before old storage is
   released. The session does not expose historical FunctionHandles to callers.

Compiler state is staged in a working copy and committed only after successful
encoding and program validation. Isolate layout allocation needs a real transaction
boundary so failed compilation/loading can release newly created metadata without
invalidating existing objects or layouts. Only metadata created before any instance
can escape belongs to this transaction.

## Synthetic entries and source provenance

A submission is parsed through the real parser. Statement/expression submissions
use a synthetic function. Prepared AST module loading must bind that parsed root
alongside ordinary imported modules; generated identifiers are renamed to internal
`$` names before binding, so they cannot collide with user identifiers. Compiler
diagnostics and runtime traces map generated root lines back to submission sources.

An internal statement kind can echo a checked expression: lowering evaluates it
once, prints non-void results with the existing output rules, and discards void.
It does not add implicit conversion or a user-visible language keyword.

Old synthetic entry functions remain available to semantic/capture analysis but
are retired from parent-body lowering. Their nested lambdas remain compiled with
stable IDs. A single reserved entry function ID can be reused because synthetic
entries cannot be named or captured by user code and the session exposes no entry
handles. This avoids consuming a function slot for every ordinary expression.
Prior wrapper bodies execute neither during compilation nor later submissions.

## Required verification

- Values and initialization side effects persist without historical output replay.
- Closure targets and mutable captures survive inserted functions/globals, imports,
  generic instantiations and repeated collection.
- Existing instances retain class/cast/interface and nested array/Map/Set identity.
- First interface calls introduced in later submissions work on earlier instances.
- Hundreds of non-defining submissions reuse the synthetic entry slot.
- Compile errors roll back source/IDs/layout metadata; runtime errors preserve prior
  state and reset in-progress initializers.
- Module snapshots, multiline parsing, EOF/reset/quit and diagnostics are exercised
  through the actual CLI. Measure growing-session compile cost and retained memory.

References: [Node REPL](https://nodejs.org/api/repl.html),
[Julia REPL](https://docs.julialang.org/en/v1/stdlib/REPL/), and
[LLVM ORC execution sessions and symbol tables](https://llvm.org/docs/ORCv2.html).
These inform persistence and identity contracts; no JIT dependency is introduced.
