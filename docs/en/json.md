# JSON and dynamic values

[**English**](json.md) | [简体中文](../zh-CN/json.md)

This document defines the JSON API and checked dynamic value contracts. The runnable
example is [`examples/json.vela`](../../examples/json.vela).

Vela uses Dart's JSON value model: decoding produces ordinary primitive values,
arrays, and ordered maps; encoding accepts those values directly. The public
names are `Json.parse` and `Json.stringify`. There is no separate JSON node
class or node-construction API.

```vela
func main(): void {
  const payload: Map<string, dynamic> = Map<string, dynamic>();
  payload.set('name', 'Vela');
  const items: dynamic[] = [1, true, null];
  payload.set('items', items);

  const text: string = Json.stringify(payload);
  const root: dynamic = Json.parse(text);
  const name: string = root.get('name') ?? '';
  print(name);
}
```

This specification covers in-memory strings, the dynamic type needed by JSON,
and conversion through a public `toJson()` method. It does not add file, stream,
or network I/O; general map literals; revivers; replacers; conversion callbacks;
or formatting options. [Byte storage and views](byte_data.md) have separate
types and ownership contracts.

## Public API and value model

| API | Contract |
| --- | --- |
| `Json.parse(source: string): dynamic` | Parse one complete JSON value; return a native Vela value |
| `Json.stringify(value: dynamic): string` | Encode a value graph as compact JSON; reject unsupported values and cycles |

`Json` is a standard static class, available without an import and not
constructible. Its declaration participates in the standard-source dependency
closure and compilation-cache fingerprint, like the existing standard objects.

| JSON value | Decoded Vela value |
| --- | --- |
| `null` | Vela `null` |
| Boolean | `bool` |
| Integer token without a decimal point or exponent | Signed 64-bit `int` |
| Number token containing a decimal point or exponent | `double` |
| String | Immutable UTF-8 `string` |
| Array | `dynamic[]` |
| Object | `Map<string, dynamic>` |

All JSON root types are accepted. Every decoded array uses `dynamic[]`, even
when its current elements are all integers. Object arrays contain maps; parsing
does not construct application class instances. A missing map entry and an
entry containing JSON null both produce null from `get`; use `has` to distinguish
them. Object access uses the existing Map methods, not property-name shorthand.

## Dynamic typing and checked operations

`dynamic` can hold any Vela value, including null, primitives, enums, instances,
functions, and containers. It is a language type, not a JSON-only wrapper or a
replacement for an invalid frontend type. `void` is not a storable value.
`dynamic` already includes null; an additional `?` is redundant and is rejected.

Assignment to `dynamic` preserves the original value, reference identity, and
language type. Assignment from `dynamic` to a concrete type inserts a runtime
type check. The same rule applies to initialization, assignments, arguments,
returns, fields, indexed writes, and closure captures that cross into a typed
slot. A successful check does not perform numeric conversion or container
conversion. Failure follows the existing checked-cast error path.

```vela
const values: dynamic[] = Json.parse('[7, "Vela", true]');
const id: int = values[0];       // Runtime check succeeds.
const name: string = values[1];  // Runtime check succeeds.
const wrong: int = values[1];    // Catchable checked-cast failure.
```

Member access, method calls, callable invocation, indexing, and operators on a
dynamic receiver are permitted and checked at runtime. Dispatch uses the actual
receiver type and its existing public API. It checks member existence, writable
status, argument names and count, parameter types, and element constraints before
performing the operation. Missing members and invalid operations produce a
catchable type error. Dynamic dispatch cannot access private members, create new
instance fields, or bypass a read-only property. A method obtained as a value
retains its receiver.

Dynamic operations retain Vela's ordinary semantics: integer overflow remains
checked; int/double conversion remains explicit; array bounds remain checked;
and a condition must evaluate to bool. Null-safe operators retain their existing
evaluation order and short-circuit rules. They do not introduce truthiness,
JavaScript coercions, or arbitrary user-defined operators.

`is` and `as` inspect the retained language type. Existing flow refinement
extends to dynamic locals and parameters, and is invalidated by assignments or
calls that can modify captured bindings. Fields and indexed elements are not
promoted. Without a known function signature, a dynamic call cannot provide
context for unannotated lambda parameters; ordinary inference diagnostics apply.
Generic callables must be instantiated before erasure; a dynamic call does not
accept explicit type arguments.

Dynamic typing is introduced by an explicit annotation, a declared dynamic
result, or a dynamic operation. Mixed array literals are allowed in a
`dynamic[]` context; an array literal in a `dynamic` context creates `dynamic[]`.
Absent such a context, existing strict array and return-type inference remains
in force. Unresolved names and inference failures never become dynamic.

## Containers and business model conversion

Vela containers remain invariant. Storing an `int[]` in a dynamic variable does
not turn its storage into `dynamic[]`. A dynamic call that attempts to insert a
string into that array fails before mutation. Map and Set writes preserve their
declared key/value or element types in the same way.

