# Vela for Visual Studio Code

Adds `.vela` syntax highlighting, comments/brackets, diagnostics and document
formatting. Diagnostics and formatting use the native `vela lsp` server and its
production compiler frontend. The extension has no npm dependencies.

Build Vela, then package the extension from the repository root:

```powershell
python -m tools.vl build
python -m tools.vl editor package
```

In VS Code, run **Extensions: Install from VSIX...** and select the generated
`out/editor/vela-language-0.1.0.vsix`. Open the Vela workspace. The default server
is `out/debug/vela.exe` on Windows or `out/debug/vela` elsewhere. Set
`vela.serverPath` to use another build or an installed executable; it must expose
the `lsp` command. No global PATH changes are needed.

**Format Document** uses Vela's two-space formatter. Changes are sent after a
short configurable delay (`vela.diagnosticsDelay`, default 150 ms). Import and
module file changes trigger diagnostics as well. **Vela: Show Language
Server Output** displays server startup errors and logs.

For another editor, configure `vela lsp` as a stdio LSP server for `.vela` files.
It supports UTF-8/UTF-16 positions, document open/change/close/save, diagnostics,
watched-file invalidation and document formatting.
