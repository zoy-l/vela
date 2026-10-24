# Vela examples

The examples use only syntax supported by the current bootstrap compiler. The
CLI prints the value returned by `main`; object-returning examples print
`object` because object field inspection is deliberately a runtime API concern.

```powershell
.\out\debug\vela.exe run examples\hello.vela
.\out\debug\vela.exe run examples\loop.vela
.\out\debug\vela.exe run examples\class_construction.vela
.\out\debug\vela.exe run examples\language_features.vela
.\out\debug\vela.exe run examples\imports\main.vela
```

`language_features.vela` covers loops, strict comparisons and logic, strings,
arrays, class methods, `this`, and console output. The imports example compiles
relative source files as one program.

Use `check` when validating source without executing it:

```powershell
.\out\debug\vela.exe check examples\function_call.vela
```
