# Vela compiler workspace

This directory contains the executable, typed IR lowering, and build identity
for the bootstrap compiler. The language and runtime contracts are recorded in
[`docs/language_architecture.md`](../docs/language_architecture.md); the
bootstrap executable remains intentionally smaller than that target.

The frontend, compiler, and VM stay under the same GN/Ninja target boundary so
later optimizer and native backend work does not change the developer entry
points:

- `//compiler:vela` is the user-facing executable.
- `//compiler:build_info` is the small shared build identity target.
- `//compiler:frontend` contains the lexer, parser, and static checker.
- `//runtime:runtime` contains the isolate-owned object heap and collector
  contract used by the VM.
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

The bootstrap compiler path can execute the currently supported integer and
fixed-layout class subset:

```powershell
.\out\debug\vela.exe run path\to\source.vela
```

`run` selects a zero-argument `main` function, lowers the checked AST through
Typed IR, emits register bytecode, and executes it in the VM. Function calls,
control-flow branches, and loops are supported in this bootstrap slice; arrays
and strings remain outside it.

It covers the initial C-style block syntax, enum declarations, class fields,
aggregate construction with named arguments, functions, and static type
checking. The checker feeds the typed IR and bytecode VM; it does not interpret
source directly.

The executable also has a repeatable VM benchmark mode:

```powershell
.\out\release\vela.exe bench benchmarks\integer_loop.vela 1000
```

For the complete benchmark set, use `python -m tools.vl bench`. Compilation is
performed once per source file, then the first execution and the remaining
steady-state executions are timed separately.
