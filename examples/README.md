# Vela examples

The examples use syntax supported by the current compiler. Use `print` to display
values. An integer returned by `main` becomes the process exit code; `void` means
success. The CLI also waits for an async main's Future and uses its result as the
exit code. The examples in the table below all exit with code `0`.

## Operators, iteration, classes, and async

Each file runs independently. Comments next to the expressions show their expected
output and where evaluation stops.

| Example | Features | Expected output (one value per line) |
| --- | --- | --- |
| [compound_assignment.vela](compound_assignment.vela) | All eleven compound assignments, prefix/postfix `++` and `--`, and an index evaluated once | `9`, `9`, `11`, `11`, `9`, `15`, `1` |
| [collection_iteration.vela](collection_iteration.vela) | Read-only `const` bindings for arrays and Sets; Map `[key, value]` destructuring | `60`, `Vela`, `Dart`, `Vela: 42`, `Dart: 7` |
| [bitwise.vela](bitwise.vela) | 64-bit `~`, `&`, `\|`, `^`, `<<`, `>>`, and `>>>` | `-6`, `1`, `7`, `6`, `20`, `-2`, `4611686018427387902`, `1099511627776` |
| [conditional.vela](conditional.vela) | Right-associative `? :` and evaluation of only the selected branch | `high`, `pass`, `retry`, `selected`, `42` |
| [null_safe.vela](null_safe.vela) | `?.`, `?.[]`, and `??`; skipped method calls, index expressions, and default expressions | `fallback`, `empty`, `Vela`, `no label`, `label: Vela`, `7`, `index evaluated`, `42` |
| [inheritance.vela](inheritance.vela) | Single inheritance, implicit and explicit `super(...)`, two interfaces, inherited fields, and `override` | `2`, `40`, `2`, `42`, `42`, `bonus counter` |
| [accessors.vela](accessors.vela) | Getter/setter property access and a computed read-only property | `10`, `21`, `42` |
| [async_tasks.vela](async_tasks.vela) | Async main, `await`, `sleep`, `Future.wait`, `Future.value`, and `Future.microtask` | `42`, `7`, `8` |
| [numeric_parse.vela](numeric_parse.vela) | Decimal/hexadecimal integers, nullable tryParse, and decimal exponents | `42`, `255`, `0`, `125` |
| [string_case.vela](string_case.vela) | Unicode full mappings, expansion, and contextual lowercasing | `vela`, `VELA`, `STRASSE`, `ος`, `i̇` |
| [time.vela](time.vela) | Explicit-offset parsing, canonical UTC output, readonly calendar parts, and reconstruction | `2026-10-26T04:30:45.123Z`, `true`, `1970-01-01T00:00:00.000Z` |
| [byte_data.vela](byte_data.vela) | Fixed byte storage, unaligned shared views, integer/float access, byte order, and explicit copies | `42`, `1.25`, `42`, `3` |

For example:

```powershell
.\out\release\vela.exe run examples\compound_assignment.vela
.\out\release\vela.exe run examples\collection_iteration.vela
.\out\release\vela.exe run examples\bitwise.vela
.\out\release\vela.exe run examples\conditional.vela
.\out\release\vela.exe run examples\null_safe.vela
.\out\release\vela.exe run examples\inheritance.vela
.\out\release\vela.exe run examples\accessors.vela
.\out\release\vela.exe run examples\async_tasks.vela
```

## Other examples

```powershell
.\out\debug\vela.exe run examples\hello.vela
.\out\debug\vela.exe run examples\loop.vela
.\out\debug\vela.exe run examples\class_construction.vela
.\out\debug\vela.exe run examples\language_features.vela
.\out\debug\vela.exe run examples\typed_arrays.vela
.\out\debug\vela.exe run examples\dynamic_arrays.vela
.\out\debug\vela.exe run examples\double.vela
.\out\debug\vela.exe run examples\string_gc.vela
.\out\debug\vela.exe run examples\strings.vela
.\out\debug\vela.exe run examples\string_builder.vela
.\out\debug\vela.exe run examples\switch.vela
.\out\debug\vela.exe run examples\nullable.vela
.\out\debug\vela.exe run examples\imports\main.vela
```

`language_features.vela` covers loops, strict comparisons and logic, strings,
arrays, class methods, `this`, and console output. The imports example compiles
relative source files as one program.

`typed_arrays.vela` demonstrates integer, boolean, string, enum, class, and
nested arrays, contextual empty literals, array parameters and returns, and
shared storage on assignment. It prints `second`, `true`, `42`, then exits with
code `42`. Element types match exactly; implicit conversions
are not supported.

`double.vela` uses decimal and exponent literals, double parameters and returns,
class fields and methods, and native double arrays. It prints `25`, `true`,
then returns `14.5`. Integer and double operands cannot be mixed implicitly.

`string_gc.vela` keeps strings in an object and array while creating temporary
concatenations. It prints `Vela`, `Vela GC`, then returns `true`.

`strings.vela` exercises escapes, interpolation, explicit conversions, Unicode
APIs, array `.join(separator)`, primitive formatting, and imports. `string_builder.vela` builds text in a loop and keeps
an immutable snapshot. See [string contracts](../docs/en/strings.md).

`switch.vela` demonstrates enum cases, explicit branch exits, and loop control; it prints
`close` and returns `121`. `nullable.vela` demonstrates `T?`, guard checks,
nullable fields and array elements; it prints `label: Vela`, `empty`, and returns
`42`. See [basic language contracts](../docs/en/language_basics.md).

`array_algorithms.vela` demonstrates array callbacks and ordered Set operations
without standard-module imports. `standard_library.vela` uses default `Math`,
`Random`, `performance` and `Date` objects.

Use `check` when validating source without executing it:

```powershell
.\out\debug\vela.exe check examples\function_call.vela
```
