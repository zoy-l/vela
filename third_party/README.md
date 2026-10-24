# Third-party dependencies

`gclient sync --no-history` obtains source dependencies and host tools from the
immutable revisions in the root `DEPS`. Vela's GN integration lives in
`third_party/BUILD.gn`; upstream sources are not modified. No VerseGame checkout
or tools are needed to build or run Vela.

GN, Ninja, and Clang are installed under `third_party/build_tools`. Build scripts
resolve their repository-relative paths, use no system copies, and do not modify
`PATH`.

Windows uses the project `clang-cl`, `lld-link`, and `llvm-lib` with the installed
Visual Studio C++ libraries and Windows SDK. macOS uses Xcode for its SDK,
standard library, and linker. See the root README for platform configuration.

| Dependency | Revision | License |
| --- | --- | --- |
| [GoogleTest](https://github.com/google/googletest) | `52eb8108c5bdec04579160ae17225d66034bd723` | BSD-3-Clause; [upstream license](https://github.com/google/googletest/blob/52eb8108c5bdec04579160ae17225d66034bd723/LICENSE) |
| [Google Benchmark 1.9.5](https://github.com/google/benchmark) | `192ef10025eb2c4cdd392bc502f0c852196baa48` | Apache-2.0; [upstream license](https://github.com/google/benchmark/blob/192ef10025eb2c4cdd392bc502f0c852196baa48/LICENSE) |

Both are `testonly` GN dependencies. Their headers are system headers and their
source targets use the compiler's default warnings; Vela's additional warning
flags and clang-tidy checks continue to apply to project-owned code, including
unit tests and benchmarks. Formatting skips upstream source trees.
