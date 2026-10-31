# Regular expressions

[**English**](regexp.md) | [简体中文](../zh-CN/regexp.md)

> Status: implemented. RegExp and RegExpMatch use pinned PCRE2 10.49 with Unicode 17.0.0 tables, code-point coordinates and accounted native storage.

Vela uses Dart-style `RegExp` objects and match results, backed by the standalone
PCRE2 library. Patterns follow the pinned PCRE2 version's syntax and matching
rules with the configuration below. Positions use Vela's existing Unicode code
point indices. A pattern is compiled at construction; matching does not mutate
a cursor on the RegExp object. The API makes no ECMAScript compatibility promise.

```vela
func main(): void {
  const pattern = RegExp('([A-Za-z]+)=([0-9]+)');
  const found: RegExpMatch? = pattern.firstMatch('age=42');
  if (found != null) {
    print(found.group(1) ?? ''); // age
    print(found.group(2) ?? ''); // 42
  }

  const numbers = RegExp('[0-9]+');
  const matches: RegExpMatch[] = numbers.allMatches('7, 42');
  print(matches.length); // 2
}
```

This API is independent of dynamic typing and JSON. The first version does not
add a general Pattern/Iterable protocol, regular-expression literals, raw string
syntax, replacement/split APIs, or user callbacks during matching.
[String case conversion](string_case.md) is specified separately.

## RegExp API

| API | Result / contract |
| --- | --- |
| `RegExp(source: string, multiLine?: bool, caseSensitive?: bool, dotAll?: bool)` | Construct and compile a pattern; defaults are false, true, false respectively |
| `RegExp.escape(text: string): string` | Produce a PCRE2 literal pattern fragment for text; output spelling is unspecified |
| `pattern.pattern: string` | Read-only original decoded source string |
| `pattern.isMultiLine: bool` | Read-only multiLine setting |
| `pattern.isCaseSensitive: bool` | Read-only caseSensitive setting |
| `pattern.isDotAll: bool` | Read-only dotAll setting |
| `pattern.isUnicode: bool` | Read-only true; matching always uses Unicode mode |
| `pattern.hasMatch(input: string): bool` | Whether any match exists |
| `pattern.firstMatch(input: string): RegExpMatch?` | First match, or null |
| `pattern.allMatches(input: string, start?: int): RegExpMatch[]` | All non-overlapping matches from start, default zero |
| `pattern.matchAsPrefix(input: string, start?: int): RegExpMatch?` | Anchored matching attempt at start, default zero; return a match or null |
| `pattern.stringMatch(input: string): string?` | The first whole matched substring, or null |

RegExp and RegExpMatch are dedicated built-in reference types, not user
subclasses or dynamic property maps. They support ordinary fields, parameters,
returns, nullable annotations, captures, and invariant typed containers.
RegExpMatch cannot be directly constructed by user code. All properties are
read-only. Register the standard declarations and dependency closure using the
existing reserved-name, local-shadowing, overlay, and cache-fingerprint rules.

Arguments follow ordinary Vela evaluation order and positional/named calling
rules. Optional parameters are omitted arguments, not nullable values. No
implicit number/string conversion is performed.

## Matching and Unicode

PCRE2 defines pattern semantics, including ordered alternatives, greedy/lazy and
possessive quantifiers, captures, backreferences, lookarounds, Unicode property
escapes, and inline options. Matching uses `pcre2_match`, not the DFA interface;
the native backtracking result is exposed. Do not translate patterns into a
different dialect or maintain an ECMAScript grammar checker. PCRE2's own
constraints apply: for example, variable-length lookbehind must have a bounded
maximum length allowed by the pinned build; unlimited repetition in lookbehind
is not promised. Unsupported patterns produce a compile error.

| Setting | Vela contract |
| --- | --- |
| Code unit width | Only the 8-bit library, matching UTF-8 directly |
| `PCRE2_UTF` | Always enabled |
| `PCRE2_UCP` | Always enabled; shorthand classes such as `\d`, `\w`, `\s` use PCRE2 Unicode-property defaults |
| `PCRE2_NEVER_BACKSLASH_C` | Always enabled; `\C` cannot split a UTF-8 character |
| `multiLine` | Maps to `PCRE2_MULTILINE`, initially false |
| `caseSensitive` | False enables `PCRE2_CASELESS`, initially true |
| `dotAll` | Maps to `PCRE2_DOTALL`, initially false |
| Newline convention | Explicit `PCRE2_NEWLINE_ANY` |
| `\R` convention | Explicit `PCRE2_BSR_UNICODE` |
| JIT | Disabled; use the interpreter |

Constructor properties report the initial option values. PCRE2 inline options
and supported pattern directives retain their native meaning, including scoped
overrides. UTF mode and the prohibition on `\C` cannot be relaxed. Leave
ECMAScript-compatibility, locale-specific casing, and allowing `\K` inside
lookarounds disabled; in particular, do not enable
`PCRE2_EXTRA_ALLOW_LOOKAROUND_BSK`, which can produce reversed whole-match spans.
Use the library's default character tables, not tables made from host locale.

