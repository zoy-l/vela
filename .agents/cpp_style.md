# Vela C++ Code Rules

These rules apply to Vela-owned C++ source code and cover language conventions, APIs, performance,
and file organization. See [Architecture and Contracts](architecture.md) for architecture and
failure boundaries, and [Development Workflow](development_workflow.md) for execution and validation.

## Basic conventions

- Follow the Google C++ Style Guide by default. The repository's `.clang-format`, `.clang-tidy`, and
  `vl lint` provide executable formatting and static-analysis rules.
- Use the project's fixed C++23 standard. Do not upgrade it independently.
- Use leading return types for ordinary function declarations and definitions, such as
  `bool IsLambda() const;`, and traditional return types in function-pointer aliases.
  Retain trailing return types only where C++ syntax requires them, such as explicit lambda
  results or deduction guides. Local variable type inference remains allowed.
- Use `#pragma once` in headers; do not use include guards.
- Use the repository's `.clang-format` for mechanical formatting, with a 100-column line limit.
- For C++ enums and initializer lists, clang-tidy requires trailing commas in multiline constructs;
  clang-format controls their layout. Single-line comma policy is `Ignore`: the pinned checker
  mistakes the separator after an empty designated initializer such as `.signature = {},` for a
  trailing comma and can delete it. Re-evaluate this setting when updating the pinned tools.
- Keep `readability-redundant-member-init` enabled. For aggregate result objects, explicitly
  initialize class-type fields at construction sites that would otherwise trigger the compiler's
  missing-designated-field warning; do not add redundant default member initializers to silence it.
- Use `snake_case` for file names, `PascalCase` for types, and CamelCase beginning with an uppercase
  letter for ordinary functions and methods.
- Name test source files `*_unittest.cc` and place them in the module under test. Use GoogleTest
  `TEST` / `TEST_F` and `ASSERT_*` / `EXPECT_*`. Do not write custom test entry points, boolean result
  aggregation, or assertion frameworks.
- Cognitive-complexity checks count project control flow and ignore macro expansions, including
  GoogleTest assertion implementation branches. Keep assertion calls at the tested boundary;
  do not split a scenario merely to reduce the framework's expanded branch count.
- Name benchmark source files `*_benchmark.cc`. Use Google Benchmark for registration, timing, and
  statistics; do not write a custom timing framework.
- Put the operating system immediately before the extension in platform-specific files, such as
  `*_win.cc` or `*_linux.cc`. Shared headers must not expose platform SDK types. Platform tests use
  the same ordering: `*_unittest_win.cc` and `*_unittest_linux.cc`.
- Use `snake_case` for simple accessors and `set_snake_case` for setters.
- Use `snake_case` for local variables and fields; suffix private fields with `_`.
- Use `kPascalCase` for constants and enum values, and `UPPER_SNAKE_CASE` for macros.
- Place Vela-owned types in the `vela` namespace. Internal implementations may use `vela::internal`.
- Prefer fixed-width integer types from `<cstdint>` for project-owned data and interfaces.
- Maintain strict type boundaries. Do not rely on implicit conversions to resolve type mismatches.
- Include Abseil headers with angle brackets, such as `#include <absl/container/flat_hash_map.h>`.
  Prefer Abseil flat hash containers for hash-based lookup; account for pointer and reference
  invalidation on rehash when choosing storage and lifetimes.

## APIs and expressions

- Project-owned action interfaces must not return a `bool` without clear semantics. Only pure
  predicates may return `bool`, and their names must begin with `Can`, `Is`, or `Has`.
- Do not use bare `bool` parameters as control-flow switches in action interfaces or functions with
  multiple parameters. Use a named `enum class` or an Options structure for modes.
- When a scope repeatedly evaluates a function call, handle lookup, type conversion, nontrivial
  computation, or container index whose result should remain stable, evaluate it once and store it
  in a meaningfully named local variable.
- Do not introduce meaningless `Manager`, `Service`, `Facade`, `Utils`, or `Helper` abstractions to
  hide simple logic.
- Public headers should expose only stable types and interfaces that callers need. Put complex
  implementation, allocation, I/O, and loops in `.cc` files.
- In Windows source files that use Win32 SDK leaf headers, retain the prerequisite `<windows.h>`
  include with `IWYU pragma: keep` when it initializes architecture macros and base types required
  by those headers. Also include each SDK symbol's declaring header directly. This exception is
  limited to the SDK prerequisite; it does not permit retaining other unused dependencies.
