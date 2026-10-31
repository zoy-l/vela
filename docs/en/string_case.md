# String case conversion

[**English**](string_case.md) | [简体中文](../zh-CN/string_case.md)

Vela provides Dart-style `toLowerCase()` and `toUpperCase()` instance methods on
immutable `string`. Both use locale-independent Unicode default case conversion,
including multi-code-point mappings and language-independent contextual rules.
They return strings and do not modify the receiver.

```vela
func main(): void {
  print('Vela'.toLowerCase());   // vela
  print('Vela'.toUpperCase());   // VELA
  print('straße'.toUpperCase()); // STRASSE
  print('ΟΣ'.toLowerCase());     // ος
  print('İ'.toLowerCase());      // i followed by U+0307
}
```

The first version does not add locale-specific casing, case folding,
titlecasing, normalization, in-place builder conversion, or case-insensitive
comparison APIs. These methods require neither dynamic nor JSON. The
[RegExp API](regexp.md) uses a separate ignore-case comparison contract.

## API and observable behavior

| API | Contract |
| --- | --- |
| `text.toLowerCase(): string` | Unicode default lowercase conversion |
| `text.toUpperCase(): string` | Unicode default uppercase conversion |

The receiver must be a non-null string; nullable receivers use existing
null-safe access or refinement. Neither method takes arguments. Wrong static
receivers or arguments are diagnosed by the frontend. Checked builtin calls
also validate runtime operand types. Results are ordinary immutable UTF-8 strings usable
in fields, arrays, captures, and native string calls.

Apply the Unicode default conversion for pinned Unicode 18.0.0:
unconditional full mappings plus language-independent contextual mappings,
including Final_Sigma. Do not apply Turkish, Azerbaijani, Lithuanian, or other
locale-specific rules. In particular, default lowercase I becomes i, while
uppercase I with dot becomes i followed by a combining dot.

One input code point may produce multiple output code points. Output byte length
and code point length may both change, so these methods are not index-preserving
transformations. Empty input stays empty; uncased characters, supplementary
characters without a mapping, combining marks without a mapping, emoji, and
embedded NUL are preserved. Contextual rules inspect Unicode Cased and
Case_Ignorable properties, not ASCII-only word boundaries.

No normalization or accent stripping is performed. Full conversion can produce
a decomposed sequence, and it must remain decomposed. Lower/upper conversion
is not an invertible encoding: uppercasing and then lowercasing need not restore
the original text. Repeating the same conversion is idempotent for the pinned
mapping data. If no content changes, an implementation may return the original
immutable string; object allocation identity is not part of the API contract.

RegExp ignore-case matching must not implement comparison by converting the
whole input with these methods. Expansions change offsets and do not implement
the RegExp API's caseless matching rules.

## Unicode data and implementation

Use the existing utf8proc dependency for simple mappings and add generated
Unicode full/contextual mapping data. The casing operation is independent of
the regex engine. The existing [Unicode operations](../../runtime/strings/unicode_operations.cc)
continue to use utf8proc for code points and grapheme segmentation.

Generate the supplement from the same Unicode version used by the pinned
utf8proc build: unconditional and language-independent conditional entries from
SpecialCasing.txt, plus Cased and Case_Ignorable ranges from
DerivedCoreProperties.txt. Use full/contextual entries where defined and
utf8proc's public simple-mapping functions for the remaining code points. Apply
Final_Sigma against the original input context. Locale-tagged mappings are
excluded by the default-casing contract. A simple per-code-point loop alone
does not implement full conversion.

Manage the UCD inputs as immutable, checksummed source dependencies through
the repository's dependency workflow. Provide a deterministic offline generator
using the project Python toolchain and GN integration; generation must not
depend on a host Unicode database, moving latest URLs, or network access during
a build. Do not read utf8proc's private sequence tables or modify upstream
sources. Keep data/schema validation and regeneration checks with the generator,
not in each runtime conversion.

