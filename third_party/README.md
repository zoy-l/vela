# Third-party tools

The root [`DEPS`](../DEPS) file is the only source of host build tools. Run
`gclient sync --no-history` from this checkout to materialize the pinned GN,
Ninja, and Clang packages under `third_party/build_tools`.

The build scripts resolve executables by their repository-relative paths. They
do not read a system copy and do not modify `PATH`.
