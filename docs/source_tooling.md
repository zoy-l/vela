# Source tools

## Formatter

`vela fmt FILE` writes formatted source to standard output. `vela fmt --write FILE`
replaces the file atomically; `vela fmt --check FILE` exits with `1` when formatting
would change it. Syntax errors return the ordinary source diagnostics and exit `65`.
Failed I/O returns `74`. No file is rewritten after a syntax error.

The formatter uses the production lexer and parser. Tokens retain their raw byte
ranges separately from decoded literal values; the parser annotates generic type
delimiters and prefix operators. It uses two-space indentation, LF endings,
braced blocks on multiple lines, spaces around binary operators and no spaces
inside generic delimiters. Long parenthesized/bracketed groups use continuation
lines at comma boundaries. This is deterministic layout, not a maximum-width
guarantee for indivisible tokens or long expressions.

Comments remain in source order, including trailing line comments and comments
inside interpolations. String spelling, escapes and interpolation interiors stay
verbatim. Reformatting is idempotent and preserves every language token's kind
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
Private class members are omitted; private `param` fields still appear as constructor
parameters because they belong to the public call signature. Default expressions
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
watched-file invalidation, versioned diagnostics and document formatting.
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
Diagnostics and formatting never execute script code or write source files.

The [VS Code extension](../editors/vscode/README.md) contains TextMate highlighting
and a dependency-free stdio client. `vl editor package` produces a VSIX under
`out/editor`. `vl test debug tooling` runs real-process protocol and tool tests.
The optional Node client tests use `node --test editors/vscode/extension_unittest.cjs`;
Node is provided by VS Code at runtime and is not a compiler build dependency.

The [terminal debugger](debugger.md) uses `vela debug FILE` and the VM's explicit
instruction observer; it shares source mappings with runtime diagnostics.

The [persistent REPL](repl.md) uses `vela repl`, retaining declarations and values
without replaying previous submissions.