- Do not use Pimpl. Declare actual private state directly and keep platform state within its
  platform boundary.

## Performance and contiguous memory

- Prefer direct, optimizable access for contiguous memory and hot paths. Use `std::span` and
  `std::vector` at interface and lifetime boundaries.
- Do not introduce heavier wrappers, checked accessors, or extra function layers solely to remove
  clang-tidy diagnostics about raw pointers or indexing.
- Do not repeat defensive checks on every element access when an outer contract already guarantees
  array length, index bounds, and lifetime.
- In `benchmarks/dasprofile` reference ports, preserve upstream numerical coefficients and
  conventional mathematical symbols. Its local `.clang-tidy` permits those literals and names,
  contiguous buffer indexing, and pointer arithmetic. Bound traversals by prepared container sizes
  or fixed algorithm dimensions and check every workload result; do not add per-element checked
  access solely for lint. These exceptions do not apply to compiler/runtime code or input boundaries.
- Aim for O(1), O(log n), or O(n) core loops. Avoid unindexed nested linear scans and frequent
  linear searches.
- Validate refactoring that may affect hot paths with Release benchmarks, assembly, or
  vectorization reports. Do not judge performance solely by abstraction depth.

## Numbers, strings, and paths

- Derive memory layout with language mechanisms such as `sizeof` and `offsetof`; do not calculate
  byte offsets manually.
- Use named `kPascalCase` constants or enums in the owning module for architecture, protocol, and
  resource limits.
- Self-explanatory values in mathematical, geometric, and normalization formulas may remain inline.
  Explain empirical strategy values in an anonymous namespace or configuration structure.
- Use strong types or enums for internal object types, modes, and state machines instead of string
  comparisons.
- Preserve externally fixed native ABI and LLVM override signatures. `vl lint` excludes
  `bugprone-easily-swappable-parameters` in `runtime/native/native_operations.cc` and
  `compiler/jit/platform_support_win.cc` and `compiler/jit/code_resources.cc`: generated C entries
  and ORC resource-transfer callbacks
  retain their specified positional scalar arguments. Keep their parameter roles explicit; use
  strong types for new internal interfaces instead of extending this exception.
- Centralize protocol keys shared between modules as `inline constexpr std::string_view` constants.
  Do not scatter hard-coded copies.
- Use structured path interfaces such as `std::filesystem::path`; do not concatenate separators
  manually.

## Errors, ownership, and assertions

- Use explicit result enums or result objects for failures across module boundaries. Do not use
  exceptions for normal control flow. Vela currently disables C++ exceptions; project code must
  not use `try` / `catch`.
- Do not combine `bool` with output parameters to collapse distinct failure reasons.
  `std::optional` represents only legitimate absence.
- Define ownership, borrowing duration, destruction order, and threading constraints for every
  resource type.
- In `runtime/native/native_instance.cc`, use `std::unique_ptr<T[]>` for fallible fixed-array
  preparation before transferring ownership to the native program state. `vl lint` excludes the two
  `avoid-c-arrays` checks in that file because this owner preserves the existing `nothrow` failure
  boundary and the generated code's raw-pointer ABI. Other C-style array declarations remain
  prohibited; do not substitute a throwing allocation or an ABI-dependent container layout.
- External files, text, and user input must not cause intentional process termination. Return
  actionable diagnostics or failure results at the actual boundary.
- Assert runtime invariants once at their actual boundary. Do not repeatedly check state guaranteed
  by contract in every leaf function.
- Do not use runtime assertions inside loops. Do not add calculations, traversals, lookups, or
  temporary results solely to support an assertion. Rely on established contracts and required
  data types; handle real input failures at their boundary. Do not move assertions into helpers
  merely to bypass this rule.
- Use `static_assert` for compile-time type, constant, and layout constraints.

## Files, comments, and cleanup

- Keep each file focused on one core type, pipeline stage, or cohesive contract. Reassess
  responsibilities when a file exceeds 1,000 lines; do not split it mechanically.
- Split files along independent data contracts, lifetimes, platform branches, or test scenarios.
  Do not introduce `*_part1.cc` files, hollow wrappers, or pass-through layers.
- Write explanatory code comments in Chinese, preserving technical terms, API names, and protocol
  names. Explain design reasons, invariants, ownership, synchronization, and units.
- Do not use decorative section comments, mechanical Doxygen templates, or comments that conceal
  unclear naming or responsibilities.
- After changes, remove stale comments, obsolete fields and enums, duplicate helpers, temporary
  output, caches, and implementations with no callers.
