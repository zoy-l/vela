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
native object files. The `vela_thin_lto` GN argument records this compilation
choice. Both Debug and Release output directories store incremental link
results in their own `thinlto-cache/`: the prebuilt LLVM SDK can contain
ThinLTO bitcode independently of the project compilation mode. This also
caches SDK backend results when linking Debug executables and tests.

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

## GitHub builds and releases

The `Build` workflow runs Debug and Release tests on macOS arm64 and Windows x64
for branch pushes, pull requests, and manual runs. Each configuration uses a separate
runner. Release jobs also create a tested SDK archive and SHA-256 checksum as workflow
artifacts. Build outputs and ThinLTO link caches are removed after successful testing
and artifact upload, before saving the dependency cache, to reclaim runner disk space.

The shared setup action installs sccache 0.16.0 and enables its GitHub Actions cache
backend. The default compiler wrapper remains enabled. Pinned source dependencies,
GN, Ninja, LLVM and Unicode inputs use a separate cache keyed by OS, architecture and
dependency declarations. Debug and Release share this dependency cache. Every run
executes `gclient sync --no-history`, including cache hits, to synchronize the fixed
inputs and run their verification hooks. A missing or evicted cache requires a normal
download and build; cache availability does not determine build correctness. GitHub's
repository cache quota and eviction policy apply to both caches.

Push a tag matching `v` plus the version in `compiler/build_info.h` to run the `Release`
workflow. It rebuilds and tests both platforms at that tag, verifies archive checksums,
and publishes a GitHub Release after both builds and all asset uploads succeed. Tags
containing a hyphen produce prereleases. The workflow can also be run manually for an
existing version tag.
Only the publication job has `contents: write`; build jobs have read-only repository
access. No personal access token is required.

macOS packages probe the selected Apple SDK, record the exact SDK inputs and minimum
OS selected by Clang, and bundle the matching LLVM compiler-runtime archive. Apple SDK
files remain external. Before building AOT executables with an installed package, set
`VELA_MACOS_SDK` to the matching SDK path returned by
`xcrun --sdk macosx --show-sdk-path`; the required SDK version and file hashes are in
`share/vela/target.json`. Windows packages record their required MSVC and Windows SDK
roots in the same manifest. Object and static-library output do not require the final
system SDK link inputs. Linux distribution packaging still requires an audited
`--support-spec`; it is not part of these release workflows.
