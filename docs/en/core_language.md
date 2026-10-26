# Core language specification

[**English**](core_language.md) | [简体中文](../zh-CN/core_language.md)


This document defines the core language features, object model, and runtime contracts for Vela.
Function type annotations contain only parameter types, followed by `=>` and
the return type; parameter names remain in function declarations and lambdas.
Success/error results follow the
requested `std::expected` model as `Expected<T, E>`, replacing the earlier
payload-enum proposal. Runnable versions are in [`examples/`](../../examples/).
`func`, postfix annotations, strict types, explicit
enum qualification, `T?`, and C/C++ switch fallthrough remain established rules.

## Functions, function values, and closures

```typescript
func add(left: int, right: int): int {
  return left + right;
}

func apply(value: int, operation: (int) => int): int {
  return operation(value);
}

func makeCounter(start: int): () => int {
  let current: int = start;
  return (): int => {
    current = current + 1;
    return current;
  };
}

func functionDemo(): void {
  const operation: (int, int) => int = add;
  print(operation(20, 22));             // 42

  const twice: (int) => int = value => value * 2;
  print(apply(21, twice));              // 42

  const counter: () => int = makeCounter(40);
  print(counter());                     // 41
  print(counter());                     // 42
}
```

Function types list parameter types in order without parameter names, such as
`(int, int) => int`. Calls through these types use positional arguments.
Function declarations and lambdas still declare named parameters. Lambda
annotations can be inferred from their expected function type, such as
`const twice: (int) => int = value => value * 2`. Without parameter context,
`(value: int) => value * 2` infers its return type from the body. See
[type inference](type_inference.md) for the shared rules and diagnostics.
Lambdas use `=>`; both expression and
block bodies are supported. Parameters and return types remain explicit in
function declarations. Captures use lexical bindings: mutation of a captured `let` is
shared, and its lifetime extends beyond the defining call. Capturing a `const`
does not make its referenced object immutable.

Use inference or an alias to avoid repeating a long function type:

```typescript
type BinaryOperation = (int, int) => int;

func aliasDemo(): void {
  const operation: BinaryOperation = add;
  const inferred = add;
  print(operation(20, 22));             // 42
  print(inferred(20, 22));              // 42
}
```

Instance method values retain their receiver, evaluated once when the value is
created. This includes primitive, string, builder, array, collection, and Expected
builtins, inherited methods, and interface/super bindings. A null-safe method
access returns null for a null receiver; a live binding keeps its receiver alive.

```vela
func builtinValues(): void {
  const values = [20, 22];
  const copy: () => int[] = values.slice;
  const append: (int) => int = values.push;
  const map: ((int) => string) => string[] = values.map;
  append(42);
  print(copy().length); // 3
  print(map(value => value.toString()).join(',')); // 20,22,42
}
```

A function value has a fixed positional signature. A contextual function type
selects a legal argument count for optional/variadic builtin methods and fixes
callback/result types for higher-order methods. Without such context, an ordinary
fixed-parameter builtin value uses its complete parameter list; variadic and
callback algorithm values require a function type context. For example, inferred
`values.slice` takes two indices, while the annotated copy above takes none.
Defaults are evaluated by the underlying method at invocation, not at binding.
No dynamic dispatcher or second callable ABI is introduced.

## Generic functions, classes, and aliases

```typescript
type UserId = int;
type IntOperation = (int) => int;

func identity<T>(value: T): T {
  return value;
}

class Box<T> {
  public param value: T;

  public get(): T {
    return this.value;
  }
}

func genericDemo(): void {
  const id: UserId = identity<int>(42);
  const box: Box<int> = Box<int>(value: id);
  print(box.get());                     // 42
}
```

Aliases are transparent names, not distinct nominal types. Generic arguments
are explicit. The compiler specializes static generic uses,
preserving primitive storage instead of boxing every `T` as a heap object.
Equivalent transparent aliases reuse the same specialization. Function and
class templates are checked when instantiated. Expansion is bounded to 256
specializations per compilation, and type nesting is bounded to 128; exceeding
either limit produces a diagnostic. Expanding recursive templates cannot
silently consume unbounded compiler memory.

