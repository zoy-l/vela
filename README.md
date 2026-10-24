# Vela

Vela is a new statically typed scripting language workspace. The repository
now contains the native build environment and the first static front-end slice;
the bytecode runtime and later native backends are still being built.

The project follows a small GN/Ninja layout:

- `compiler/` contains the bootstrap executable and its build identity.
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
python -m tools.vl test
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
```

The current language boundary and performance contract are documented in
[`docs/language_architecture.md`](docs/language_architecture.md); implementation
details are listed in [`compiler/README.md`](compiler/README.md).
