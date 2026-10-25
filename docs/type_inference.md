# Type inference

Vela checks expressions in two directions: an expression synthesizes its type,
and an enclosing declaration or operation may supply an expected type. This is
compile-time inference; it does not introduce `any`, runtime type discovery, or
numeric conversions. Named functions, methods, and external declarations retain
explicit parameter and return annotations.

## Lambda parameters

An expected function type supplies the types of omitted lambda parameters by
position. This applies uniformly to annotated local/global variables, assignments,
returns, class fields and constructor arguments, function arguments, arrays of
callbacks, interfaces, and generic functions after explicit specialization.
Aliases and nullable function types carry the same context. Explicit annotations
are still allowed and must satisfy the expected signature.

```typescript
func apply(value: int, operation: (int) => int): int {
  return operation(value);
}

func makeAdder(): (int) => (int) => int {
  return left => right => left + right;
}

func main(): void {
  const add: (int, int) => int = (left, right) => left + right;
  print(apply(21, value => value * 2));

  const values = [3, 1, 2];
  values.sort((left, right) => left - right);
  values.forEach(value => { print(value); });
  print(values.reduce(0, (sum, value) => sum + value));
}
```

A single unannotated parameter may omit parentheses. Multiple parameters and
annotated parameters require parentheses. Parameter counts must match exactly.
Without a function type context, parameter annotations remain necessary:
`const twice = (value: int) => value * 2` is valid, while
`const twice = value => value * 2` reports that `value` needs an annotation or
function type context. The checker does not guess parameter types from operators
or later calls.

## Lambda results

An explicit return annotation takes precedence. Otherwise an expected function
type supplies the result type and checks every return against it. It also
provides context for returned callbacks and empty array literals.

Without a result context, the checker synthesizes the result from expression or
block returns. Equal types merge unchanged; `T` and null merge into `T?`, as do
`T` and `T?`. Different primitive types, including int versus double, do not
merge. Bare returns and value returns cannot mix. No returns means `void`.
A null-only result requires an explicit nullable return annotation. Non-void
blocks must return or throw on every path; inference does not bypass flow checks.
Nested lambda results are collected independently, and closure capture rules
remain unchanged.

## Array callback contexts

Array algorithms supply ordinary function signatures to the same checker; no
special lambda typing rule belongs to `sort` or `some`. `map` supplies its
input parameter type and infers the result array element type from the callback.
For `reduce`, `initial` first supplies the accumulator type, which can differ
from the element type. Direct lambda bodies are checked with that completed
signature even when named arguments list the callback before `initial`.
Runtime argument evaluation remains once in source order.

This follows TypeScript's contextual callback inference. TypeScript 6.0.3's
`lib.es5.d.ts` declares a separate accumulator type `U` for
`reduce<U>(callbackfn: (previousValue: U, currentValue: T, ...) => U, initialValue: U): U`.
Strict-mode checks infer number/number parameters for numeric accumulation and
string/number for string accumulation over the same `number[]`. Vela keeps its
existing `initial`-first argument order and exact callback arity; this change
adopts inference rather than the entire JavaScript array API.

## Typed IR and editor contracts

The semantic model owns each completed lambda signature. Lowering uses that
signature for parameter registers, return types, captures, and callable layouts.
Source annotations may be absent; backends must not infer or recover types from
AST annotation text. A regression test compares instruction streams and register
counts for equivalent explicit and inferred callbacks.

The language server uses the same checked signature for inferred parameter
hover, completion, and definition lookup. Formatting preserves omitted annotations
and supports both parenthesized and single-parameter forms.

## Validation and cost (2026-10-25)

These measurements predate the API naming migration and its replacement of
in-place `transform` by a separate result-producing `map`. Current contracts
and measurements are in [the standard library document](standard_library.md).

Debug and Release each passed 836 C++ tests and 57 tooling tests. Coverage includes
imported generic signatures, nested captures under GC pressure, nullable results,
different accumulator types, missing context, inconsistent returns, formatter
idempotence, inferred parameter hover/member completion, and definition lookup.
Equivalent explicit and inferred callbacks produce identical bytecode, register
counts, and parameter counts.

Windows x64 Clang Release measurements use three Google Benchmark repetitions
with a 0.2 s minimum. Values are median microseconds for each complete workload.
The array workload sorts six integers, reduces them 100 times, and transforms
the result; its source now uses inferred callbacks.

| Workload | Before | After |
| --- | ---: | ---: |
| Compile array algorithms | 137.180 | 145.615 |
| Execute array algorithms | 34.854 | 35.553 |
| Compile ordinary function calls | 17.147 | 16.847 |
| Execute ordinary function calls | 29.032 | 29.073 |

The observed array compilation increase is 6.1%; its execution difference is
2.0%. These sequential local samples do not isolate scheduling or CPU-frequency
effects. Runtime representation is unchanged: the array workload still uses
189 register bytes, has 6,656 managed bytes after first execution, and retains
zero managed bytes after full collection. Inference adds compiler work, not
runtime type discovery or callback conversions.

Raw reports: `.cache/contextual-lambda-before.json`,
`.cache/contextual-lambda-after.json`, `.cache/lambda-inference-debug-final.log`,
and `.cache/lambda-inference-release-final.log`.
