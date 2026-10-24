# Vela

Vela is a new statically typed scripting language workspace. The repository
contains the native build environment, a static front end, and a bootstrap
bytecode runtime; later native backends are still being built.

The project follows a small GN/Ninja layout:

- `compiler/` contains the bootstrap executable and its build identity.
- `compiler/ir/` contains the typed register IR and source lowering path.
- `vm/` contains the fixed-width register VM and frame root integration.
- `examples/` contains runnable source examples for the supported language slice.
- `benchmarks/` contains repeatable VM workloads and benchmark notes.
- `build/` contains the GN configuration and project toolchain.
- `tools/` contains the `vl` developer commands.
- `third_party/build_tools/` is populated only by the versions pinned in
  `DEPS`.

The toolchain is repository managed. Build scripts resolve GN, Ninja, Clang,
LLVM archive tools, and analysis tools from `third_party/build_tools`, using the
same pinned Clang revision as VerseGame. They do not modify `PATH` or use a
system C/C++ compiler.

On Windows, builds use `clang-cl`, `lld-link`, and `llvm-lib`, with Visual Studio
C++ libraries and the Windows SDK. Debug uses `/MDd /Od /Z7`, `_DEBUG`, and PDB
output; Release uses `/MD /O2` and `NDEBUG`. Both use C++23 and disable C++
exceptions. Ninja tracks headers through `/showIncludes` and MSVC dependency
format. Test and benchmark targets use the same compiler and CRT configuration.

Linux uses project Clang, LLD, LLVM `ar`, and libc++. macOS uses project Clang
and LLVM `ar`, with the Xcode SDK, libc++, and Xcode linker. On these platforms,
Ninja's `gcc` dependency format describes Clang's Make-format dependency output;
it does not invoke GCC. Dependencies include system and third-party headers.

## First checkout

Run the dependency sync from the Vela checkout with the depot-tools `gclient`
launcher available on the machine:

```powershell
gclient sync --no-history
```

The command materializes the pinned host tools, Abseil, GoogleTest, and Google Benchmark
source declared in `DEPS`. No external engine checkout is required by the Vela
build after synchronization. Dependency versions and licenses are documented in
[`third_party/README.md`](third_party/README.md).

Compiler symbol tables, scope tables, keywords, and import tracking use Abseil
`flat_hash_map` / `flat_hash_set`. String-keyed tables support lookup by
`std::string_view` without constructing an owning string. Declaration tables are
built before checking function bodies and stay immutable during lowering, so
borrowed descriptors are not invalidated by table growth.

## Configure, build, and test

```powershell
python -m tools.vl doctor
python -m tools.vl config
python -m tools.vl build
python -m tools.vl clangd
python -m tools.vl lint
python -m tools.vl test
python -m tools.vl bench
```

`vl lint` reports warnings as failures. Use `vl lint --fix` to apply clang-tidy
code fixes, then review and validate the result. The check configuration remains
in `.clang-tidy`; project headers included by the selected translation units are
checked as well, with third-party code excluded. Formatting also excludes vendored
source trees. `vl test` executes the environment, frontend, compiler, runtime, and
VM GoogleTest suites; use `vl test list` to list individual selections. Test source
files use `*_unittest.cc` and link the upstream `gtest_main`. Each case reports its own
result, with assertion locations and expected/actual values on failure. GoogleTest
is only linked into test executables.

Arguments after `--` go directly to GoogleTest:

```powershell
python -m tools.vl test debug compiler -- --gtest_list_tests
python -m tools.vl test release compiler -- --gtest_filter=CompilerTest.CompilesIntegerFunction
python -m tools.vl test debug runtime -- --gtest_repeat=10 --gtest_shuffle
```

On Windows the same entry point is available as `bin/vl.cmd`, so the commands
can also be run as `./bin/vl.cmd doctor`, `./bin/vl.cmd build`,
`./bin/vl.cmd clangd`, and `./bin/vl.cmd test` without installing a global
command.

Release output uses an isolated directory:

```powershell
python -m tools.vl build release
```

The executable currently exposes the bootstrap identity and the first static
checker:

```powershell
.\out\debug\vela.exe --version
.\out\debug\vela.exe --help
.\out\debug\vela.exe check path\to\source.vela
.\out\debug\vela.exe run path\to\source.vela
```

`run` currently executes a zero-argument `main` function. The current language
boundary and performance contract are documented in
[`docs/language_architecture.md`](docs/language_architecture.md); implementation
details are listed in [`compiler/README.md`](compiler/README.md).
`python -m tools.vl bench` builds a separate Google Benchmark executable in
Release mode. It measures compilation, first execution, and steady VM execution
for the four workloads, plus compilation of generated symbol-table workloads at
three sizes, using upstream iteration calibration and statistics.
Arguments after `--` go directly to Google Benchmark:

```powershell
python -m tools.vl bench -- --benchmark_list_tests
python -m tools.vl bench -- --benchmark_filter=^vm/ --benchmark_repetitions=5
python -m tools.vl bench -- --benchmark_out=out/benchmark.json --benchmark_out_format=json
```

Measurement boundaries and expected results are documented in
[`benchmarks/README.md`](benchmarks/README.md). The language feature checklist is
in [`docs/roadmap.md`](docs/roadmap.md).
