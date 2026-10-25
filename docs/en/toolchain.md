# Vela toolchain contract

[**English**](toolchain.md) | [简体中文](../zh-CN/toolchain.md)


`DEPS` is the single dependency declaration. Its CIPD entries materialize
host-specific GN, Ninja, and Clang packages under
`third_party/build_tools/<tool>/<host-platform>`. The path is ignored by Git
because it is generated checkout state.

`tools/config.py` resolves those paths from the repository root and passes
absolute paths into GN arguments. The generated toolchain uses the same paths
for compilation, archiving, linking, and formatting. This keeps Debug and
Release outputs separate and avoids ambient tool discovery.

The supported bootstrap hosts are Windows x64, Linux x64, and macOS arm64.
The initial local validation runs on Windows x64. Adding another host requires
the matching CIPD package entries and the corresponding GN toolchain flags in
the same change.

The `vela` target is intentionally a bootstrap executable. It proves that the
compiler target, linker, and test target are wired correctly while syntax and
runtime contracts remain unspecified.
