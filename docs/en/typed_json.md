# Typed JSON decoding design

[**English**](typed_json.md) | [简体中文](../zh-CN/typed_json.md)

> Status: M0–M5 are complete. Builds, regression tests, static checks, standalone AOT, and Release measurements passed on macOS arm64. Section 11 records two environment-dependent native distribution skips and unverified platforms.

The target type T defines the JSON input contract. A decoder checks the complete input and constructs an actual T. Callers need neither a separate schema nor an `as` expression.

```vela
class User {
  name: string;
  age: int;
  nickname: string? = null;
}

func main(): void {
  const result = JSON.safeParse<User>('{"name":"Vela","age":18}');
  if (result.success) {
    const user = result.value; // Both the static type and runtime instance are User.
    print(user.name);
  } else {
    const error = result.error;
    print('${error.path}: ${error.message}');
  }
}
```

## 1. API and type selection

The standard class is named `JSON` consistently across dynamic parse, generic parse, safeParse, and stringify. Do not retain a `Json` alias, duplicate registration, or old-name fallback. New result and error types are named `JSONParseResult<T>` and `JSONParseError`.

| Call | Result and failure behavior |
| --- | --- |
| `JSON.parse(source: string): dynamic` | Dynamic parsing under the renamed class; invalid input throws `Error(code: 'JsonDecodeError')` |
| `JSON.parse<T>(source: string): T` | Typed decoding; invalid input throws `JSONParseError` |
| `JSON.safeParse<T>(source: string): JSONParseResult<T>` | Typed decoding; invalid input returns a failure result |
| `JSON.stringify(value: dynamic): string` | Follows the existing JSON encoding contract |

These standard static methods require no import. Generic calls must supply T explicitly. A generic function may forward its own parameter; decodability is checked for its concrete applications. `JSON.safeParse(source)` without a target is a compile error and does not default to dynamic. `JSON.parse(source)` without type arguments always returns dynamic; an expected assignment type must not silently select typed decoding.

The `JSON.parse` function value remains `(string) => dynamic`. Explicitly specialized `JSON.parse<User>` and `JSON.safeParse<User>` values have signatures `(string) => User` and `(string) => JSONParseResult<User>`. Standard declarations, specialization, and tools must represent both parse call forms together. This does not add overloads or default generic arguments to ordinary user functions.

Source must be a string. The API does not accept arbitrary existing dynamic objects or provide conversion by stringify followed by parse. Nominal checks on existing objects continue to use `is`; statically known internal interfaces should declare their actual types.

## 2. Success and failure results

`JSONParseResult<T>` is a nominal result type created by the standard library, with no public constructor and an immutable discriminator. Its public read-only properties are:

| Property | Static type | Valid state |
| --- | --- | --- |
| `success` | `bool` | Both states |
| `value` | `T` | Success |
| `error` | `JSONParseError` | Failure |

The logical result shapes are below. This notation describes states, not new object-literal syntax:

```text
success: { success: true, value: User(name: 'Vela', age: 18) }
failure: { success: false, error: JSONParseError(...) }
```

A successful result stores only its success payload; a failed result stores only its error payload. Reading an inactive property reports `Error(code: 'TypeError')` through the ordinary exception path. It never supplies a fabricated T, a default, or null. After branching on success, callers can directly read `value: T`, without nullable-field promotion. This design requires neither general union types nor new field-discriminator refinement syntax. Reads through dynamic values, closures, or ordinary methods perform the same state check.

`JSON.safeParse<User?>('null')` succeeds with `success == true` and `value == null`. It differs from the failed result of `JSON.safeParse<User>('null')`. An immutable result state does not make its successful object immutable: User and its containers retain ordinary mutability.

`JSONParseError` is created by the decoder and has no public constructor. An obtained error can be thrown directly and caught with `catch (error: JSONParseError)`. This standard nominal type has these read-only properties:

| Property | Type | Meaning |
| --- | --- | --- |
| `code` | `string` | Stable input-error category |
| `path` | `string` | Input path starting at `$` |
| `expected` | `string?` | Target type, or null when inapplicable |
| `actual` | `string?` | Actual JSON kind; absent fields use `missing` |
| `offset` | `int?` | Zero-based original UTF-8 byte offset when known; otherwise null |
| `message` | `string` | Human-readable reason; wording is not a stable identifier |

An error thrown by `JSON.parse<T>` has the same fields as the safeParse failure payload for that input. Both APIs use one decoder and one input-error model.

## 3. Decodable types

