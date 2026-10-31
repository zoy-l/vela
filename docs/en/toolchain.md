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

The `vela` target provides source checking, JIT execution, native AOT builds,
JIT development sessions, testing, and source tooling commands. See
[language architecture](architecture.md) for language and runtime contracts,
and [AOT contracts](aot.md) for native distribution and platform SDK acceptance.