## Interfaces and explicit implementation

```typescript
interface Measurable {
  measure(): int;
}

class Rectangle implements Measurable {
  public param width: int;
  public param height: int;

  public measure(): int {
    return this.width * this.height;
  }
}

func area(value: Measurable): int {
  return value.measure();
}

func interfaceDemo(): void {
  const rectangle: Rectangle = Rectangle(width: 6, height: 7);
  print(area(rectangle));               // 42

  const measure: () => int = rectangle.measure;
  print(measure());                     // 42
}
```

`implements` must be declared; matching method names alone do not implement an
interface. Signatures must agree without numeric conversion. Using an object
as an interface preserves its identity and numeric representation. Concrete
calls remain statically resolved; interface calls need runtime dispatch.
Method values bind their receiver.

## Expected success/error results

```typescript
func divide(numerator: int, denominator: int): Expected<int, string> {
  if (denominator == 0) {
    return err("division by zero");
  }
  return ok(numerator / denominator);
}

func expectedDemo(): void {
  const result: Expected<int, string> = divide(84, 2);
  if (result.hasValue()) {
    print(result.value());              // 42
  } else {
    print(result.error());
  }
}
```

`Expected<T, E>` contains exactly one active alternative: a success value of
type `T`, or an error of type `E`. It is a generic result type, not an enum
declaration. The selected explicit factories are `ok(value: T)` and
`err(error: E)`. The compiler infers their generic arguments from an expected
`Expected<T, E>` type, such as a function return type or an annotated local.
If context cannot determine both arguments, an explicit type annotation is
required. This is compile-time inference, not a runtime conversion.
There is no conversion from a plain value/error to Expected,
or from Expected to bool. `hasValue()` returns bool. `value()` and `error()`
are checked accessors; accessing the inactive alternative reports a runtime
failure. The error type may be a string, a normal enum, or another value type.
Success and error types may be identical without construction ambiguity.
`Expected<void, E>` represents successful completion without a value, using
`ok()` without an argument.

This follows the success/error contract of C++ `std::expected`, while keeping
Vela's explicit conversions and runtime failure reporting. It does not import
C++ exceptions, implicit converting constructors, or unchecked access.
The current representation is a GC object with a header discriminant and one
active payload slot. Construction allocates; access performs a tag check and
does not use string-key lookup or a dynamic property dictionary.

## Normal enums and exhaustive match expressions

```typescript
enum Brightness {
  light,
  dark,
}

func brightnessName(value: Brightness): string {
  return match (value) {
    Brightness.light => "light",
    Brightness.dark => "dark",
  };
}

func nullableValue(value: int?): int {
  return match (value) {
    null => 0,
    let present => present,
  };
}

func enumDemo(): void {
  print(brightnessName(Brightness.light)); // light
}
```

Match evaluates its selector once and chooses one arm, without fallthrough.
Every possible value must be covered; missing enum variants are compile errors.
`let` binds the selected value; `_` is a wildcard. Arms must produce the same type, apart
from the existing permitted nullable widening. A selector covering all non-null
values followed by a null arm, or vice versa, supports nullable narrowing.
The catch-all `let present` above excludes null after the preceding null arm.
Matching does not add conversions; string formatting follows the
[string context rules](strings.md). Traditional `switch`
keeps its existing C/C++ fallthrough semantics.
Constant patterns support integers, booleans, strings, and explicitly qualified
enum members. Double literal patterns, destructuring, and guards are not
supported; wildcard and binding arms can handle other selector types.

## Type tests and checked casts

```typescript
func rectangleArea(value: Measurable): int {
  if (value is Rectangle) {
    return value.width * value.height;
  }
  return value.measure();
}

func requireRectangle(value: Measurable): Rectangle {
  return value as Rectangle;
}
```

`is` tests runtime type and narrows stable bindings on the matching branch.
`as` checks the type and reports a runtime cast failure on mismatch. Neither
performs a numeric conversion. Numeric conversions remain explicit
`toInt()`/`toDouble()` operations.

## Class construction with param fields