Conversely, a decoded `dynamic[]` does not become `int[]` just because all its
current elements are integers. Use an explicit checked copy:

```vela
const values: dynamic[] = Json.parse('[1, 2, 3]');
const integers: int[] = values.map(value => value as int);
```

The map operation is eager. It either returns a new typed array or propagates
the first failed element check. It does not publish a partially filled result.
An object array similarly needs an explicit model conversion function that
checks required fields, nullability, and field types. Casting a decoded map to
`User` does not instantiate a User. Typed decoding and generated model converters
are outside the first version.

Dynamic `map` derives the result array's element type from the callback's retained
result signature. Dynamic `reduce` preserves the initial accumulator's actual
type; a null initial value requires a nullable or dynamic accumulator in the
callback signature. Callback contracts are checked even when the array is empty.

Dynamic array elements and dynamic Map/Set slots use tagged storage. Each value
retains the information needed by existing equality and hashing rules, including
enum identity. Existing compact storage for statically typed primitive containers
is preserved. Dynamic keys are supported by ordinary collections; JSON encoding
separately requires every actual object key to be a string.

## Parsing contracts

Parsing accepts the complete JSON value grammar, including heterogeneous arrays.
Only JSON whitespace is accepted around the root. Empty input, extra root values,
trailing garbage, comments, trailing commas, a leading BOM, invalid escapes,
unescaped control characters, and invalid Unicode are errors. UTF-16 escape pairs
are decoded to valid UTF-8; isolated surrogates are rejected. Embedded NUL is
preserved as string content and never terminates a key or value.

Integer tokens must be in `[-2^63, 2^63 - 1]`. Oversized integers are rejected,
including values that a JSON library would otherwise represent as uint64 or
double. Decimal and exponent tokens are converted to binary64 without an
intermediate integer conversion. Finite results use IEEE 754 rounding, including
underflow to signed zero; overflow to infinity is rejected. NaN and infinity
literals are not JSON. Integer `-0` becomes zero; floating negative zero is
preserved.

Object keys are strings. Duplicate keys overwrite the previous value while
retaining the key's original insertion position, following the existing ordered
Map contract. Keys are compared by decoded string contents.

Parsing returns only after the entire input and all numeric and string values
have been validated. Returned strings and containers belong to the Isolate heap;
they do not borrow the input string or a temporary JSON arena.

## Encoding and application objects

Encoding directly supports null, bool, int, finite double, string, arrays, and
maps whose actual keys are all strings. Typed arrays and maps are traversed
directly without converting their whole storage to dynamic containers. Elements
and map values are checked recursively. Enum values are not encoded as their
integer storage. Functions, Set, Expected, Future, mutable String, and byte-storage
objects are not directly encodable.

An application class instance is encodable through a public, non-generic,
zero-argument instance method named `toJson`. Its result is processed by the same
encoding rules. The method must return a value. There is no automatic field
reflection, getter enumeration, or conversion based on a class's memory layout.

```vela
class User {
  name: string;
  age: int;

  toJson(): Map<string, dynamic> {
    const result: Map<string, dynamic> = Map<string, dynamic>();
    result.set('name', this.name);
    result.set('age', this.age);
    return result;
  }
}

func main(): void {
  const users: User[] = [User(name: 'Vela', age: 7)];
  print(Json.stringify(users));
}
```

Each occurrence of an object invokes `toJson` once for that occurrence. Exceptions
thrown by the method propagate unchanged through ordinary VM frames and retain
their original trace. An unsupported result is an encoding error. Encoding is
synchronous; a Future returned by an async conversion is unsupported.

Cycle detection tracks identities on the current traversal path, including
objects undergoing `toJson` conversion. Returning the receiver from `toJson`, a
container referring to itself, and indirect cycles all fail. Repeated references
in separate branches are legal and are serialized independently.

Traversal uses array order and Map insertion order. Before invoking user code,
the encoder records structural mutation versions for active containers; after
returning, it checks them before using a saved position. A structural change
raises the existing concurrent-modification error, even if the size is restored.
Replacing an existing entry is permitted and is visible if that entry has not yet
been visited. Encoding does not provide a snapshot or roll back user side effects.

Output has no insignificant whitespace or terminal newline. Strings escape
quotes, backslashes, and control characters while retaining other valid UTF-8.
Integers use exact decimal output. Finite doubles use locale-independent shortest
round-trip formatting; output retains a decimal point or exponent so decoding
preserves double kind. Floating negative zero is written as `-0.0`.

## Errors, ownership, and resource use

| Failure | Result |
| --- | --- |
| JSON syntax, Unicode, or numeric-range failure | `Error` with code `JsonDecodeError`; message includes a zero-based UTF-8 byte offset |
| Unsupported encoded value or key, non-finite number, or cycle | `Error` with code `JsonEncodeError`; message identifies the failing value path |
| Dynamic cast, call, member, or index failure | Existing checked-cast, type, or bounds error path |
| Structural mutation during conversion | Existing `ConcurrentModification` path |
| Exception from `toJson` | Original exception and trace |
| Allocation or storage exhaustion | Existing recoverable `OutOfMemory` path |

