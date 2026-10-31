# Basic language contracts

[**English**](language_basics.md) | [简体中文](../zh-CN/language_basics.md)


This document defines the P0 control-flow, nullability, scope, and diagnostic
contracts. Numeric conversions remain explicit; `int` and `double` do not mix.

## Switch statements

```typescript
switch (value) {
  case 0:
  case 1:
    print("small");
    break;
  default:
    print("other");
}
```

The selector is evaluated exactly once. Its type must be non-nullable `int`,
`bool`, `string`, or an enum. Case labels must have that exact type and be
compile-time constants. Supported constants include literals, explicitly
qualified enum members, earlier `const` bindings with constant initializers,
and supported unary/binary constant expressions. Calls, mutable bindings,
overflow, and division by zero cannot produce case constants. Duplicate case
values and multiple defaults are errors.

Empty labels share the following statements. A non-empty branch with a path
into the next label is a compile-time error; use `break`, `continue`, `return`,
`throw`, or control flow that exits on every path. The final branch may finish
normally. A default may appear anywhere and follows the same rule; it is entered
only when no case matches. An empty switch still evaluates its selector.

`break` exits the nearest loop or switch. `continue` targets the nearest loop,
passing through enclosing switches; in a `for` loop it executes the increment.
Each case body has a separate lexical scope, so entering a later case cannot
observe uninitialized declarations from an earlier case. Use an outer binding
to share state between branches.

Return analysis composes fallthrough branches from last to first, consumes
switch breaks, and preserves loop continues. Without a default, an unmatched
selector remains a fallthrough path. Exhaustive `match` expressions are a
separate construct; see [P1 matching contracts](core_language.md#normal-enums-and-exhaustive-match-expressions).

The current lowering uses typed comparisons and jumps, with O(n) dispatch for
n labels. Labels have no runtime evaluation or allocation. Jump tables and
hash dispatch are not part of this milestone. Existing bytecode register,
constant, and jump-distance limits still produce compilation diagnostics.

## Nullable types

`T?` contains exactly the values of `T` plus `null`. `T` may be assigned to
`T?` without changing its representation or value. This is type widening;
it does not allow numeric conversion or a different underlying type.
`null` cannot initialize or be returned as a non-nullable type.
The `?` suffix describes the type, not whether a variable, field, or argument
may be omitted. Local declarations still require initializers.

```typescript
func read(value: int?): int {
  if (value == null) return 0;
  return value + 1;
}
```

Nullable parameters, locals, returns, class fields, and array elements are
supported. `void?` and repeated `?` suffixes are errors. A bare null initializer
requires an explicit nullable annotation. Array suffixes bind in source order:

| Type | Meaning |
| --- | --- |
| `int?[]` | Non-nullable array of nullable integers |
| `int[]?` | Nullable array of non-nullable integers |
| `int?[]?` | Nullable array of nullable integers |

Array element types remain invariant: an existing `int[]` is not an `int?[]`.
An array literal may use its expected element type, including null elements.

Non-null checks promote local variables and parameters on applicable paths.
The analysis understands `== null`, `!= null`, operand reversal, logical
negation, short-circuit `&&`/`||`, and guard returns. Local `let` and `const`
initializers follow the same rule as later assignments: a statically non-null
value establishes a non-null fact; a nullable or dynamic value cannot prove it.
An assignment replaces the previous fact. Calls that may modify a captured mutable
local invalidate its fact. Promotion does not change the declared type, so an
`int?` local can still be assigned null and can still use `??`.
Branches intersect facts at joins. Loops and switches conservatively discard
facts for locals they may write. Loop backedges and continue paths cannot
reuse a fact that a previous iteration could invalidate.

```typescript
func main(): void {
  let value: int? = 3;
  print(value + 1); // 4: the initializer proves value is non-null.
  value = null;
  // print(value + 1); // Error: value may be null.
}
```

Using an operand that may be null in numeric arithmetic, ordering, bitwise
operations, or boolean conditions reports the operand and requests a null check.
String concatenation and interpolation still format nullable primitive values
under the [string-context rules](strings.md).

Conditional expressions check each arm with its condition's facts and merge
only facts shared by both arms. A write in one arm does not affect checking
the other arm; for example, `value != null ? value.length : 0` is valid.

Fields and indexed elements are not promoted because another operation may
change their contents. For a non-null value, copy the value to a local and check
it, or use null-safe operators: `?.` skips member access or a method call when
the receiver is null, and `?.[index]` also skips evaluation of the index when
the array is null.

`left ?? right` evaluates the left operand once and evaluates the right operand
only when the left result is null. It returns the selected value without
assigning to the left variable, field, or array element. `??=` is not supported;
writing the result back requires explicit assignment.

```typescript
let value: int? = null;
const result: int = value ?? 42; // result is 42; value remains null.
```

Null comparisons are allowed for value types. Equality of nullable values
requires the same underlying type; null equals only null. Non-null strings
compare by contents, primitives by their existing typed equality, and class
objects by identity. Array and builder equality remain undefined except for
comparison with null.

Registers and fields already store tagged `Value` slots, so nullable values
need no wrapper object. Nullable primitive arrays use contiguous `Value`
slots. Nullable reference arrays retain pointer-sized elements, using a null
pointer for null. Non-nullable primitive arrays retain compact native storage.
GC scans reference arrays and applies their existing write barriers.

## Scope and declaration order

Top-level types, function signatures, fields, and method signatures are
collected before function bodies. They may refer to later declarations,
including mutually recursive functions. Type/function name collisions are
errors. Field and method names cannot collide within a class.

Local bindings become visible after their initializer is checked. Initializers
may refer to an existing outer binding of the same name; self-reference without
an outer binding is an unknown-name error. Locals must be declared before use.
Duplicates in one scope are errors. Function parameters and the outer function
body share a scope, so the body cannot redeclare a parameter.

Nested blocks may shadow outer bindings. Locals also take precedence over
global function, constructor, and enum names; a local value cannot be called
as a function. `this` is reserved for the constant method receiver and cannot
be declared or shadowed explicitly. `const` prevents rebinding, not mutation
through object or array references.

An `if` branch or loop body has a local scope even without braces. A `for`
initializer is visible to its condition, body, and increment, but not after
the loop. Each case has its own scope as described above.

## Diagnostic locations

Lexer tokens, AST nodes, and IR instructions carry a compact source ID and
one-based line/column. Columns count UTF-8 bytes; tabs count as one byte, and
LF advances the line. CRLF source also advances once per LF. Filenames are
stored once in the program rather than copied into every AST node.

Diagnostics own their source path, so results survive destruction of the parsed
program. File checking and compilation report the actual imported file for
syntax and type errors. Import resolution failures point to the importing
string literal. In-memory multi-source compilation uses `<source:1>`,
`<source:2>`, etc. Single unnamed sources retain line/column without a path.
The CLI renders `path:line:column: error: message`.

Duplicate declarations point to the repeated declaration token; duplicate
enum members retain their individual location. Unterminated block comments
point to their opening delimiter. Parser recovery preserves closing braces
and the following top-level declaration after an incomplete function body.
