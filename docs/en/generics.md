# Generic bounds and inference

[**English**](generics.md) | [简体中文](../zh-CN/generics.md)

Vela supports generic declarations, one upper bound per parameter, and inference
driven by arguments, context, and bounds. Concrete applications are monomorphized;
primitive values retain their compact storage.

```vela
interface Comparable<T> { compareTo(other: T): int; }
class Score implements Comparable<Score> {
  value: int;
  compareTo(other: Score): int { return this.value - other.value; }
}
func compare<T extends Comparable<T>>(left: T, right: T): int {
  return left.compareTo(right);
}
func main(): int { return compare(Score(value: 42), Score(value: 40)); }
```

The parameter T keeps its identity. Its bound grants capabilities and constrains
applications; it does not replace T with `Comparable<T>`. The complete runnable
example is [examples/generics.vela](../../examples/generics.vela).

## Declarations and bounds

Functions, classes, interfaces, transparent aliases, class instance/static
methods, and interface methods can declare `<T, U extends T>` parameters.
Constructors use their class parameters and cannot declare another parameter list.
All names in a list are registered before bounds are resolved, so a bound can
reference a later parameter. Duplicate names in one list are errors. A method
parameter can shadow an owner parameter; identity includes the declaration and
parameter index.

Type annotations require complete applications such as `Box<int>`. Calls and
ordinary function contexts may omit the entire argument list. Explicit lists
must be complete; there are no raw types, partial lists, or default arguments.

A bound may use existing primitive, enum, class, interface, array, function, or
parameterized value types, their nullable forms, and parameters in scope.
Qualified names and transparent aliases use normal module/type resolution.
`void`, the null-literal type, and dynamic are invalid explicit bounds. Omitting
a bound leaves a distinct abstract parameter; it does not make it dynamic.

An unbounded parameter may receive nullable types or explicitly supplied dynamic.
Existing return-only uses of void remain valid, while fields, parameters, array
elements, and bounded value positions still reject void. Non-null bounds reject
nullable arguments. Nullable bounds allow appropriate non-null and nullable
arguments, but member access still requires the usual non-null proof. Applying
`T?` to an already nullable argument does not add another nullable wrapper.

Recursive bounds such as `T extends Comparable<T>` are valid. Parameter dependencies
may also be mutually recursive through real type constructors. Forwarding cycles
without a constructor, such as `T extends T` or `T extends U, U extends T`, are
errors, including cycles hidden by transparent aliases. Real type nesting and
generic expansion are each limited to 128 levels.

## Static checking

Every generic definition in every loaded module is checked before instantiation,
including unused functions, classes, interfaces, and aliases. Bounds determine
available members and static relations. Locals, fields, closures, returns, async,
and flow analysis use the normal checker. A concrete application cannot supply
capabilities missing from the declaration.

Unbounded T can be passed, captured, stored in legal parameterized types, and
returned. Operations need a suitable bound or an ordinary function argument.
An upper bound does not grant construction capability: use `() => T` rather
than `T()` when a factory is needed. A bound value cannot be returned as an
arbitrary caller-selected T.

Static proof uses type identity, nullability, class inheritance, and explicit
interface implementations. Nominal identity includes the defining module,
declaration, and canonical arguments. Equal member names do not establish an
interface implementation. The same nominal relation governs argument binding
and bounds. Checked dynamic conversion remains a separate operation; dynamic
cannot prove a bound. Use `is` or `as` to establish a concrete type first.

User generic applications and mutable arrays, Set, and Map remain invariant.
Future retains its existing payload-adoption contract without creating general
generic covariance. There is no int/double promotion or implicit common numeric
supertype. Function assignment and overrides retain their existing signature
rules; no general function subtyping is introduced.

Aliases validate their own bounds and every application in their expanded target.
For example, an unconstrained alias cannot forward T to a declaration requiring
`T extends Reader<int>`. Aliases do not create runtime nominal types.

## Inference and function values

