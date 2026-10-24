# Vela examples

The examples use only syntax supported by the current bootstrap compiler. The
CLI prints the value returned by `main`; object-returning examples print
`object` because object field inspection is deliberately a runtime API concern.

```powershell
.\out\debug\vela.exe run examples\hello.vela
.\out\debug\vela.exe run examples\loop.vela
.\out\debug\vela.exe run examples\class_construction.vela
```

Use `check` when validating source without executing it:

```powershell
.\out\debug\vela.exe check examples\function_call.vela
```