Ignore-case matching follows PCRE2's Unicode caseless rules, not full string
case expansion. Do not lowercase or uppercase the subject before matching:
that changes positions and is not the engine's comparison algorithm.

`RegExp.escape` escapes PCRE2 metacharacters and handles literal whitespace and
`#` safely even when its fragment is embedded in extended mode. Scalar hex
escapes may be used where necessary. It does not escape Vela source literals;
the exact generated fragment is not a serialization contract.

Vela strings are valid UTF-8, and all exposed positions count Unicode scalar
values. Unicode mode is mandatory; there is no `unicode: false` option.
`allMatches` eagerly returns the existing `RegExpMatch[]` type rather than adding
a lazy Iterable. Pattern text uses ordinary Vela strings: write
`RegExp('\\d+')` to pass a backslash followed by d to the pattern compiler.
There is no Dart raw-string or JavaScript regex-literal syntax in this version.

Matching preserves the input exactly: there is no normalization, grapheme
segmentation, or locale-dependent behavior. `.` consumes one code point in its
ordinary single-character use, including a supplementary character. Embedded
NUL is supported in both input and pattern; APIs always pass explicit lengths.
Composed and decomposed strings remain distinct unless the pattern matches them
explicitly. Unicode tables are pinned dependency data, never host locale data.

## Positions and repeated matching

For input length N, start must satisfy `0 <= start <= N`; invalid starts use the
existing bounds error. Positions are relative to the original input, and every
match spans the half-open interval `[start, end)`. A start argument controls the
search position, not the input's origin or the context available to anchors and
lookbehind. The engine receives the whole input and a byte start offset.
matchAsPrefix uses `PCRE2_ANCHORED` for that matching attempt. Native `\K` may
reset the reported whole-match start after the anchored attempt began; capture
groups may also span text outside the reported whole match. These are PCRE2
semantics, not an error or a reason to discard a successful result.

```vela
const found = RegExp('Vela').firstMatch('😀Vela');
if (found != null) {
  print(found.start); // 1
  print(found.end);   // 5
}
```

`allMatches` searches from the requested position, then continues from the end
of each nonempty match. After an empty match, advance by one complete code point.
An empty match at N is included once and terminates enumeration. An empty
pattern therefore yields N + 1 empty matches. This rule also applies to empty
matches produced by alternatives or quantifiers.

Calls do not share iteration state. Repeating firstMatch or hasMatch with the
same object and input produces the same result. There is no lastIndex, global,
or sticky flag; matchAsPrefix provides an explicitly anchored operation.

## RegExpMatch API

| API | Result / contract |
| --- | --- |
| `match.start: int` | Whole-match start in code points |
| `match.end: int` | Whole-match exclusive end in code points |
| `match.input: string` | Original input string |
| `match.pattern: RegExp` | RegExp that produced this match |
| `match.groupCount: int` | Capturing-group count, excluding group zero |
| `match.groupNames: string[]` | Snapshot of unique names in PCRE2 name-table order (lexicographic UTF-8 order) |
| `match.group(index: int): string?` | Whole match at zero, capture at 1..groupCount; null if the capture did not participate |
| `match[index]: string?` | Same behavior as group(index); read-only |
| `match.groups(indices: int[]): string?[]` | Requested captures in request order |
| `match.namedGroup(name: string): string?` | Named capture, or null when that defined group did not participate |

An existing capture matching zero characters returns an empty string, not null.
Group zero always participates. Native branch-reset numbering is preserved;
groupCount is the highest valid capture number. If a pattern enables duplicate
names, namedGroup uses PCRE2's selection of the first participating occurrence
and returns null if none participated. An out-of-range numeric group index
throws the existing bounds error; an unknown group name throws `Error` with code
`RegExpGroupError`. Negative indices are not supported. `groups` validates
indices before publishing a result and returns an independent array. Repeated
groupNames access returns independent snapshots; changing a snapshot does not
change compiled metadata.

## Engine, ownership, and failures

Integrate PCRE2 through DEPS and GN with Unicode enabled, only its 8-bit source
closure, and JIT disabled. Its C API returns explicit errors and fits Vela's
exception-free C++ build. Pin an immutable revision and record the actual
BSD-3-Clause WITH PCRE2-exception and bundled data licenses, Unicode table version,
source closure, and build configuration in dependency records. Do not modify
upstream sources or retain another engine as a fallback.

Use a general context with Vela's allocator, a compile context with the settings
above, and operation-owned match data. Matching uses the immutable default
context; mutable work buffers use the accounted pattern allocator. Keep the pinned build's match,
depth, and heap limits explicit in GN configuration and dependency records;
they are implementation limits, and changes require regression review. Translate
`PCRE2_ERROR_MATCHLIMIT`, `PCRE2_ERROR_DEPTHLIMIT`, and `PCRE2_ERROR_HEAPLIMIT`
to a distinct limit error. They are not no-match or physical allocation failure.
Do not set unlimited budgets. These engine counters are not a wall-clock timeout.

