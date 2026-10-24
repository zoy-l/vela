# Vela strings

## Encoding and types

`string` is immutable, valid UTF-8. Source files, runtime storage, and console
output use UTF-8. Embedded NUL bytes are allowed and do not terminate a string.
Length, indexing, slice boundaries, and search results count Unicode scalar
values (code points), not bytes or grapheme clusters. There is no `char` type.

For `"A中😀e\u0301"`, `length` is 5 and `byteLength` is 11. The final `e` and
combining accent count separately. Vela does not normalize Unicode; equality
compares the exact encoded content. Random code point indexing requires a
scan. A loop that repeatedly indexes a long string can therefore take quadratic
time; code point iterators and grapheme APIs remain future work.

## Literals, escapes, and interpolation

Double-quoted literals support `\"`, `\'`, `\\`, `\n`, `\r`, `\t`, `\b`, `\f`,
`\v`, `\0`, and `\$`. `\xHH` encodes the scalar U+00HH as UTF-8 (it does not
inject a possibly invalid raw byte). Unicode escapes use `\uXXXX` or `\u{H...}` with
one to six hexadecimal digits in braces. A fixed-width high surrogate must be
followed immediately by a fixed-width low-surrogate escape; together they
encode one scalar value. Lone surrogates, surrogate values in braces, values
above U+10FFFF, missing digits, unknown escapes, invalid source UTF-8, raw
newlines, and unfinished strings/interpolations produce compilation diagnostics.

```typescript
const name = "Vela";
const greeting = "Hello, ${name}! Count: ${42.toString()}";
const literal = "\${this is literal text}";
const emoji = "\u{1F600}";
```

`${expression}` accepts an arbitrary expression whose result is `string`.
Nested strings and interpolation are allowed. Each expression runs once in
source order, and its value is saved before the next expression runs. The
compiler rejects `${42}`, `${true}`, and mixed string/numeric `+`; it never
inserts conversions. Unicode escapes that decode to `$` do not start
interpolation. Import paths decode escapes but reject interpolation and NUL.
File checking and compilation share a dependency loader, so escaped paths and
Unicode filenames resolve consistently. Imports are parsed once in dependency
order, with duplicate files skipped and cycles diagnosed.

`int.toString()`, `double.toString()`, and `bool.toString()` allocate strings
explicitly. Integer output is decimal; booleans are `true`/`false`. Doubles use
the same shortest round-trip `std::to_chars` formatter as console output,
including signed zero, infinities, and NaNs. Formatting is independent of
stream locale and numeric formatting flags.

## API

| Expression | Result | Contract |
| --- | --- | --- |
| `text.length` | `int` | Code point count, O(byte length) |
| `text.byteLength` | `int` | Encoded byte count, O(1) |
| `text[index]` | `string` | One code point; scans to the index and allocates 1–4 bytes of content |
| `text.slice(start: int, end: int)` | `string` | Half-open code point range; scans boundaries once and copies the result |
| `text.indexOf(text: string)` | `int` | First matching code point offset, or -1; empty pattern matches at 0 |
| `text.contains(text: string)` | `bool` | Whether the pattern occurs; empty pattern is contained |
| `text.startsWith(prefix: string)` | `bool` | Exact prefix, including the empty prefix |
| `text.endsWith(suffix: string)` | `bool` | Exact suffix, including the empty suffix |
| `left + right` | `string` | Both operands must be strings; copies into one new flat string |
| `join(parts: string[], separator: string)` | `string` | Joins the current array elements; empty array produces an empty string |
| `StringBuilder()` | `StringBuilder` | Creates an empty, GC-owned mutable builder |
| `builder.append(text: string)` | `void` | Copies text into its buffer with geometric capacity growth |
| `builder.toString()` | `string` | Creates an independent immutable snapshot |

All listed parameters are required. Arguments may be positional or named and
are evaluated in source order even when named arguments reorder parameters.
Indices must satisfy `0 <= index < length`; slice bounds must satisfy
`0 <= start <= end <= length`. Invalid ranges return the VM's recoverable
`IndexOutOfBounds` status. There is no negative indexing, clamping, or implicit
numeric conversion. Strings' indexed values and length properties cannot be
assigned. `join` and `StringBuilder` are reserved builtin declaration names.

`indexOf` and `contains` use KMP for multi-byte patterns, guaranteeing linear
search time for repeated-prefix inputs. Short pattern tables use stack storage;
patterns longer than 64 bytes allocate a temporary native prefix table and
report allocation failure explicitly. Prefix/suffix tests compare bytes
directly; valid UTF-8 guarantees these matches respect code point boundaries.

## Compilation, allocation, and GC

The lexer emits decoded literal segments and expression tokens. The parser
builds an interpolation node; the checker validates every segment/expression and
records builtin bindings. Typed IR retains an interpolation operation and
statically selected builtin IDs, carried directly into register bytecode. The
VM validates builtin IDs and register ranges before execution. Runtime calls
return distinct type, bounds, and allocation errors.

Interpolation saves its parts in contiguous rooted frame slots. It discards
empty literal segments, computes the total byte count, allocates the final
string once, and copies each part directly. `join` borrows the array's native
string pointer span and follows the same allocation path, with separator space
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

## Examples and measurement

Run `examples/strings.vela` for escapes, interpolation, conversions, string
operations, `join`, and an imported helper. `examples/string_builder.vela`
demonstrates loop construction and immutable snapshots. See
[string performance measurements](string_performance.md) for Release results,
memory accounting, comparison methodology, and known measurement limits.