Explicit arguments fix their parameters. Otherwise argument types, expected
results, callbacks, and declaration bounds enter one constraint domain. Parameter
dependencies, invariant equalities, nominal projections, and recursive bounds
propagate together until a unique most-specific valid solution is available.

```vela
interface Reader<E> { read(): E; }
class IntReader implements Reader<int> { read(): int { return 42; } }
func read<T extends Reader<E>, E>(source: T): E { return source.read(); }
func main(): int { return read(IntReader()); }
```

Here E is inferred from the actual `Reader<int>` implementation. Conflicting
nominal paths are errors. For an F-bound `X extends A<X>`, an argument of class C
can select B through the explicit chain `C extends B`, `B extends A<B>`; solving
is not limited to trying C and checking its bound afterward. Without a relevant
bound, ordinary inference retains strict equal-type and nullable merging.

No evidence means no default. A context-free call to
`func empty<T extends Reader<int>>(): T[]` cannot select its bound as T. Inconsistent
or incomparable valid candidates produce diagnostics. Inference never selects
dynamic, an arbitrary parent, or the first implementation path to hide ambiguity.

Dependent lambdas, generic function/method values, empty constructions, and
nested calls wait for sufficient context, including through conditional, match,
coalescing, and array expressions. Deferred checking retains source-position flow
facts and effects. Runtime evaluation remains exactly once in source order.

An ordinary function type can specialize a generic value:

```vela
func identity<T>(value: T): T { return value; }
func main(): int {
  const operation: (int) => int = identity;
  return operation(42);
}
```

Without sufficient context, an open generic value is an error. There are no
runtime polymorphic function values or generic lambdas. Array callbacks retain
their existing prefix-parameter contract; ordinary callbacks require exact arity.

## Methods, interfaces, and execution

Receiver arguments determine owner parameters before method parameters are solved.
Static methods follow the existing owner binding rules. Captured method values
retain their receiver and concrete application using the ordinary closure layout.
Interface selectors carry concrete method arguments and every reachable explicit
implementation. Generic implementations and overrides must match parameter counts,
normalized bounds, and method signatures after alpha renaming. They cannot tighten
a caller's contract.

Dynamic receivers cannot supply an open method declaration. Narrow or cast to a
known class/interface before a generic method call. Ordinary non-generic dynamic
calls retain their existing checked boundary.

The frontend publishes separate abstract-definition facts and concrete execution
facts, including parameter identity, canonical applications, final signatures,
and resolved targets. Backends consume concrete types and targets without solving
inference variables or interpreting generated names. Bounds require no new VM
instruction. GC, exceptions, async, interface dispatch, JIT, and AOT use existing
concrete runtime contracts.

Host imports and AOT exports retain concrete signatures. Script generics can call
concrete C++ bindings; they cannot expose an open generic host ABI. Standalone
native distribution includes required concrete implementations before publication.

## Persistence and tools

Declaration contracts retain normalized bounds and nominal references. Cache and
native-image identity cover these facts. Parameter counts, semantic bounds, or
interface method-contract changes require Reload. Alpha renames, equivalent
transparent bounds, and import-prefix changes preserve semantic identity.
Reload creates a fresh Isolate. Implementation-body updates preserve the ordinary
version protocol: old frames, closures, suspended continuations, and deoptimization
records keep their own concrete signatures.

Unused definitions support parameter/bound hover, definition lookup, and bound-member
completion. Type-parameter rename respects lexical identity and bound references,
and rejects capture or collision. Formatting, documentation, semantic tokens,
editor highlighting, and persistent REPL use the same syntax and type contracts.
Incomplete trailing input does not discard facts from retained complete declarations.

See [type inference](type_inference.md), [core language](core_language.md),
[modules](modules.md), and [source tools](source_tooling.md). The
[Chinese specification](../zh-CN/generics.md#8-诊断与验证矩阵) also records the compiler
validation matrix and implementation acceptance boundaries.
