# Vela strings

[**English**](strings.md) | [简体中文](../zh-CN/strings.md)


## Encoding and types

`string` is immutable, valid UTF-8. Source files, runtime storage, and console
output use UTF-8. Embedded NUL bytes are allowed and do not terminate a string.
Length, indexing, slice boundaries, and search results count Unicode scalar
values (code points), not bytes or grapheme clusters. There is no `char` type.

For `"A中😀e\u0301"`, `length` is 5 and `byteLength` is 11. The final `e` and
combining accent count separately. Vela does not normalize Unicode; equality
compares the exact encoded content. `length` is cached and takes O(1).
ASCII indexing takes O(1). For other strings, a cached code point position
allows consecutive forward or reverse indexing in amortized O(1) per access;
`for (let i = 0; i < text.length; i++) { text[i]; }` takes O(n) in total.
Arbitrary jumps still take O(n) in the worst case. Use `codePoints()` to
materialize a native `int[]` in linear time when repeated random access is
needed, then index that array in constant time. Grapheme APIs (`graphemes()`,
`graphemeLength`, `graphemeSlice(start, end)`) use extended grapheme clusters;
see [P1 Unicode contracts](core_language.md#unicode-code-points-and-graphemes).

## Literals, escapes, and interpolation

Single- and double-quoted literals have the same `string` type and interpolation
rules. The formatter defaults to single quotes. Both styles support `\"`, `\'`, `\\`, `\n`, `\r`, `\t`, `\b`, `\f`,
`\v`, `\0`, and `\$`. `\xHH` encodes the scalar U+00HH as UTF-8 (it does not
inject a possibly invalid raw byte). Unicode escapes use `\uXXXX` or `\u{H...}` with
one to six hexadecimal digits in braces. A fixed-width high surrogate must be
followed immediately by a fixed-width low-surrogate escape; together they
encode one scalar value. Lone surrogates, surrogate values in braces, values
above U+10FFFF, missing digits, unknown escapes, invalid source UTF-8, raw
newlines, and unfinished strings/interpolations produce compilation diagnostics.

```typescript
const name = 'Vela';
const greeting = 'Hello, ${name}! Count: ${42}';
const literal = '\${this is literal text}';
const emoji = '\u{1F600}';
```

`${expression}` accepts `string`, `int`, `double`, `bool`, `null`, and nullable
versions of those primitive types. Null formats as `null`.
Nested strings and interpolation are allowed. Each expression runs once in
source order, and its value is saved before the next expression runs. The
same formatting applies to `+` when at least one operand has string type,
including nullable string. For example, `'count=' + 42` produces `count=42`.
`+` remains left-associative: `1 + 2 + '3'` produces `33`; `'1' + 2 + 3`
produces `123`. Objects and arrays require an explicit representation;
there is no automatic object placeholder or user method invocation.
Assignments, function arguments, numeric arithmetic, equality, and conditions
retain their existing strict type rules. Unicode escapes that decode to `$` do not start
interpolation. Import paths decode escapes but reject interpolation and NUL.
File checking and compilation share a dependency loader, so escaped paths and
Unicode filenames resolve consistently. Imports are parsed once in dependency
graphs, with duplicate files loaded once and explicit module exports.

`int.toString()`, `double.toString()`, and `bool.toString()` allocate strings
explicitly. Integer output is decimal; booleans are `true`/`false`. Doubles use
the same shortest round-trip `std::to_chars` formatter as console output,
including signed zero, infinities, and NaNs. Formatting is independent of
stream locale and numeric formatting flags.

## API

| Expression | Result | Contract |
| --- | --- | --- |
| `text.length` | `int` | Cached code point count, O(1) |
| `text.byteLength` | `int` | Encoded byte count, O(1) |
| `text[index]` | `string` | One code point; ASCII O(1), consecutive forward/reverse access amortized O(1), arbitrary jumps worst-case O(n); allocates 1–4 bytes of content |
| `text.slice(start: int, end: int)` | `string` | Half-open code point range; locates boundaries using the position cache and copies the result |
| `text.indexOf(text: string)` | `int` | First matching code point offset, or -1; empty pattern matches at 0 |
| `text.includes(text: string)` | `bool` | Whether the pattern occurs; empty pattern is contained |
| `text.startsWith(prefix: string)` | `bool` | Exact prefix, including the empty prefix |
| `text.endsWith(suffix: string)` | `bool` | Exact suffix, including the empty suffix |
| `left + right` | `string` | At least one operand has string type; formats primitive/null operands into one final string |
| `parts.join(separator?: string)` | `string` | Separator defaults to comma; null contributes empty text; primitive arrays, including nullable elements; empty array produces an empty string |
| `String()` | `String` | Creates an empty, GC-owned mutable builder |
| `builder.append(text: string)` | `void` | Copies text into its buffer with geometric capacity growth |
| `builder.toString()` | `string` | Creates an independent immutable snapshot |

`String` is the mutable construction type; lowercase `string` is the immutable
UTF-8 value type. Use `toString()` to create an immutable snapshot.

All listed parameters are required. Arguments may be positional or named and
are evaluated in source order even when named arguments reorder parameters.
Indices must satisfy `0 <= index < length`; slice bounds must satisfy
`0 <= start <= end <= length`. Invalid ranges return the VM's recoverable
`IndexOutOfBounds` status. There is no negative indexing, clamping, or implicit
numeric conversion. Strings' indexed values and length properties cannot be
assigned. `String` is a reserved builtin declaration name. There is no
global `join`; ordinary user declarations may use that name. The array receiver
evaluates before the separator, each once. Array element types remain homogeneous:
`[1, 2].join(',')` is valid; `[name, 42, true]` is not. Use
`'${name} / ${42} / ${true}'` to format mixed values without creating an array.

`indexOf` and `includes` use KMP for multi-byte patterns, guaranteeing linear
search time for repeated-prefix inputs. Short pattern tables use stack storage;
patterns longer than 64 bytes allocate a temporary native prefix table and
report allocation failure explicitly. Prefix/suffix tests compare bytes
directly; valid UTF-8 guarantees these matches respect code point boundaries.

## Compilation, allocation, and GC

Every string creation path initializes the code point count. Concatenation,
interpolation and joins reuse input counts where available; raw UTF-8 input is
counted during creation. The count uses existing header padding and the
position cache shares a metadata slot unused by strings, so neither increases
the object header size. String bytes remain immutable, but the runtime may
update integer position metadata on the Isolate's owning thread. VM, JIT and
AOT consumers must preserve this distinction: the whole object is not read-only.
The cache contains no managed references and needs no GC write barrier.

The lexer emits decoded literal segments and expression tokens. The parser
builds an interpolation node; the checker validates every segment/expression and
records builtin bindings. Typed IR retains an interpolation operation and
statically selected builtin IDs, carried directly into register bytecode. The
program creation/loading boundary validates builtin IDs and register ranges
once. Runtime calls
return distinct type, bounds, and allocation errors.

Interpolation saves its parts in contiguous rooted frame slots. It discards
empty literal segments, computes the total byte count, allocates the final
string once, and copies each part directly. Primitive formatting uses native
scratch storage and never allocates intermediate GC strings. `parts.join(separator)`
borrows native array storage and follows the same allocation path, with separator space
included in overflow checks. Neither operation constructs intermediate
concatenation strings. Allocations performed by the expressions themselves,
such as an explicit `toString()`, are separate from the final allocation.

Builders are leaf objects in the existing non-moving collector. Their native
buffers grow geometrically, so a sequence of appends copies O(total bytes)
overall. Buffer capacity contributes to both `allocated_bytes` and
`committed_bytes`, influences collection thresholds, and is released when the
builder is swept or its Isolate is destroyed. Snapshots copy their bytes;
neither snapshots nor future appends keep old buffers alive. Builders in class
fields and typed/nested arrays use the existing reference barriers and tracing.

Every allocation input backed by the GC heap must remain rooted until the
allocation finishes. The VM keeps receivers, arguments, interpolation parts,
array elements, and separators reachable from registered frames. Destinations
are written after allocation, including when raw bytecode aliases an input.
Hosts must root returned VM values before another allocation or collection.
Low-level heap string/builder interfaces accept valid UTF-8; untrusted native
input must first use `InvalidUtf8Offset`. The source lexer is the current
external text validation boundary.

## Case conversion and regular expression matching

`text.toLowerCase(): string` and `text.toUpperCase(): string` implement Unicode
18.0.0 default full case conversion, including expansions and Final_Sigma.
They are locale-independent and do not normalize text. See
[string case conversion](string_case.md).

Dart-style RegExp and RegExpMatch provide PCRE2 Unicode matching with Vela
code-point coordinates. See [regular expressions](regexp.md).

## Examples

Run `examples/strings.vela` for escapes, interpolation, conversions, string operations, `join`, and an imported helper. `examples/string_builder.vela` demonstrates loop construction and immutable snapshots.
