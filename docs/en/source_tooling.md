# Source tools

[**English**](source_tooling.md) | [简体中文](../zh-CN/source_tooling.md)


## Formatter

`vela fmt FILE` writes formatted source to standard output. `vela fmt --write FILE`
replaces the file atomically; `vela fmt --check FILE` exits with `1` when formatting
would change it. Syntax errors return the ordinary source diagnostics and exit `65`.
Failed I/O returns `74`. No file is rewritten after a syntax error.

The formatter uses the production lexer and parser. Tokens retain their raw byte
ranges separately from decoded literal values; the parser annotates generic type
delimiters and prefix operators. It uses two-space indentation, LF endings,
braced blocks on multiple lines, a space after type/named-argument colons,
spaces around binary operators and no spaces inside generic delimiters.
Top-level declarations are separated by a blank line; consecutive imports stay
together, and existing blank lines inside blocks are capped at one.
Long parenthesized/bracketed groups use continuation
lines at comma boundaries. This is deterministic layout, not a maximum-width
guarantee for indivisible tokens or long expressions.

Comments remain in source order, including trailing line comments and comments
inside interpolations. Single and double quotes both produce `string` values
and support `${...}` interpolation. Formatting uses single quotes, including
nested string literals inside interpolation, escapes apostrophes, and removes
unnecessary double-quote escapes. Other escape spellings and interpolation
expression layout remain unchanged. Reformatting is idempotent and preserves every language token's kind
and decoded value. Formatting neither type checks nor executes source; files with
unresolved imports or temporarily inconsistent types can still be formatted.

## API documentation

`vela doc FILE OUTPUT.md` checks the complete module graph and generates Markdown
for the entry module's exported declarations. It consumes the same immutable
source snapshots as module loading, without running global initializers or `main`.
Imports remain local to their modules and are not documented as re-exports.

Documentation comments use consecutive `///` lines immediately above declarations,
following the convention used by Dart. They can contain Markdown. Generated output
includes functions, aliases, enums, interfaces, globals, class fields and methods,
generic signatures, nullable/array/function types and named construction parameters.
Private class members are omitted. Generated constructor signatures contain only
public instance fields; explicit constructors expose their own parameter lists.
Default expressions
and executable bodies are represented by `…`, never evaluated. Inferred global
annotations remain omitted, matching their source declarations.

`examples/documented_library.vela` is a complete example:

```powershell
out/debug/vela.exe doc examples/documented_library.vela api.md
```

The in-memory `GenerateModuleDocumentation` API parses one supplied module and
does not resolve its imports. The file entry point performs full checking before
generation. Both render source-level declarations rather than internal specialized
or module-prefixed symbol names.

## Editor protocol reference

`vela lsp` runs the language server over binary stdio. The implementation follows
[LSP 3.17](https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/):
byte-counted Content-Length framing, JSON-RPC lifecycle, document overlays and
negotiated text positions. Editor functionality shares the production checker
and formatter; it does not define an alternate language grammar.

The server supports initialize/shutdown/exit, open/change/close/save,
watched-file invalidation, versioned diagnostics, completion, hover, definition lookup, document links
and document formatting. Full semantic tokens classify user types and built-in types,
including constructors, annotations, import references and generic parameters.
UTF-16 is the default protocol encoding; clients advertising UTF-8 can use
byte positions. Incremental edits are applied in order and committed atomically;
invalid edits leave text and version unchanged. Stale versions are ignored.

All open document overlays participate in each module check. Imported-file
diagnostics use the exact loaded source snapshots, including syntax errors.
Duplicate diagnostics are removed and obsolete dependency diagnostics are cleared.
Requests execute synchronously; changes recheck open module graphs rather than
maintaining an incremental semantic graph. The VS Code client coalesces typing
updates for 150 ms by default. Formatting is fixed to Vela's two-space style.

Each RPC message is limited to 64 MiB and headers to 8 KiB. Invalid JSON in a
complete frame receives a JSON-RPC parse error and the next frame remains readable;
invalid framing terminates the session. Unsupported requests return explicit errors.
Editor queries never execute script code or write source files.

`textDocument/completion` suggests declarations, visible locals and parameters,
import exports, enum values, class/interface members and built-in APIs. `.` is
the member trigger; text edits replace the complete identifier around the cursor.
Member selection uses the production checker's receiver types, including null
refinement, and filters private/static members. A query-only snapshot fills the
cursor identifier and trailing delimiters so an unfinished `object.` can be
checked without changing the document. Editor loading preserves the production
parser's complete declarations and statements alongside syntax diagnostics;
a missing semicolon no longer discards the next statement or the whole module
graph. Compilation continues to reject these errors. Incomplete declarations
that the parser cannot retain remain unavailable for semantic queries.