The selected field modifier is `param`, written after visibility and before
the field name. It declares a field and a same-name constructor parameter
together. This replaces the earlier Dart field-formal constructor examples:

```typescript
class Counter {
  public param value: int;
  public param step: int = 1;

  private count: int = 0;

  constructor {
    // All required fields are initialized before this body executes.
  }

  public next(): int {
    this.value = this.value + this.step;
    this.count = this.count + 1;
    return this.value;
  }
}

func main(): void {
  functionDemo();
  genericDemo();
  interfaceDemo();
  expectedDemo();
  enumDemo();

  const counter: Counter = Counter(value: 40);
  print(counter.next());               // 41
}
```

`param` is an instance-field modifier, not an annotation. Constructor arguments
generated from these fields are named: a field without an initializer is
required, and a field with an initializer supplies the default when omitted.
Visibility controls member access independently of parameter generation.
Ordinary fields do not create constructor parameters; static fields cannot
use `param`. Calls retain `ClassName(...)` syntax without `new`.

`constructor { ... }` is an optional body for the generated constructor, not
a zero-argument parameter declaration. Fields must be initialized before that
body runs. Explicit arguments evaluate once in source order. Omitted defaults
then evaluate in field declaration order, followed by the constructor body.
A supplied argument skips its field's default. Defaults cannot access `this`
or caller locals. The generated constructor keeps its newly allocated receiver
rooted and stores arguments directly into fields; it does not retain every
field argument in a separate register until the body starts.

Classes requiring hand-written parameters can instead declare
`constructor(name: Type, ...) { ... }`. Hand-written parameter lists and
`param` fields are mutually exclusive. This makes the signature's source
explicit and avoids silently injecting parameters into `constructor()`.

Parameter binding and field offsets are resolved at compile time. There is
no runtime annotation execution, reflection, or argument-name dictionary.

For explicit constructors, defaults run before the body; definite initialization
analysis requires every field to be initialized on every returning path. Reading
an uninitialized field, calling an instance method, or exposing `this` before
initialization is rejected. Members default to `public`; `private` is scoped to
the declaring class. Private `param` fields still create named parameters.

A derived constructor does not inherit the base class's `param` arguments. When
the base has required parameters, the derived constructor must pass them through
an explicit `super(...)` call:

```typescript
class Counter {
  public param value: int;
}

class BonusCounter extends Counter {
  public param bonus: int;

  constructor {
    super(value: this.bonus * 20);
  }
}

const counter = BonusCounter(bonus: 2); // value = 40, bonus = 2
```

When every base parameter has a default, or the base has no required parameters,
the derived constructor may omit `super()`. The base constructor then runs
implicitly before the derived constructor body. Omitting `super(...)` when the
base has a required parameter is a compile-time error.

## Static members

```typescript
class Counts {
  private static count: int = 0;
  public static next(): int {
    Counts.count = Counts.count + 1;
    return Counts.count;
  }
}
```

Static fields require initializers. Each field initializes lazily on first
access and then persists across executions of the same compiled program.
Program copies share this state; separate compilations do not. Initializer
cycles report `CyclicInitialization`; failed initialization resets the active
initializer state for a later attempt. Static slots remain GC roots as long
as the program is alive. Static methods have no `this` and resolve directly.

## Explicit numeric conversions

```typescript
const whole: int = 42.9.toInt();
const fraction: double = whole.toDouble();
```

`int` and `double` expose `toInt()` / `toDouble()`. Double-to-int truncates
toward zero; non-finite values and values outside `[-2^63, 2^63)` report
`InvalidNumericConversion`. Int-to-double rounds according to binary64 and
can lose precision beyond 53 significant bits. Same-type conversions preserve
the value, including signed zero. These operations allocate no GC objects.
Arithmetic, assignment, arguments, and results still forbid implicit numeric
conversion. `is` and `as` do not perform numeric conversion.

`int.parse` / `int.tryParse` and `double.parse` / `double.tryParse` explicitly
convert complete strings to numbers. Integer parsing checks signed int64 bounds;
tryParse returns null for invalid text. See [numeric parsing](numeric_parse.md)
for radix selection, floating-point grammar, and errors.

