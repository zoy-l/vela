# Vela

[**English**](README.md) | [简体中文](README.zh-CN.md)

Vela is a statically typed scripting language for native applications. The
repository contains the language front end, a typed intermediate representation,
a fixed-width register bytecode VM, and an isolate-local runtime. Native AOT
backends are planned for a later stage.

Vela is currently in an early stage of development, and its language and runtime
interfaces are still evolving.

Vela's language and runtime design draws on implementation ideas from Luau,
JavaScript, Dart, and other modern scripting languages.

## Usage

The `vela` executable currently provides two source-level commands:

- `vela check <source.vela>` parses and statically checks a source file without
  executing it.
- `vela run <source.vela>` compiles the source through Typed IR and register
  bytecode, then executes its zero-argument `main` function in the VM.

For example:

```powershell
.\out\debug\vela.exe check examples\function_call.vela
.\out\debug\vela.exe run examples\hello.vela
.\out\debug\vela.exe run examples\language_features.vela
```

The current language slice uses C/C++-style blocks and semicolons. It includes
integers, doubles, booleans, strings, arrays, functions, direct calls, classes with
fixed fields and methods, enums, control flow, console output, imports, and
multi-file compilation. See [`examples/`](examples/) for runnable programs and
[`docs/roadmap.md`](docs/roadmap.md) for the feature checklist.

## Installation

Vela is currently built from source. The repository manages its own compiler
and build tools.

From a fresh checkout, run dependency synchronization with the depot-tools
`gclient` launcher available on the machine:

```powershell
gclient sync --no-history
python -m tools.vl doctor
```

The sync command fetches the pinned host tools, Abseil, GoogleTest, and Google
Benchmark sources declared in [`DEPS`](DEPS). Dependency versions and licenses
are documented in [`third_party/README.md`](third_party/README.md).

## Building

The supported developer entry point is `python -m tools.vl`:

```powershell
python -m tools.vl config
python -m tools.vl build
python -m tools.vl build release
```

On Windows, the same commands are available through `bin/vl.cmd`, for example:

```powershell
.\bin\vl.cmd build
```

Debug and Release outputs are isolated from each other. Windows builds use the
repository's `clang-cl`, `lld-link`, and `llvm-lib` with Visual Studio C++
libraries and the Windows SDK. Linux and macOS builds use the repository's
Clang and LLVM tools with the platform SDK and libc++.

The project uses C++23 with C++ exceptions disabled. The build scripts resolve
GN, Ninja, Clang, LLVM archive tools, and analysis tools from
`third_party/build_tools` without modifying `PATH`.

## Testing

Vela's C++ tests use GoogleTest and cover the environment, frontend, compiler,
runtime, and VM:

```powershell
python -m tools.vl test
python -m tools.vl test list
```

Arguments after `--` are passed directly to GoogleTest:

```powershell
python -m tools.vl test debug compiler -- --gtest_list_tests
python -m tools.vl test release compiler -- --gtest_filter=CompilerTest.CompilesIntegerFunction
python -m tools.vl test debug runtime -- --gtest_repeat=10 --gtest_shuffle
```

`python -m tools.vl lint` runs the repository's clang-tidy checks. Use
`python -m tools.vl lint --fix` to apply available fixes, then review the
result before validating it.

## Benchmarks

The benchmark executable is built separately in Release mode:

```powershell
python -m tools.vl bench
python -m tools.vl bench -- --benchmark_list_tests
python -m tools.vl bench -- --benchmark_filter=^vm/ --benchmark_repetitions=5
```

Each workload measures compilation, first execution, and steady-state VM
execution separately. The benchmark suite covers integer loops, function calls,
field access, allocation and GC, and generated symbol-table workloads. See
[`benchmarks/README.md`](benchmarks/README.md) for measurement boundaries and
expected results.

## Architecture

```text
source
  -> lexer and parser
  -> name resolution and static type checking
  -> typed control-flow IR
  -> register bytecode
  -> Vela VM
```

Typed IR is the semantic boundary shared by the bytecode backend and future
native backends. The VM uses contiguous register frames and fixed-width
instructions for predictable decoding. Each `Isolate` owns its heap; the
runtime uses a precise, non-moving, incremental mark-and-sweep collector, and
VM frame slots are registered explicitly as GC root ranges.

The module responsibilities are:

- `compiler/frontend/`: lexer, parser, syntax tree, and static type checking.
- `compiler/ir/`: lowering from the checked AST to Typed IR.
- `compiler/`: bytecode encoding and the `vela` executable.
- `vm/`: bytecode validation, register frames, and interpretation.
- `runtime/`: values, strings, objects, class layouts, the heap, and Isolate.
- `examples/` and `benchmarks/`: runnable programs and repeatable workloads.

More detailed contracts are documented in
[`docs/language_architecture.md`](docs/language_architecture.md) and
[`compiler/README.md`](compiler/README.md).

## Dependencies

The build uses repository-pinned GN, Ninja, Clang, LLVM tools, Abseil,
GoogleTest, and Google Benchmark. Third-party source and license information is
listed in [`third_party/README.md`](third_party/README.md).

## License

Vela is distributed under the [MIT License](LICENSE).
