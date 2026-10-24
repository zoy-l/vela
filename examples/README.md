# Vela examples

The examples use only syntax supported by the current bootstrap compiler. The
CLI prints the value returned by `main`; object-returning examples print
`object` because object field inspection is deliberately a runtime API concern.

```powershell
.\out\debug\vela.exe run examples\hello.vela
.\out\debug\vela.exe run examples\loop.vela
.\out\debug\vela.exe run examples\class_construction.vela
.\out\debug\vela.exe run examples\language_features.vela
.\out\debug\vela.exe run examples\typed_arrays.vela
.\out\debug\vela.exe run examples\double.vela
.\out\debug\vela.exe run examples\string_gc.vela
.\out\debug\vela.exe run examples\strings.vela
.\out\debug\vela.exe run examples\string_builder.vela
.\out\debug\vela.exe run examples\imports\main.vela
```

`language_features.vela` covers loops, strict comparisons and logic, strings,
arrays, class methods, `this`, and console output. The imports example compiles
relative source files as one program.

`typed_arrays.vela` demonstrates integer, boolean, string, enum, class, and
nested arrays, contextual empty literals, array parameters and returns, and
shared storage on assignment. It prints `second`, `true`, `42`, then the CLI
prints the returned `42`. Element types match exactly; implicit conversions
are not supported.

`double.vela` uses decimal and exponent literals, double parameters and returns,
class fields and methods, and native double arrays. It prints `25`, `true`,
then returns `14.5`. Integer and double operands cannot be mixed implicitly.

`string_gc.vela` keeps strings in an object and array while creating temporary
concatenations. It prints `Vela`, `Vela GC`, then returns `true`.

`strings.vela` exercises escapes, interpolation, explicit conversions, Unicode
APIs, `join`, and imports. `string_builder.vela` builds text in a loop and keeps
an immutable snapshot. See [string contracts](../docs/strings.md).

Use `check` when validating source without executing it:

```powershell
.\out\debug\vela.exe check examples\function_call.vela
```
