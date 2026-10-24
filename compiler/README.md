# Vela compiler workspace

This directory currently contains the executable and build identity needed to
validate the native toolchain. The language and runtime contracts are recorded
in [`docs/language_architecture.md`](../docs/language_architecture.md); the
bootstrap executable is still intentionally smaller than that target.

The frontend is kept under the same target boundary so later IR and VM work
does not change the GN/Ninja entry points:

- `//compiler:vela` is the user-facing executable.
- `//compiler:build_info` is the small shared build identity target.
- `//compiler:frontend` contains the lexer, parser, and static checker.
- `//runtime:runtime` contains the isolate-owned object heap and collector
  contract used by the future VM.
- `//vm:vm` contains the fixed-width register VM bootstrap and frame root
  integration.
- `//compiler:vela_environment_tests` validates that the target links and its
  build identity is available to consumers.
- `//compiler:vela_frontend_tests` covers the first source-level contracts.
- `//runtime:runtime_tests` covers object reachability, cycles, write barriers,
  and size-class reuse.
- `//vm:vm_tests` covers typed arithmetic, bytecode validation, and register
  roots surviving collection.

The first frontend slice is now available through:

```powershell
.\out\debug\vela.exe check path\to\source.vela
```

It covers the initial C-style block syntax, enum declarations, class fields,
aggregate construction with named arguments, functions, and static type
checking. The bytecode VM is the next execution layer; this checker does not
interpret source directly.