An unexpected EOF uses the input byte length as its offset. Other decode offsets
refer to the original input, not an unescaped string or modified parser buffer.
JSON-specific error codes must survive builtin/VM propagation without being
flattened into a generic native failure. Error-object allocation follows the
existing VM failure contract when memory is exhausted.

The parser roots partially built containers and values before allocation
safepoints. The encoder roots its input, active containers, conversion receivers,
and conversion results across calls and allocations. Dynamic fields, arrays,
maps, captures, globals, and suspended locals are traced by their actual value
tags; reference publication uses the existing write barrier. JSON temporaries
are released on success and failure, and their external allocations are included
in runtime memory accounting.

Use explicit traversal state rather than unbounded C++ recursion. Complexity is
linear in input/output size plus expected ordered-map operations; duplicate-key
handling must not scan previous keys. Shared graphs may generate repeated output,
so encode cost is measured against output size and conversion work. There is no
language-level fixed nesting limit; resource exhaustion is reported explicitly.

## Implementation and completion

Existing [standard declarations](../../compiler/frontend/standard_environment.cc),
[builtin signatures](../../compiler/frontend/builtins.h),
[runtime values](../../runtime/value.h), and
[JSON infrastructure](../../base/json.h) establish the integration boundaries.
The existing JSON helper owns yyjson arenas; runtime values must not expose those
nodes. Reuse the pinned yyjson dependency and shared low-level infrastructure.
Preserve raw numeric tokens and original offsets so library numeric coercion
cannot violate the int64 contract. Runtime does not depend on compiler/tooling.

| Stage | Required completion |
| --- | --- |
| Standard environment | Register Json and its dependent declarations once; preserve reserved names, local shadowing, source overlays, and cache fingerprints |
| Frontend | Add Dynamic; check concrete-boundary conversions, contextual mixed literals, dynamic operations, and refinement invalidation; retain container invariance |
| Runtime identity | Distinguish enums from int, full function signatures, class identities, container arguments, and Future arguments after erasure; reuse canonical type metadata |
| Runtime storage and GC | Add genuine tagged dynamic slots to arrays/collections and trace all dynamic-bearing storage; preserve primitive fast paths |
| IR and compiler | Represent checked conversions and dynamic dispatch explicitly; describe allocations, calls, reads, writes, and throwing effects; preserve evaluation order |
| VM and errors | Resolve public members and exact signatures; implement parse/encode and resumable encoding state around ordinary `toJson` calls, without reentering the VM from a C++ builtin |
| Images and caches | Serialize required type and dispatch metadata, validate indices/layouts at load, update format/build identities, and reject incompatible images |
| Tooling | Share checked signatures and dynamic facts with completion, hover, diagnostics, formatting, documentation, debugger, and persistent REPL |
| Delivery | Add `examples/json.vela`, update the feature checklist only after all production paths and required checks pass |

The [offline layout descriptions](../../compiler/ir/layout_description.cc) and
[runtime instantiation](../../runtime/program_layouts.cc) retain nominal
class, interface and enum identities, structural function and Expected signatures,
and Future/Completer payload signatures. Arrays and collections retain their full
element constraints. Boxed enums preserve identity in erased slots while typed
enum arrays retain integer storage. Isolate-owned metadata survives compilation
results and is rebuilt and validated when a bytecode image is loaded.

Required validation covers all JSON root kinds, heterogeneous and object arrays,
missing versus null entries, duplicate-key order, escaped Unicode and embedded
NUL, int64 endpoints and overflow, binary64 boundaries and signed zero, malformed
input offsets, non-string keys, unsupported types, cycles versus shared branches,
toJson success/failure/mutation, and VM reuse after caught errors. Type tests cover
every dynamic-to-typed boundary, private/read-only members, named calls, callable
signatures, enum identity, generic arguments, invariant-container writes, literal
contexts, and invalid inference. GC stress covers partial parsing, conversion
calls, dynamic slots, images loaded in a fresh Isolate, and retained REPL values.

At implementation completion, run relevant Debug/Release regressions, tooling
tests, repository static checks, and Release benchmarks. Measure parse and encode
throughput, allocations and retained memory, dynamic dispatch, typed-boundary
checks, and regressions in ordinary typed code. Completed aggregate measurements
are retained in [the rolling benchmark summary](../../benchmarks/results/rolling_summary.json).

## References

- [Dart jsonDecode](https://api.dart.dev/dart-convert/jsonDecode.html) and [jsonEncode](https://api.dart.dev/dart-convert/jsonEncode.html).
- [Dart JSON implementation and conversion contract](https://github.com/dart-lang/sdk/blob/main/sdk/lib/convert/json.dart).
- [Vela core types and collections](core_language.md), [exceptions](exceptions.md), and [compilation cache](compilation_cache.md).