Record the UCD release, input identities, generated output, generator, and
Unicode data license in the existing dependency/build records. Require the
supplement and utf8proc Unicode versions to agree. The PCRE2-backed
RegExp engine pins its Unicode tables separately; they need not be identical because they implement regex
matching, not this conversion API. A Unicode update is observable, participates
in build/cache identity, and requires fixture review. Never select mappings
from the host OS, browser, or locale or download data at runtime. Host C/C++
locale functions and utf8proc's simple mappings alone cannot substitute for
the full algorithm.

Use shared [UTF-8 primitives](../../base/utf8.h) on the already validated runtime
string. Determine mapped content, context, and checked output size in linear
scans, then allocate the final heap string once and write valid UTF-8 directly.
Do not repeatedly rescan prefixes for each contextual character, allocate
per-character strings, or concatenate intermediate strings. Keep the receiver
rooted across allocation, and write the destination register only after success,
including when source and destination registers alias.

Overflow or a result exceeding representable heap storage follows the existing
recoverable OutOfMemory path. An allocation failure does not alter the receiver
or publish a partially initialized string. Input validation remains at the
existing external boundaries; conversion does not repeat full UTF-8 validation
for every internal call. Unicode table and algorithm invariants are internal
development contracts, not user-triggerable fatal checks.

## Integration and validation

The methods use checked string builtin signatures shared by the frontend, IR,
VM, and tooling. Method values, member completion, hover, documentation,
formatting, debugger queries, and persistent REPL use follow the existing
string-method paths. Bytecode images validate builtin IDs and operands;
cache identity includes the generator and pinned input manifest.

`DEPS` synchronizes checksummed UCD inputs through
[`tools/unicode_case.py`](../../tools/unicode_case.py). GN generates
`gen/runtime/unicode_case_data.h` offline for each output directory.
[`third_party/unicode_data/manifest.json`](../../third_party/unicode_data/manifest.json)
records input identities and the Unicode data license. Run
[`examples/string_case.vela`](../../examples/string_case.vela) for the public API.

Tests cover ASCII, empty/already-converted text, uncased scripts, sharp
s expansion, dotted I expansion, sigma in final/non-final positions, sigma with
Case_Ignorable context, supplementary-plane mappings, combining sequences,
NUL, non-normalization, idempotence, receiver immutability, and wrong-argument
diagnostics. Unicode fixtures must come from the pinned mapping data, not host
locale expectations. Verify deterministic regeneration, version agreement, full
mapping coverage, and contextual-property ranges. Stress allocation, output-size
checks, GC during conversion,
aliased VM registers, typed containers, fresh-Isolate images, and REPL reuse.

Release benchmarks measure unchanged ASCII, changing ASCII, non-ASCII mappings,
expanding mappings, and contextual runs at three input sizes. Measurement
boundaries and retained results are documented in
[the benchmark guide](../../benchmarks/README.md).

On the recorded Windows x64 Ryzen 9 7900X Release run, 16,384 input bytes took
302 µs for unchanged ASCII (zero first-call GC bytes), 582 µs for changing ASCII,
and 311 µs for simple Unicode mappings. The 20,480-byte contextual case took
612 µs; the 8,192-byte expanding case produced 24,576 bytes in 81 µs. Across
256 and 4096 input units, time increased approximately with input size. These
measurements include VM execution and GC; counters exclude native allocation
overhead. All cases retained zero additional heap bytes after collection.
Exact medians, variation, sizes, and machine/toolchain settings are in
[`rolling_summary.json`](../../benchmarks/results/rolling_summary.json).

## References

- [Dart toLowerCase](https://api.dart.dev/dart-core/String/toLowerCase.html) and [toUpperCase](https://api.dart.dev/dart-core/String/toUpperCase.html).
- [Unicode default case conversion](https://www.unicode.org/versions/latest/core-spec/chapter-3/) and [SpecialCasing data format](https://www.unicode.org/Public/UCD/latest/ucd/SpecialCasing.txt); implementation fixtures use the pinned version rather than these moving reference URLs.
- [utf8proc public API](../../third_party/utf8proc/utf8proc.h) and [Unicode data generation inputs](../../third_party/utf8proc/data/Makefile).
- Vela [strings](strings.md), [regular expressions](regexp.md), and [development workflow](../../.agents/development_workflow.md).
