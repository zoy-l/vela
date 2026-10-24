# Vela language roadmap

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
- [x] Classes, fixed fields, and aggregate construction
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
- [x] Add strict string interpolation and explicit primitive `toString()`
- [x] Add single-allocation `join`, string operations, and GC-owned `StringBuilder`
- [ ] Add Unicode code point iteration and grapheme cluster APIs
- [ ] Implement explicit numeric conversions (implicit conversions are forbidden)
- [x] Implement invariant typed arrays, nested arrays, and compact contiguous storage
- [ ] Implement tuples and fixed-size records
- [ ] Implement `List<T>`
- [ ] Implement `Map<K, V>`
- [x] Lower class methods to bytecode
- [x] Add `this` and `object.method()` calls
- [ ] Define field default values and constructor initialization
- [ ] Add static fields and static methods
- [ ] Add visibility rules for public/private members
- [ ] Add recursive function support and recursion diagnostics
- [ ] Complete function types and closures

## P1: type system and pattern matching

- [ ] Add enum payloads
- [x] Add enum `switch`
- [ ] Add exhaustive matching
- [ ] Add `match` expressions
- [ ] Add type aliases
- [x] Add nullable types such as `T?`
- [ ] Add generics such as `List<T>` and `Map<K, V>`
- [ ] Add interfaces or traits
- [ ] Add type tests and checked casts
- [x] Diagnose unreachable statements
- [x] Require complete returns on every finite control-flow path

## P2: modules and standard library

- [x] Add `import`
- [ ] Add `export`
- [ ] Add module-level symbol isolation
- [ ] Add import aliases for qualified access and name conflict resolution
- [x] Compile multiple source files as one program
- [ ] Define package and dependency metadata
- [x] Add console output
- [ ] Add console input
- [ ] Add time, random, and math libraries
- [x] Add basic string standard library APIs ([contracts](strings.md))
- [ ] Add collection standard library APIs
- [ ] Define process exit codes

## P2: errors and host integration

- [ ] Add `try`, `catch`, and `finally`
- [ ] Add `throw` and typed error values
- [ ] Preserve VM stack traces
- [ ] Define compiler and runtime error formatting
- [ ] Add typed native function bindings
- [ ] Define host handle rooting and lifetime APIs

## P2: performance and tooling

- [ ] Add typed VM register slots for primitive values
- [ ] Add a bytecode constant pool
- [x] Add constant folding
- [x] Add CFG liveness and physical register reuse with contiguous call arguments
- [x] Validate immutable owned programs once at compilation/loading
- [x] Reuse register storage sized to active functions by call depth
- [x] Add dead-code and unreachable-block elimination
- [x] Eliminate self-moves after register allocation
- [ ] Add jump threading and general redundant-move elimination
- [ ] Specialize direct calls and field offsets
- [ ] Benchmark computed-goto dispatch before adopting it
- [ ] Measure and tune generational GC only after representative benchmarks
- [ ] Add bytecode and compilation caches
- [ ] Add debug symbols and source-level stack traces
- [ ] Add formatter, LSP diagnostics, and syntax highlighting
- [ ] Add a debugger and REPL
- [ ] Add documentation generation

Current bytecode limits and measured execution costs are documented in
[VM execution contracts](vm_execution.md).

## Recommended implementation order

1. Complete comparisons, logical operators, `for`, `break`, and `continue`.
2. Implement strings and arrays with typed contiguous storage.
3. Finish class methods, `this`, and method calls.
4. Add console output and the first standard library surface.
5. Add imports and multi-file compilation.
6. Add constant folding and basic bytecode cleanup.
7. Add errors, stack traces, and native bindings.
8. Re-run Release benchmarks before changing VM dispatch or GC strategy.

Features such as inheritance, virtual dispatch, async tasks, and JIT remain
outside the basic language milestone until the static object model and typed VM
contracts are stable.