| Target | Accepted JSON value |
| --- | --- |
| `bool`, `string` | Scalar of the same kind |
| `int` | Integer token without decimal point or exponent, within signed int64 |
| `double` | Number token with a decimal point or exponent, representable as finite binary64 |
| `T?` | Null or a value decodable as T |
| `T[]` | Array; decode every element and construct an actual T[] |
| `Map<string, T>` | Object; decode values in key insertion order and construct that concrete Map |
| Decodable concrete class | Object; decode against the constructor contract in section 4 |
| `dynamic` | Existing JSON value model: scalars, dynamic[], and Map<string, dynamic> |
| Transparent alias | Its expanded target |

Rules apply recursively to generic applications and nested containers. Numeric kinds follow [existing JSON semantics](json.md): `1` does not implicitly become double, and `1.0` does not become int. Strings and booleans do not participate in numeric conversion. Finite floating-point results retain existing rounding, subnormal, and signed-zero behavior.

Void, functions, interfaces, enums, Set, Future, byte storage, regular expressions, and other standard objects with dedicated runtime construction semantics are not automatically decodable. Map keys must be string; string keys are not parsed into another type. Matching member names do not implement an interface, and extra tags do not select a subclass. Unsupported types are rejected at compile time even inside nullable types or empty containers. A user class is decodable according to its constructor contract, not merely its object layout.

Regular recursive types may close the plan graph through references, as with Node containing Node? or Node[]. Expanding generic applications remain subject to [existing specialization limits](generics.md), producing diagnostics instead of infinite plans. JSON data is a finite tree; decoding does not restore identity, cycles, or shared references.

## 4. Classes and constructor parameters

Decoders must invoke the target's public construction path. They must not write raw object memory, bypass private access, or treat Map storage as a class layout. Input keys match constructor parameter names exactly and case-sensitively.

| Constructor form | JSON object contract |
| --- | --- |
| Generated field constructor, including `constructor { ... }` | Named parameters from the current class's public instance fields; fields without initializers are required, and fields with initializers may be omitted |
| Explicit `constructor(name: Type, ...)` | The complete explicit parameter list; field names add no keys, and all explicit parameters in the current language are required |
| Explicit `constructor()` | Accept only an empty object, then execute ordinary defaults and the constructor body |
| Non-public constructor | Target is not decodable at compile time |