Within a call, `textDocument/completion` also offers unsupplied named parameters
from checked constructors, functions and methods. `(` and `,` trigger completion;
selecting `value:` inserts `value: ` without duplicating an existing colon. Parameter
candidates are sorted before ordinary symbols, while local values remain available.
Types and defaults appear in completion details. Query snapshots repair incomplete
argument slots without changing compiler diagnostics. Function values accept only
positional arguments and do not offer parameter names.

`textDocument/definition` resolves lexical binding identities, function/type/global
names, members and named arguments to exact declaration ranges, including unsaved
dependencies. Positions inside a name and immediately after its final character
resolve to the same declaration. Checked snapshots are reused for repeated definition and hover requests and
invalidated on document, save or watched-file changes. Built-in declarations
without a disk source return `null`. Diagnostics and queries share parsed module
snapshots across edits; changed overlays replace only their own cached syntax.
Repeated completions reuse an index when their repaired source is identical.
Document changes invalidate checked indices; save and watched-file events also
invalidate disk snapshots, and closing an overlay restores disk loading. A new
query snapshot still checks its module graph synchronously; semantic checking
does not incrementally update individual AST subtrees.

Import path literals and import alias declarations also resolve to the imported
file at line 1. `textDocument/documentLink` supplies resolved links over the raw
path contents, excluding quotes, so editors can underline and open the whole
path. Repeated link, hover and definition requests share one cached index.
Targets reuse the loader's canonical module identities, including
relative paths, omitted `.vela` extensions, decoded escapes, Unicode names and unsaved module overlays.
Ordinary strings and unresolved imports have no target. Default standard objects
have internal declaration sources for checking and hover, rather than disk
navigation targets.

`textDocument/hover` returns a Markdown Vela signature and adjacent `///`
documentation, with the range of the name under the cursor. It covers inferred
local/global types, parameters, functions, fields, methods, classes, interfaces,
enums, aliases and built-in APIs. Source type references are indexed before
specialization expands aliases and templates. Imported documentation comes from
the same loaded overlays as type checking. Unknown names, comments, literal text
and whitespace have no hover. The VS Code client synchronizes unsaved text,
discards cancelled or stale replies, and renders Markdown without trusted links
or HTML.

Built-in API documentation is in English; user `///` comments retain their original
text. Hover formatting was checked against TypeScript 6.0.3's
`getQuickInfoAtPosition`: enum and interface names retain nominal declaration
headers with documentation, enum members show qualified names and numeric values,
and type aliases show full definitions with generic parameters. Methods and
properties include their containing type, and parameters carry an explicit role
label. Function-type arrays and nullable function types retain their grouping.
Vela additionally preserves constructor parameters and defaults in class hover.

The [VS Code extension](../../editors/vscode/README.md) contains TextMate highlighting
and a dependency-free stdio client. `vl editor package` produces a VSIX under
`out/editor`. `vl test debug tooling` runs real-process protocol and tool tests.
The optional Node client tests use `node --test editors/vscode/extension_unittest.cjs`;
Node is provided by VS Code at runtime and is not a compiler build dependency.
`vl editor test --code PATH` additionally runs the real VS Code extension host;
it requires a VS Code installation (use `bin/code.cmd` on Windows), an isolated
temporary profile, and no npm dependencies.

The [terminal debugger](debugger.md) uses `vela debug FILE` and the VM's explicit
instruction observer; it shares source mappings with runtime diagnostics.

The [persistent REPL](repl.md) uses `vela repl`, retaining declarations and values
without replaying previous submissions.

## Source testing

`vela test` reads the nearest `vela.json` and discovers tests from explicit
paths. `--list` executes registration and lists selected tests;
`--reporter=json` emits the version 1 `vela.test` NDJSON protocol. See the
[testing contract](../zh-CN/testing.md) for APIs, exit codes, asynchronous error
ownership and timeout boundaries. `vela run FILE` automatically runs the single
suite when the program imports `sdk:test`, without project configuration.

SDK completion, hover, documentation and locations use the embedded source.
Definitions and import links retain the `sdk:test` URI. VS Code reads its
readonly content with `vela/sdkSource`, passing `{ "uri": "sdk:test" }`;
only registered SDK modules are accepted. Static editor queries never execute
test registration or test bodies.
