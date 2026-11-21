# Vela for Visual Studio Code

Adds `.vela` syntax highlighting, comments/brackets, code completion, go to
definition, type-parameter rename, hover information, diagnostics, document formatting and file execution. Semantic features use the native
`vela lsp` server and its production compiler frontend. The extension has no npm dependencies.

Build Vela, then package the extension from the repository root:

```powershell
python -m tools.vl build
python -m tools.vl editor package
```

In VS Code, run **Extensions: Install from VSIX...** and select the generated
`out/editor/vela-language-0.1.23.vsix`. Open the Vela workspace. The default server
is `out/debug/vela.exe` on Windows or `out/debug/vela` elsewhere. Set
`vela.serverPath` to use another build or an installed executable; it must expose
the `lsp` and `run` commands. No global PATH changes are needed.

Open a workspace folder, then click the play button in the editor title bar, choose **Vela: Run File** from
the command palette, or use the editor context menu to run the current saved
`.vela` file. Run File saves pending edits to that file, then executes `vela run FILE`
using `vela.serverPath`. Imported files are read from disk; save their changes before
running. Output, diagnostics and the process exit code appear in a VS Code task terminal.
Use **Tasks: Terminate Task** or the task terminal's stop control to stop the program.
The working directory is the file's workspace folder, or its parent directory for
a file outside the workspace.

**Format Document** uses two-space indentation, single-quoted strings and a
100-column target. Groups break according to their available line width and
comments. Multiline argument, parameter, array, map, enum and match lists use
trailing commas. Empty blocks stay compact. Operators use syntax-aware spacing,
and declarations are separated by blank lines.
Both quote styles support interpolation. Changes are sent after a
short configurable delay (`vela.diagnosticsDelay`, default 150 ms). Import and
module file changes trigger diagnostics as well. **Vela: Show Language
Server Output** displays server startup errors and logs.

Use **Alt+/** or **Ctrl+Space** for completion, or type `.` after an object, type
or import prefix. The Vela-specific **Alt+/** binding also works when the input
method intercepts **Ctrl+Space**. **F12** and **Ctrl+Click** jump to declarations, including imported files.
On an `import` path or its alias declaration, they open the imported local file.
Import paths use VS Code's native document links for underlining and opening;
keep **Editor: Links** enabled. `import 'math';` resolves to `math.vela`.
Queries synchronize unsaved text immediately. **F2** renames a type parameter
and its bound/signature/body references while keeping shadowed parameters separate.
Conflicting names are rejected. Completion respects lexical scope, private/static
members and nullable type refinement. After installing an updated
VSIX or rebuilding the server, run **Developer: Reload Window**.

Hover over a name to see its type or function signature and adjacent `///`
documentation. Imported symbols and unsaved declarations use the same checked
snapshot as navigation. **Ctrl+K Ctrl+I** also opens the hover from the keyboard.

Class hover shows constructor parameters and literal defaults. Built-in API documentation is in English and shows
usage, parameters, return values, units, boundary conditions and examples where useful;
the same documentation appears in completion details.

Type names use the active theme's type colors in member and parameter annotations,
return types, inheritance clauses, generic arguments, and after `is` and `as`.
The language server distinguishes classes, interfaces, enums, enum members, aliases and type
parameters, including imported names and constructor calls.

Unused parameters and imports produce editor warnings and fade their source ranges.
Parameters whose names start with `_` express intentional non-use. Interface and
external signatures have no implementation to inspect. Type-only references,
default expressions and closure captures count as uses.

Hover follows TypeScript's display conventions: enum and interface names show their
declaration headers and documentation; enum members include their owner and value;
type aliases include generic parameters and their complete definitions. Methods,
properties and parameters are labeled by role, with qualified member names.

Typing `(` or `,` in a call opens code completion for named arguments, including
generated field constructors and explicit constructors. Selecting `value:` inserts
`value: `; supplied arguments are omitted. Typing a prefix filters parameter names,
and their details show types and defaults. **Ctrl+Space** opens completion manually.

VS Code installs extensions separately for each profile. If a named profile is
active, install the VSIX in that profile, for example
`code --profile PROFILE --install-extension PATH_TO_VSIX --force`.

For another editor, configure `vela lsp` as a stdio LSP server for `.vela` files.
It supports UTF-8/UTF-16 positions, document open/change/close/save, diagnostics,
watched-file invalidation, completion, hover, definition lookup, document links and document formatting.

Run the real VS Code extension-host tests with
`python -m tools.vl editor test --code PATH_TO_VSCODE_LAUNCHER`.
On Windows use the installation's `bin/code.cmd` launcher. Tests use an isolated
profile and verify `examples/exceptions.vela`, F12 at word boundaries, and
completion/navigation with unsaved statements. They never edit the example file.

`.vela` files use a sail mark, with separate SVG palettes for light and
dark themes. Icons are registered with the language and work with file icon
themes that display language icons, including Seti and Material Icon Theme.
