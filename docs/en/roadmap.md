# Vela language roadmap

[**English**](roadmap.md) | [简体中文](../zh-CN/roadmap.md)


This is the implementation checklist for the basic Vela programming language.
The checklist describes production work remaining after the current bootstrap
compiler, typed IR, register VM, and isolate-local GC foundation.

## Current bootstrap

- [x] C/C++-style blocks and semicolons
- [x] `int` and `bool` values
- [x] Local declarations and assignment
- [x] `func`, postfix types, and local type inference
- [x] Integer arithmetic and equality comparison
- [x] Logical negation
- [x] `if` and `while`
- [x] Functions and direct function calls
- [x] Classes, fixed fields, and generated/explicit construction
- [x] Enums with explicitly qualified members (`Enum.member`)
- [x] Static type checking
- [x] Typed IR and fixed-width register bytecode
- [x] Isolate-local precise incremental mark-sweep GC

## P0: close the basic language loop

- [x] Add `<`, `<=`, `>`, and `>=`
- [x] Add `&&` and `||`
- [x] Add `for`
- [x] Add `break` and `continue`
- [x] Add `switch` and `case`
- [x] Add a `null` literal and nullability checks
- [x] Add `let`/`const` declarations and constant binding assignment checks
- [x] Define scope, shadowing, and declaration-before-use rules
- [x] Complete source locations and diagnostics for all parser and checker errors

P0 behavior is specified in [Basic language contracts](language_basics.md).

## P1: core values and object model

- [x] Implement IEEE 754 binary64 `double` values, operations, arrays, and output
- [x] Implement `string`, comparison, and concatenation
- [x] Collect strings with precise roots, field/array barriers, and accounted contiguous storage
- [x] Decode string escapes and Unicode escapes with invalid-input diagnostics
- [x] Define UTF-8 storage and code point length/index/slice units (no separate `char` type)
- [x] Add string-context primitive/null formatting and explicit primitive `toString()`
- [x] Add single-allocation array `.join(separator)`, string operations, and GC-owned `String`
- [x] Add Unicode code point iteration and grapheme cluster APIs
- [x] Implement explicit numeric conversions (implicit conversions are forbidden)
- [x] Implement invariant typed arrays, nested arrays, and compact contiguous storage
- [x] Add growable `T[]`, geometric capacity growth, reserve and insertion/removal APIs
- [x] Implement `Set<T>`
- [x] Implement `Map<K, V>`
- [x] Lower class methods to bytecode
- [x] Add `this` and `object.method()` calls
- [x] Define field default values and constructor initialization
- [x] Add static fields and static methods
- [x] Add visibility rules for public/private members
- [x] Add recursive function support and recursion diagnostics
- [x] Complete function types and closures

## P1: type system and pattern matching

- [x] Add enum `switch`
- [x] Add exhaustive matching
- [x] Add `match` expressions
- [x] Add type aliases
- [x] Add nullable types such as `T?`
- [x] Add generics such as `Set<T>` and `Map<K, V>`
- [x] Add explicit interface / implements contracts
- [x] Add type tests and checked casts
- [x] Add checked dynamic values, heterogeneous containers, and dynamic dispatch ([design](json.md))
- [x] Diagnose unreachable statements
- [x] Require complete returns on every finite control-flow path

P1 contracts are specified in [Core language specification](core_language.md).

## P2: modules and standard library

- [x] Add `import`
- [x] Add `export`
- [x] Add module-level symbol isolation
- [x] Add import aliases for qualified access and name conflict resolution
- [x] Compile multiple source files as one program
- [x] Add console output
- [ ] Console input is outside the current standard API; see [standard library scope](standard_library.md).
- [x] Add time, random, and math libraries
- [x] Add lightweight UTC calendar conversion and strict ISO date-time input/output ([contract](time.md))
- [x] Add basic string standard library APIs ([contracts](strings.md))
- [x] Add checked string numeric parsing with int/double parse and tryParse ([contract](numeric_parse.md))
- [x] Add Unicode string case conversion ([contract](string_case.md))
- [x] Add PCRE2-backed regular expressions with Dart-style APIs and typed matches ([contract](regexp.md))
- [x] Add collection standard library APIs
- [x] Add JSON parsing and serialization over native values ([design](json.md))
- [x] Add fixed byte storage and typed byte views ([contract](byte_data.md))
- [x] Define process exit codes

## P2: errors and host integration

- [x] Add `try`, `catch`, and `finally`
- [x] Add `throw` and typed error values
- [x] Preserve VM stack traces
- [x] Define compiler and runtime error formatting
- [x] Stabilize machine-readable runtime error codes independently of diagnostic wording
- [x] Add typed native function bindings
- [x] Define host handle rooting and lifetime APIs

## P2: performance and tooling

- [x] Add typed VM register slots for primitive values
- [x] Add a bytecode constant pool
- [x] Add constant folding
- [x] Add CFG liveness and physical register reuse with contiguous call arguments
- [x] Validate immutable owned programs once at compilation/loading
- [x] Reuse register storage sized to active functions by call depth
- [x] Add dead-code and unreachable-block elimination
- [x] Eliminate self-moves after register allocation
- [x] Add jump threading and general redundant-move elimination
- [x] Specialize direct calls and field offsets
- [x] Evaluate direct-threaded/computed-goto dispatch and retain optimized switch dispatch
- [x] Measure GC workloads and incremental pause budgets ([garbage collection](garbage_collection.md))
- [x] Add bytecode and compilation caches ([contracts](compilation_cache.md))

- [x] Add debug symbols and source-level stack traces
- [x] Add formatter, LSP diagnostics, and syntax highlighting ([tools](source_tooling.md))
- [x] Add a debugger and REPL ([debugger](debugger.md), [REPL](repl.md))
- [x] Add documentation generation

Current bytecode limits and measured execution costs are documented in
[VM execution contracts](vm_execution.md).
Async APIs, scheduling, host ownership, and performance boundaries are specified in
[Async Functions and Future Semantics](async_tasks.md).

## Recommended implementation order

1. Complete comparisons, logical operators, `for`, `break`, and `continue`.
2. Implement strings and arrays with typed contiguous storage.
3. Finish class methods, `this`, and method calls.
4. Add console output and the first standard library surface.
5. Add imports and multi-file compilation.
6. Add constant folding and basic bytecode cleanup.
7. Add errors, stack traces, and native bindings.
8. Re-run Release benchmarks before changing VM dispatch or GC strategy.

Class inheritance, method overriding, and async tasks now run through the source,
type checker, Typed IR, bytecode, VM, and CLI paths. JIT and stateful hot reload
are separate follow-on capabilities. Their H1–H5 implementation order, Reload,
and acceptance contracts are specified in the
[JIT and hot reload design (Chinese)](../zh-CN/hot_reload.md#12-实施顺序与完成条件).

## Later: native AOT

The [AOT design](aot.md#implementation-stages-and-completion-criteria) records the
current implementation status and defines the complete A1–A5 sequence and
acceptance criteria: shared compilation boundaries, synchronous native execution,
async and dynamic paths, user builds
and distribution, and release acceptance. Emitting the first object file is not
completion. Every language path and the distribution workflow without a separate
user installation of Clang/LLVM must be connected.
