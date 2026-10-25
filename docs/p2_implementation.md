# P2 implementation and acceptance

This is the execution record for the complete P2 goal. The P2 sections of both
roadmaps define the scope. Partial milestones do not complete this goal.
Existing P1 source changes and measurements are preserved.

## Decisions and research

Language changes preserve strict types, explicit numeric conversions, `func`,
postfix annotations, `param`, qualified enum members, and UTF-8. No language
file I/O, C++ namespace blocks, inheritance, async, or JIT is added by this goal.

Modules use explicit `export` visibility, following the
[TypeScript module scope and export rules](https://www.typescriptlang.org/docs/handbook/2/modules.html).
Import prefixes follow [Dart libraries](https://dart.dev/language/libraries):
`import 'math' as math;`. An unprefixed import makes only the dependency's
exports available. Imports are not re-exported automatically. Every source file
has its own symbol table, even if it contains no import/export directives.
Module names are resolved before generic specialization; runtime calls and
field access do not perform module-name dictionary lookups.

Function-only import cycles can be linked statically. Each file is loaded once;
cyclic lazy static initialization still fails at runtime. A dependency's own
imports and private declarations never leak into its importer. Local lexical
bindings and generic parameters retain their shadowing rules. Conflicting
top-level imports require an explicit prefix. There is no alternate flat-global
loading mode.

For further implementation decisions, inspect the current producers/consumers
and mature implementations before choosing semantics. Record source links or
local reference paths here. The local Luau checkout is a reference, not a build
dependency. Benchmark computed-goto dispatch and GC alternatives before deciding
whether to adopt them; a measured decision to retain the current strategy must
state the evidence and limits.

## Implementation sequence

1. Modules: export, symbol isolation, import prefixes, in-memory module inputs,
   source identity, migrated imports and examples.
2. Errors and embedding: typed throw/catch/finally, exact unwinding and roots,
   source-aware stack traces, consistent diagnostics, typed native bindings,
   host root handles and documented isolate lifetimes.
3. Library and CLI: console input, time/random/math, collection operations,
   deterministic seeded random tests, explicit process exit status contracts.
4. VM/compiler: complete constant pools, typed primitive register storage,
   direct-call/field specialization, jump threading and move cleanup, validated
   versioned bytecode and compilation caches.
5. Tooling: source mappings, formatter, LSP diagnostics, syntax highlighting,
   usable debugger and REPL, generated API documentation. Tooling consumes the
   same parser/module/checker contracts as compilation.
6. Performance: representative Release measurements, dispatch and generational
   GC experiments/decisions, allocation/root correctness and old-load comparison.

## Acceptance ledger

- [x] Inspect workspace, project rules, complete P2 scope, and actual interfaces.
- [x] Capture a P2 Release baseline (`.cache/p2-before.json`).
- [x] Complete export, module scopes, prefix imports and module-aware checking.
- [x] Complete console input and process exit codes (actual piped CLI execution verified).
- [x] Complete time/random/math and collection standard libraries
  ([contracts](standard_library.md)); seeded random reference vectors verified.
- [x] Complete typed errors, throw, catch and finally through all execution paths
  ([contracts](exceptions.md)); Debug integration regression: 600 tests.
- [x] Preserve stack traces and unify source/runtime diagnostics.
- [x] Complete typed native bindings and rooted host handles ([contracts](embedding.md)).
- [x] Complete primitive register representation and constant pools
  ([contracts and intermediate measurements](vm_storage.md)).
- [x] Complete jump threading, redundant moves and direct/field specialization.
- [x] Measure computed-goto and GC strategies and record adoption decisions
  ([dispatch](dispatch_measurements.md), [GC](gc_strategy.md)). Retain switch and
  incremental non-generational collection; remove losing production experiments.
- [x] Complete validated bytecode and compilation caches ([contracts](compilation_cache.md));
  26 compiler integration tests, malformed-image VM tests and atomic-file tests passed.
- [x] Complete debug symbols and source mappings.
- [x] Complete formatter, LSP diagnostics and syntax highlighting ([tools](source_tooling.md));
  formatter CLI idempotence checked on 65 source files, 27 process/tool tests and
  three VS Code client tests passed. Extension packaged without npm dependencies.
- [x] Complete debugger and persistent REPL.
  Source-stepping debugger, read-only VM observer, breakpoints and register/object
  inspection are implemented ([contracts](debugger.md)); persistent REPL preserves
  globals, closures, layouts, module snapshots and failed-compilation transactions
  ([contracts](repl.md)). Twelve REPL tests and three actual CLI cases cover
  transactional module snapshots, GC pressure, persistent identities and recovery.
- [x] Complete documentation generation; exported API example actually generated.
- [x] Run final Debug/Release regression and zero-warning lint without suppression:
  745 C++ and 27 process/tool tests per variant; all lint findings resolved.
- [x] Actually execute examples and tooling integration scenarios:
  34 examples checked, 31 entry points run, three VS Code client tests passed.
- [x] Run final Release benchmark, memory and baseline comparisons:
  109 cases and a 30-case final VM rerun passed; GC, REPL and process memory measured.
- [x] Synchronize both roadmaps, contracts and performance report; check links/LF.

All entries are implemented and validated. [Final acceptance](p2_acceptance.md)
records the full comparison, regressions, optimization attempts and practical limits.