PCRE2 receives the original UTF-8 buffer and explicit length and returns byte
offsets. There is no UTF-16 subject copy. Convert public code point starts to
byte starts and capture spans back to code point positions using one shared
boundary mapping per operation. Build it once for allMatches rather than
rescanning prefixes for every capture. The already validated internal UTF-8
contract permits `PCRE2_NO_UTF_CHECK` on repeated matching calls; it never
permits malformed external input or starts inside a code point. Check all size
and index conversions before allocation, without truncating `PCRE2_SIZE` values.
Map compile diagnostics' byte offsets back to decoded-pattern code point offsets.

RegExp owns the compiled program, original pattern, flags, and capture-name
metadata. RegExpMatch strongly retains its input and RegExp and owns immutable
capture spans. No result points into operation-owned match data, work buffers,
or borrowed arena memory. Retain byte spans as well as public coordinate
information so group extraction copies the selected UTF-8 content without
rescanning the whole prefix. A match can outlive both caller variables.

GC traces the input/pattern references and uses existing publication barriers.
Compiled programs and engine work buffers participate in heap memory accounting;
their owner releases them exactly once on failure, collection, or Isolate
destruction. Root receivers, inputs, partial result arrays, and new match objects
before allocation safepoints. hasMatch need not allocate a RegExpMatch; group
extraction allocates a string only when content is requested and cannot be reused.

| Failure | Result |
| --- | --- |
| Invalid pattern syntax or unsupported pattern constraints | `Error` with code `RegExpSyntaxError`, the PCRE2 diagnostic, and a decoded-pattern code point offset |
| Invalid search start or numeric group index | Existing `IndexOutOfBounds` path |
| Unknown named group | `Error` with code `RegExpGroupError` |
| Engine/backing allocation or representable-size exhaustion | Existing recoverable `OutOfMemory` path |
| Match-work, matching-depth, or engine-heap limit reached | `Error` with code `RegExpLimitError` |
| Other engine execution failure | Explicit `RegExpExecutionError`, not a no-match result |
| Wrong static receiver, argument, or index type | Compile-time diagnostic; dynamic native boundaries retain runtime checks |

No match is a normal false, null, or empty-array result. Engine failure is never
reported as no match, and allMatches never returns a partial array. Errors flow
through ordinary catch/finally and source traces; JSON/native failure labels
must not replace regex-specific diagnostics. A caller may reuse the native instance and
RegExp after a caught operation failure.

This is a backtracking engine and has no general linear worst-case guarantee.
The first version exposes no timeout parameter. Allocation, configured limits,
and engine failures remain explicit; performance validation includes ambiguous
quantifiers and failed searches, not just successful literal matches.

## Implementation and validation

The shared [signature registry](../../compiler/frontend/builtins/regexp_builtins.cc)
provides static checking and tooling metadata. [Runtime operations](../../runtime/regexp/regexp.cc)
own compilation, search and captures; [heap integration](../../runtime/heap/heap_regexp.cc)
owns native accounting, tracing and release. Runtime does not depend on the
compiler or tooling. Ordinary string search retains its existing semantics.

The pinned interpreter uses match limit 1,000,000, depth limit 1,000 and heap
limit 8,192 KiB. Parentheses nesting is limited to 250 and maximum variable
lookbehind to 255. Unicode 17.0.0 tables are independent of string casing Unicode
18.0.0 data. [Dependency records](../../third_party/README.md) define the source
closure, licenses and configuration. Image version 7 retains typed layout
operands; native compiled programs are recreated during execution.

[Compiler/image tests](../../compiler/language/regexp_unittest.cc) cover both types,
options, Unicode coordinates, captures, native PCRE2 constructs, method values,
containers, async payloads and errors. [Runtime tests](../../runtime/regexp/regexp_unittest.cc)
check all three resource limits, execution failures, partial-result cleanup,
match-only reachability, native accounting and hasMatch zero GC allocations.
REPL and LSP tests cover persistent ownership, completion, hover and named
arguments. The runnable [example](../../examples/regexp.vela) uses ordinary Vela
string escapes and typed results.

Release benchmarks separate pattern compilation, repeated hasMatch/firstMatch,
allMatches materialization and coordinate mapping, capture extraction, GC
pressure and backtracking limits. Aggregate results, variation and native
workload comparisons are retained in the [rolling summary](../../benchmarks/results/rolling_summary.json).

## References

- [Dart RegExp](https://api.dart.dev/dart-core/RegExp-class.html) and [RegExpMatch](https://api.dart.dev/dart-core/RegExpMatch-class.html).
- [PCRE2 API](https://www.pcre.org/current/doc/html/pcre2api.html) and [pattern syntax](https://www.pcre.org/current/doc/html/pcre2pattern.html); implementation fixtures use the pinned version.
- [PCRE2 source](https://github.com/PCRE2Project/pcre2) and [license](https://github.com/PCRE2Project/pcre2/blob/main/LICENCE.md).
- Vela [strings](strings.md), [exceptions](exceptions.md), and [garbage collection](garbage_collection.md).