A derived class uses only its own constructor signature. Base arguments and `super(...)` follow [ordinary construction rules](core_language.md#class-construction-from-fields). Private/static fields, getters, setters, and ordinary methods do not add JSON keys. An explicit parameter may initialize a private field through the constructor body, as in an ordinary call.

Absence and null are distinct. `nickname: string?` is required; `nickname: string? = null` may be omitted. Explicit null does not select a default and fails for a non-nullable target. A supplied value with the wrong type must not fall back to a default. Omitted defaults execute exactly once in existing field declaration order. Each instance constructs its own mutable defaults.

Materialize explicit values in array index order, Map insertion order, or class constructor parameter declaration order. Children complete before their parent construction. Omitted defaults and constructor bodies then follow ordinary construction rules. The order of keys in JSON does not change class parameter materialization order.

Unknown keys always report `unknown_field`. A key matching a field outside the constructor signature cannot assign it or be silently ignored. Version one has no ignore-unknown option, key renaming, default overrides, enum mapping, automatic date conversion, schema objects, or custom decoder hooks. Existing validation, calculations, and exceptions in a custom constructor retain ordinary call semantics.

## 5. Validation order and error boundaries

Each call performs complete syntax parsing, complete target-structure validation, and typed materialization. Syntax or structural failure executes no user field default or constructor body. Materialization begins only after all structural validation passes.

| `code` | Trigger |
| --- | --- |
| `syntax_error` | Existing strict grammar rejects JSON syntax, escapes, UTF-8, trailing data, etc. |
| `number_out_of_range` | A number exceeds existing JSON int64 or finite binary64 limits |
| `type_mismatch` | JSON kind differs from the target, including null at a non-nullable position |
| `missing_field` | A required constructor parameter is absent |
| `unknown_field` | An object key is outside the constructor signature |

Return only the first input error. Parse the complete syntax and validate numeric representability first. Then validate the target structure depth-first. Arrays use index order; Maps use input insertion order. A class object checks required parameter presence first, unknown keys second, and values recursively in constructor parameter declaration order last. The first missing parameter is chosen in declaration order; the first unknown key is chosen in input order.

Existing duplicate-key semantics remain: the last value wins while the first insertion position is retained. Typed structural validation uses that logical object. Whole-input numeric range checks cover every token, including overwritten values of duplicate keys. Keys are JSON-decoded before parameter matching. An input naming a private field receives only the ordinary unknown_field error, without revealing its type.

Paths start at `$`; array indices use `[2]`. Object keys matching `[A-Za-z_][A-Za-z_0-9]*` use `.name`; other keys use a JSON-escaped string in brackets, such as `$["display.name"]`. Missing-field offsets are null. Errors with a corresponding token retain its starting byte offset. Syntax errors retain the parser's error offset and nearest established structural path, using `$` when unavailable. Actual kinds are `null`, `bool`, `int`, `double`, `string`, `array`, `object`, or `missing`; errors do not copy complete input values.

For `unknown_field`, offset points to the unknown key token, `actual` describes the final effective value for that logical key, and `expected` is null.

safeParse converts only these input errors into failure results. Source-expression errors, out-of-memory conditions, cancellation, execution budgets or call-depth limits, and exceptions from defaults or constructors propagate through normal execution. A catch-all must not turn them into JSON mismatches. User side effects during materialization are not rolled back. An exception publishes no success result, but objects explicitly exposed by user code remain subject to normal ownership rules.

Even a JSONParseError explicitly thrown by a user constructor must propagate rather than become this call's input failure. Decode-stage failures and user exceptions have distinct origins and statuses; catching by exception type alone is insufficient.

## 6. Compiler and runtime pipeline

| Owner | Required contract |
| --- | --- |
| Standard declarations and frontend | Migrate the standard class and reserved name to JSON, removing Json registration; publish APIs, result properties, and error type; check concrete T decodability, visibility, parameter bindings, and recursive closure |
| SemanticModel | Publish one canonical decode-plan graph per type identity: constructor targets, parameter names/types, required/default markers, containers, and recursive edges; later stages do not rediscover a schema |
| Generics and IR | Generate concrete decoding entries and ordinary constructor calls from existing specialization facts; result value retains T; both typed APIs share a plan |
| runtime/json | Reuse strict JSON parsing and numeric semantics; supply document access with offsets, iterative validation state, and structured input errors |
| Native, JIT, and AOT | Share materialization, result accessors, and exception boundaries; user construction uses ordinary checked call frames with GC and source-stack contracts |
| Caches, images, and publication | Include decode plans, target types, constructor signatures, omission contracts, and generated entries; standalone output requires neither compiler nor source reflection |
| LSP, doc, fmt, and REPL | Use the same standard declarations and specialization facts for accurate generic signatures, result properties, constructor references, and undecodable-target diagnostics |

Retain the existing parser's raw JSON document until materialization completes. Do not build an entire managed dynamic graph merely to copy it into a typed graph. Validation and materialization consume the same canonical decode plan. Existing dynamic parse may continue its native-value materialization over the same syntax producer; do not maintain two JSON parsers with different errors or numeric rules.

A generic JSON call in a generic definition records a decodability obligation for specialization and propagates it through ordinary generic call relationships. Concrete applications must validate the complete closure. This obligation grants no reflection, arbitrary member access, or T() construction to ordinary type parameters, and cannot be evaded through dynamic erasure.

Runtime presence markers must be separate from parameter values, preserving omission versus explicit null. Reuse the authoritative ordinary constructor binding and default-initialization implementation while supporting runtime omission decisions for defaulted parameters. Do not enumerate 2^n call branches for n defaults or duplicate default-expression execution rules inside the decoder. Final field initialization and constructor bodies remain ordinary class construction.

Use iterative traversal state for deep data rather than mapping untrusted JSON depth directly to C++ recursion. Index object members once per object, or use equivalent linear processing, instead of scanning every key again for every parameter. For input length and reachable output size n, total parsing, validation, and materialization are O(n). Plan construction is measured separately by reachable concrete types and members.

## 7. Ownership, caching, and performance

One decoding session owns the raw document, field indexes, and traversal scratch storage, accounting for them through existing heap external-memory tracking. Explicitly bind source borrows, document access, and native-frame lifetimes. Root temporary managed values and partial construction graphs before user calls. Normal return, input failure, user exceptions, and resource failures all release session scratch resources.

Results, error strings, and typed graphs belong to the current Isolate; they borrow neither source, parsing arena, nor compiler objects. Failure results retain neither the full document, full source string, nor partially constructed graph. Objects explicitly exposed by user code are outside automatic rollback. Failure to allocate an error or result takes the OutOfMemory path without fabricating a failure payload.

Cache identity covers canonical T, constructor input contracts, recursive plans, and the standard-library implementation. Type or constructor contract changes trigger the corresponding existing invalidation or Reload rules. Implementation-body updates follow the version protocol; active frames retain their matching older plans and call targets. AOT reachability must retain referenced construction/default initialization and result/error layouts, removing unreachable specializations.

Acceptance includes Release measurements of scalars, objects, large arrays, nested generics, early/late structural failures, and constructor costs. Report latency, first-call allocation, peak memory, and retention after a full GC separately, comparing with handwritten decoders enforcing the same validation contract. Section 10 records measured results and their comparison boundaries.

## 8. Internal-tool migration and naming

Migrate only production paths that actually consume JSON text and then convert fields individually. Native bridge results in sdk:test / sdk:mock, function results, and existing objects must not be serialized and reparsed. Address their opcode/dynamic-result contracts through accurate bridge interfaces, record types, and ordinary `is` refinement; do not hide signature problems behind the new parsing API.

New public APIs, generated decoding entries, and related project-owned helper declarations do not start with underscores. Use non-exported declarations or private members for internal visibility. JSON keys retain their spelling. Existing custom underscore prefixes in the SDK, mock generator, corresponding native bindings, and Python tools form a separate migration scope; remove old names without aliases. Do not rewrite language/platform-required protocol names. Import aliases in `import ... as ...` are unrelated to casts.

## 9. Implementation stages and completion

Validate each stage against the table below. Passing a stage does not complete this feature. Connect every production path, remove replaced duplicate logic, and satisfy the acceptance matrix before marking delivery.

| Stage | Implementation scope | Completion criteria |
| --- | --- | --- |
| M0: declarations and results | Complete Json → JSON rename, standard APIs, nominal result/error, read-only access, explicit T and function values | Success value is T; failure has no fabricated payload; successful null is distinct; inactive-property access is checked |
| M1: decodability and plans | Generics, aliases, constructor signatures, omission/defaults, recursive closure | Precise unsupported-member diagnostics; one plan identity; no reflection or open generic host ABI |
| M2: validation and materialization | Document access, scalars/containers/classes, field presence, ordinary construction | Input errors execute no user code; complete default/explicit-parameter contracts; stable paths and ordering |
| M3: execution and lifetimes | JIT/AOT, GC, exceptions, execution limits, standalone publication | Backend equivalence; cleanup on every exit; execution without compiler; retained active older versions |
| M4: tools and migration | LSP/doc/fmt/REPL, caches, actual JSON consumers, and standard-class references in all project-owned examples/tests/docs | JSON decode paths need no scattered as expressions; no old duplicate parser, stale entry, or underscore-generated names |
| M5: final acceptance | Full regression, static checks, publication checks, Release measurements | Entire matrix below satisfied; environment skips and unverified platforms recorded; document status and roadmap updated |

Track the SDK bridge and underscore-name migrations separately; delivery of JSON.safeParse does not substitute for their acceptance.

The SDK migration covers custom underscore-prefixed declarations in `sdk:test`, `sdk:mock`, the mock generator, native bindings, and Python tools. Native opcodes have named bridges with exact signatures; traces use a five-argument callback, and composite matchers use `Matcher[]`. Four checked casts remain at actual dynamic boundaries: test helpers inspecting arbitrary generic container lengths and key membership, and mock response payloads recovering `R` after response-signature validation. These paths do not accept JSON text or convert existing objects through JSON round trips.

| Validation area | Required coverage |
| --- | --- |
| API and static types | One JSON standard registration, no old Json standard name or alias, user-defined Json follows ordinary name rules; missing-T diagnostics; standard-name shadowing; explicit function values; generic forwarding; value is T; wrong-state getters; read-only properties and rejected dynamic writes |
| JSON syntax and numbers | Existing grammar, UTF-8/escapes/duplicate keys; int64 limits, binary64 rounding/overflow, negative zero; no implicit mixing of numeric kinds |
| Structural errors | First-error order, escaped key paths, indices, unknown/missing/null distinctions, private/static/getter-name keys, byte offsets |
| Class construction | Generated/explicit construction, skipped defaults and independent per-instance defaults, private initialization, inheritance/super, constructor exceptions and side effects |
| Composite types | Nested arrays/Maps/generics/aliases, regular recursion, expanding-specialization diagnostics, illegal element types even in empty containers, successful null for T? |
| Execution and GC | Deep/large input; small heaps and every allocation-failure site; GC inside defaults/constructors; execution limits; reuse after failure; no input retention |
| Delivery and tools | Same values/errors in JIT/AOT; standalone native distribution; cache invalidation/hot reload; consistent LSP/doc/format/REPL signatures |

Background contracts: [JSON and dynamic values](json.md), [generics](generics.md), [exceptions](exceptions.md), and [native execution](native_execution.md).

## 10. Release measurements

Measurements ran on 2026-11-02 using an Apple M1 Pro, macOS arm64, Release, Clang 23, and LLVM 21.1.8. Each case ran in a separate process with seven repetitions and a Google Benchmark minimum time of 0.3 seconds; compilation and input preparation are excluded. Exact commands, source identity, coefficients of variation, and memory counters are in the [rolling benchmark summary](../../benchmarks/results/rolling_summary.json) entry whose `suite.identity` is `typed-json/7-workloads/typed-and-handwritten`. The implementation is in [typed_json_benchmark.cc](../../benchmarks/typed_json_benchmark.cc).

| Case | Typed median (µs) | Handwritten median (µs) | Typed / handwritten |
| --- | ---: | ---: | ---: |
| Scalar | 0.429 | 0.254 | 1.687 |
| Object | 0.706 | 0.796 | 0.887 |
| 1024-element array | 375.666 | 652.250 | 0.576 |
| Nested generics | 344.550 | 635.383 | 0.542 |
| Structural failure at first element | 118.214 | 4167.124 | 0.028 |
| Structural failure at last element | 222.033 | 5751.728 | 0.039 |
| Array with constructor bodies | 442.669 | 704.627 | 0.628 |

Each cell below lists typed / handwritten. First-call allocations count cumulative successful bytes and allocations, including managed blocks and external blocks accounted by Heap. Peak memory is the increase in live Heap bytes above the post-compilation baseline. All 14 cases retained zero bytes after a full GC.

| Case | First-call allocated bytes | First-call allocation count | Peak Heap increase (bytes) |
| --- | ---: | ---: | ---: |
| Scalar | 550 / 230 | 9 / 3 | 550 / 128 |
| Object | 1846 / 1942 | 12 / 13 | 1774 / 1472 |
| 1024-element array | 1638301 / 1458413 | 3084 / 8227 | 1490709 / 1221248 |
| Nested generics | 1639665 / 1527425 | 3088 / 8247 | 1491945 / 1289664 |
| Structural failure at first element | 1360288 / 5495152 | 16 / 71701 | 1286424 / 1430976 |
| Structural failure at last element | 1360288 / 5751767 | 16 / 73763 | 1286424 / 1430976 |
| Array with constructor bodies | 1638301 / 1458413 | 3084 / 8227 | 1490709 / 1221248 |

The handwritten baseline targets these fixed int, Item, and Box inputs. It uses dynamic parse to check the complete syntax, validates all structure before invoking constructors, and separately reconstructs original UTF-8 offsets on failure while checking code, path, expected, actual, and offset. The comparison does not cover arbitrary target types, syntax or resource failures, or nominal result APIs. Failure ratios include the handwritten offset-recovery cost and do not measure syntax parsing alone.

The highest wall-time coefficient of variation in this run was approximately 29%; the table reports this run's medians. Scalars incur fixed session overhead, and successful large-array decoding allocates more cumulative bytes and reaches a higher Heap peak than the handwritten baseline. Process RSS peaks recorded in the summary include JIT compilation and are not decoder-only peaks. These results do not establish universal speedups, zero allocation, or performance on other platforms.

## 11. Acceptance scope

Debug default artifacts and all test targets, plus Release benchmark targets, built successfully on macOS arm64. The seven base, frontend, IR, runtime, AOT, JIT, and compiler C++ suites passed 2330 tests; the final full compiler regression contained 1213 tests. JSON-specific acceptance also covers small heaps, every allocation-failure site, deep recursion, stop requests, constructor stack limits, cache invalidation, hot reload, and active old-version lifetimes. Both standalone AOT tooling tests passed, including 58 native host checks.

The Python tooling regression passed 160 of 162 tests. Two native distribution tests were skipped because the environment did not supply an audited `VELA_TEST_TARGET_SUPPORT_SPEC`: packaged language-corpus JIT/AOT comparison, and packaged builds with source-free execution. Independently linked native host acceptance does not require this configuration and passed. Windows, Linux, and other architectures remain unverified.

Repository-wide `vl lint` checked 666 C++ files and headers with zero failures; `bugprone-unchecked-optional-access` remains enabled. Formatting of changed C++ files, plus UTF-8, LF, final newlines, relative documentation links, and `git diff --check` for all changes, passed. Sections 8 and 9 define the completed SDK bridge-signature and custom underscore-name migration scope; the four retained checked casts remain at the documented dynamic boundaries.
