# Vela for Visual Studio Code

Adds `.vela` syntax highlighting, comments/brackets, code completion, go to
definition, hover information, diagnostics and document formatting. Semantic features use the native
`vela lsp` server and its production compiler frontend. The extension has no npm dependencies.

Build Vela, then package the extension from the repository root:

```powershell
python -m tools.vl build
python -m tools.vl editor package
```

In VS Code, run **Extensions: Install from VSIX...** and select the generated
`out/editor/vela-language-0.1.10.vsix`. Open the Vela workspace. The default server
is `out/debug/vela.exe` on Windows or `out/debug/vela` elsewhere. Set
`vela.serverPath` to use another build or an installed executable; it must expose
the `lsp` command. No global PATH changes are needed.

**Format Document** uses Vela's two-space formatter with single-quoted strings,
spaces after colons and around operators, and blank lines between declarations.
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
Queries synchronize unsaved text immediately. Completion respects lexical scope,
private/static members and nullable type refinement. After installing an updated
VSIX or rebuilding the server, run **Developer: Reload Window**.

Hover over a name to see its type or function signature and adjacent `///`
documentation. Imported symbols and unsaved declarations use the same checked
snapshot as navigation. **Ctrl+K Ctrl+I** also opens the hover from the keyboard.

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
