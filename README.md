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
LLVM `lib`, and `clang-format` from `third_party/build_tools`; they do not
read a system installation and do not modify `PATH`.

## First checkout

Run the dependency sync from the Vela checkout with the depot-tools `gclient`
launcher available on the machine:

```powershell
gclient sync --no-history
```

The command materializes the pinned host tools declared in `DEPS`. No external
engine checkout is required by the Vela build after synchronization.

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
.\out\release\vela.exe bench benchmarks\integer_loop.vela 1000
```

`run` currently executes a zero-argument `main` function. The current language
boundary and performance contract are documented in
[`docs/language_architecture.md`](docs/language_architecture.md); implementation
details are listed in [`compiler/README.md`](compiler/README.md). The benchmark
command compiles once and reports first execution separately from steady-state
repeated execution; `python -m tools.vl bench` builds the selected variant with
the project-local toolchain before running all files under `benchmarks/`. The
language feature checklist is in [`docs/roadmap.md`](docs/roadmap.md).
