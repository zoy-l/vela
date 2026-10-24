# Vela language roadmap

This is the implementation checklist for the basic Vela programming language.
The checklist describes production work remaining after the current bootstrap
compiler, typed IR, register VM, and isolate-local GC foundation.

## Current bootstrap

- [x] C/C++-style blocks and semicolons
- [x] `int` and `bool` values
- [x] Local declarations and assignment
- [x] Integer arithmetic and equality comparison
- [x] Logical negation
- [x] `if` and `while`
- [x] Functions and direct function calls
- [x] Classes, fixed fields, and aggregate construction
- [x] Enums and `.member` shorthand
- [x] Static type checking
- [x] Typed IR and fixed-width register bytecode
- [x] Isolate-local precise incremental mark-sweep GC

## P0: close the basic language loop

- [x] Add `<`, `<=`, `>`, and `>=`
- [x] Add `&&` and `||`
- [x] Add `for`
- [x] Add `break` and `continue`
- [ ] Add `switch` and `case`
- [ ] Add a `null` literal and nullability checks
- [ ] Add `const`/`final` declarations and immutable assignment checks
- [ ] Define scope, shadowing, and declaration-before-use rules
- [ ] Complete source locations and diagnostics for all parser and checker errors

## P1: core values and object model

- [ ] Implement `float`
- [x] Implement `string`, escapes, comparison, and concatenation
- [ ] Implement `char`
- [ ] Implement numeric conversions
- [x] Implement arrays with contiguous storage
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
- [ ] Add enum `switch` and exhaustive matching
- [ ] Add `match` expressions
- [ ] Add type aliases
- [ ] Add nullable types such as `T?`
- [ ] Add generics such as `List<T>` and `Map<K, V>`
- [ ] Add interfaces or traits
- [ ] Add type tests and checked casts
- [ ] Diagnose unreachable code
- [ ] Require complete returns on every control-flow path

## P2: modules and standard library

- [x] Add `import`
- [ ] Add `export`
- [ ] Add namespaces
- [x] Compile multiple source files as one program
- [ ] Define package and dependency metadata
- [x] Add console output
- [ ] Add console input
- [ ] Add file I/O
- [ ] Add time, random, and math libraries
- [ ] Add collection and string standard library APIs
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
- [ ] Add dead-code and unreachable-block elimination
- [ ] Add jump threading and redundant-move elimination
- [ ] Specialize direct calls and field offsets
- [ ] Benchmark computed-goto dispatch before adopting it
- [ ] Measure and tune generational GC only after representative benchmarks
- [ ] Add bytecode and compilation caches
- [ ] Add debug symbols and source-level stack traces
- [ ] Add formatter, LSP diagnostics, and syntax highlighting
- [ ] Add a debugger and REPL
- [ ] Add documentation generation

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
