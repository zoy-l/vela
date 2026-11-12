# Vela toolchain contract

[**English**](toolchain.md) | [简体中文](../zh-CN/toolchain.md)

`DEPS` is the single dependency declaration. Its CIPD entries materialize
host-specific GN and Ninja packages under `third_party/build_tools/<tool>/<host-platform>`.
Its `llvm_sdk` hook downloads the upstream LLVM 23.1.5 release archive for the host,
verifies the pinned SHA-256, and installs the complete package under
`third_party/build_tools/llvm_toolchain/23.1.5/<host-platform>`.
Clang, LLD, LLVM archive tools, clang-tidy, clang-format, clangd, runtime resources,
and the LLVM/LibTooling development libraries all come from this package.
These paths are ignored by Git because they are generated checkout state.
`python -m tools.vl llvm sync` runs the same LLVM acquisition for an existing checkout.

`tools/config.py` resolves those paths from the repository root and passes
absolute paths into GN arguments. The generated toolchain uses the same paths
for compilation, archiving, linking, and formatting. This keeps Debug and
Release outputs separate and avoids ambient tool discovery.

`python -m tools.vl build release` enables ThinLTO for C and C++ compilation,
including the runtime and third-party sources built by GN. Debug builds keep
native object files. The `vela_thin_lto` GN argument records this choice, and
each Release output directory stores incremental link results in
`thinlto-cache/`.

Clang and LLD use the same pinned release to keep ThinLTO bitcode production and
consumption aligned. Builds and native distribution packages use that release's
LLVM archiver and LLD linker. macOS uses the Xcode SDK with the pinned `ld64.lld` linker.

The macOS SDK comes from the system's Xcode or Command Line Tools installation.
`xcrun` resolves the SDK selected by `SDKROOT`, or `macosx` when it is unset.
Use `SDKROOT=/path/to/MacOSX.sdk python3 -m tools.vl test` to select an installed
SDK compatible with the pinned toolchain. Use the same selection for builds,
tests, and lint.

The supported bootstrap hosts are Windows x64, Linux x64, and macOS arm64.
Adding another host requires matching LLVM release archive and SHA-256 entries,
GN/Ninja CIPD entries, and the corresponding GN toolchain flags in the same change.

The `vela` target provides source checking, JIT execution, native AOT builds,
JIT development sessions, testing, and source tooling commands. See
[language architecture](architecture.md) for language and runtime contracts,
and [AOT contracts](aot.md) for native distribution and platform SDK acceptance.
