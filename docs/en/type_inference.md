# Type inference

[**English**](type_inference.md) | [简体中文](../zh-CN/type_inference.md)


Vela checks expressions in two directions: an expression synthesizes its type,
and an enclosing declaration or operation may supply an expected type. This is
compile-time inference; it does not introduce `any`, runtime type discovery, or
numeric conversions. Named functions, methods, and external declarations retain
explicit parameter and return annotations.

## Generic calls and shared constraints

Generic functions, class construction, and transparent generic constructor aliases
may omit all type arguments. Arguments, expected result types, and callback
signatures contribute structural constraints before specialization. Explicit type
arguments remain valid. Type annotations such as `Box<int>` and `Map<string, int>`
must remain complete; later statements do not determine an earlier variable's type.

```typescript
func identity<T>(value: T): T { return value; }
func apply<T, U>(operation: (T) => U, value: T): U { return operation(value); }
class Box<T> { value: T; }

const id = identity(42);
const box = Box(value: id);
const operation: (int) => int = identity;
const answer = apply(value => value + 2, 40);
const scores: Map<string, int> = Map();
```

Ordinary arguments supply evidence before dependent lambdas, generic function
values, and empty constructions are checked. Nested calls also wait when an
output type variable requires context from later arguments. A callback may precede
the argument that supplies its parameter types. Initializers, assignments, fields, constructor
and function arguments, returns, conditionals, `??`, and `await` pass expected
types through the same expression checker. An expected type is an inference hint;
the final assignment relation still has to hold.

Array `map/reduce`, Set/Map/Completer construction, Future factories, and user
generics share the constraint solver. Future adoption and the array result of
`wait` remain intrinsic API semantics. Ordinary callbacks require exact arity,
array callbacks may omit trailing parameters, and exception callbacks filter by
their declared throwable type. These contracts do not create separate inference rules.

Inference merges equal types and existing nullable relationships. It does not
merge int/double, invent union types or common application supertypes, or change
the invariant arguments of existing containers. Array literals still construct
storage under their array context. Insufficient evidence, conflicting constraints,
and incompatible final arguments are compile errors; failure never defaults to
dynamic. Thus `const values = Map()` and context-free `const operation = identity`
remain invalid. Inference can include dynamic only when a declaration, argument,
or operation actually has that type.

Each generic call owns its inference variables. Solved canonical type arguments
request specialization; explicit and inferred uses share the same concrete
declaration. Inference variables never reach backends. Deferred checks retain
flow facts from their source position and apply possible assignment/call effects
at that position. Runtime evaluation remains exactly once, from left to right.

## Lambda parameters

An expected function type supplies the types of omitted lambda parameters by
position. This applies uniformly to annotated local/global variables, assignments,
returns, class fields and constructor arguments, function arguments, arrays of
callbacks, interfaces, and generic calls after constraint solving.
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
  print(values.reduce((sum, value) => sum + value, 0));
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
For `reduce`, `initialValue` supplies the accumulator type, which can differ
from the element type. Direct lambda bodies are checked with that completed
signature even though the callback precedes `initialValue`. The initial value is
required; omitting it is a compile-time error.
Runtime argument evaluation remains once in source order.

This follows TypeScript's contextual callback inference. TypeScript 6.0.3's
`lib.es5.d.ts` declares a separate accumulator type `U` for
`reduce<U>(callbackfn: (previousValue: U, currentValue: T, ...) => U, initialValue: U): U`.
Strict-mode checks infer number/number parameters for numeric accumulation and
string/number for string accumulation over the same `number[]`. Vela uses
callback-first argument order. Array callbacks may omit unused trailing parameters;
the compiler checks their declared prefix and passes exactly that many arguments.
Ordinary function calls retain their exact arity contract. The remaining intentional
JavaScript differences are documented in [growable arrays](arrays.md).

## Typed IR and editor contracts

The semantic model owns each completed lambda signature. Lowering uses that
signature for parameter registers, return types, captures, and callable layouts.
Source annotations may be absent; backends must not infer or recover types from
AST annotation text. A regression test compares instruction streams and register
counts for equivalent explicit and inferred callbacks.

The language server uses the same checked signature for inferred parameter
hover, completion, and definition lookup. Incomplete expressions retain registered
signatures and established local types; diagnostics do not discard known semantic facts.
Formatting preserves omitted annotations
and supports both parenthesized and single-parameter forms.

## Implementation contracts

Equivalent explicit and inferred callbacks produce identical bytecode, register counts, and parameter counts. Inference is performed strictly at compile time; it introduces no runtime dynamic type discovery, boxing, or callback conversion overhead.