## Fixed byte storage

`ByteArray(length: int)` owns zero-initialized, fixed-length raw bytes. Indexed
reads return int; writes require int values in 0..255. `ByteArray.from(int[])` and
`slice()` copy storage. `ByteData.view(bytes, ...)` shares and retains the owner
and provides checked unaligned integer/float access with Endian enum arguments.
Neither byte type implicitly converts to int[] or string. See
[byte storage and typed views](byte_data.md).

## Regular expressions

`RegExp` and `RegExpMatch` are built-in reference types with identity equality.
They support nullable annotations, fields, closures, invariant containers and
async payloads. RegExpMatch is created by searches; all properties and match
indexing are read-only. Coordinates count Unicode code points. See
[regular expressions](regexp.md) and the [example](../../examples/regexp.vela).

## Ordered Set and Map

```typescript
const names: Set<string> = Set<string>();
names.add("Vela");                  // bool: true when inserted
names.has("Vela");                  // bool
names.delete("Vela");               // bool: true when removed
names.clear();                       // void
const count: int = names.size;
const items: string[] = names.values();

const scores: Map<string, int> = Map<string, int>();
scores.set("Vela", 42);              // void
const score: int? = scores.get("Vela");
const keys: string[] = scores.keys();
const values: int[] = scores.values();
```

An annotated destination also permits `Set()` or `Map()` with contextual type
arguments. Types are invariant; null keys/values require nullable parameters.
`get()` returns null for a missing key; `has()` distinguishes missing entries
from a stored null. Maps also expose `has`, `delete`, `clear`, and `size`.
Snapshot arrays preserve insertion order. Updating an existing key retains its
position; removal and reinsertion place it last. Strings compare by contents,
reference values by identity, all NaNs are the same key, and +0/-0 are the same
key. Ordinary numeric equality keeps its existing IEEE semantics.

Keys and values use typed contiguous storage with an open-addressed hash index
and insertion-order links. Lookup/update is expected amortized O(1); snapshots,
clearing and live-entry tracing are O(size). String-key hashing additionally
costs O(key byte length). Capacity growth and compaction
occasionally cost O(size). Backing allocations count toward heap pressure and
are released by GC. Arrays are growable `T[]`; no duplicate `List<T>` container
is introduced. See [growable arrays](arrays.md).

## Unicode code points and graphemes

```typescript
const text: string = "e\u0301🇨🇳";
const points: int[] = text.codePoints();
const clusters: string[] = text.graphemes();
const count: int = text.graphemeLength;
const first: string = text.graphemeSlice(0, 1);
```

Strings remain validated UTF-8. Existing `length`, indexing, `slice`, and
search offsets use Unicode code points. Grapheme APIs use extended grapheme
clusters, with an inclusive start and exclusive end. Invalid slice bounds
report `IndexOutOfBounds`. No normalization or locale-specific tailoring is
performed. Segmentation uses pinned utf8proc 2.12.0 / Unicode 18.0.0.
`codePoints()` and `graphemes()` materialize exact-sized arrays through linear
passes; they do not rescan a string once per indexed element. Grapheme length
and slicing scan linearly; they do not promise constant-time random access.

## Execution and ownership

Calls, including recursion and closure calls, use explicit VM frames rather
than recursive C++ execution. The fixed frame-depth limit reports
`StackOverflow`; a failed execution can be followed by another call.
Mutable captures use shared GC cells, constant captures use values, and method
values keep their receiver alive. Bound functions and closures currently
allocate GC objects. Interface calls use precomputed selector tables;
concrete calls remain direct. Selectors are assigned only to referenced interface
methods, in source order. Building class metadata visits each class's declared
implementations rather than scanning all interfaces for every class.

Program metadata is validated at compilation/loading, then shared immutably.
Layout tables are shared across functions. The owning Isolate must outlive
programs, function handles, and their borrowed layouts. Exact roots and write
barriers cover captures, static slots, Expected payloads, and collections.
Large-object tracing is not subdivided into per-entry work budgets, so the
incremental collector does not guarantee a hard maximum pause time.
