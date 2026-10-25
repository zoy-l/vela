# Third-party dependencies

`gclient sync --no-history` obtains source dependencies and host tools from the
immutable revisions in the root `DEPS`. Vela's GN integration lives in
`third_party/BUILD.gn`; upstream sources are not modified. All required sources
and host tools are obtained through this dependency configuration.

GN, Ninja, and Clang are installed under `third_party/build_tools`. Build scripts
resolve their repository-relative paths, use no system copies, and do not modify
`PATH`.

Windows uses the project `clang-cl`, `lld-link`, and `llvm-lib` with the installed
Visual Studio C++ libraries and Windows SDK. macOS uses Xcode for its SDK,
standard library, and linker. See the root README for platform configuration.

| Dependency | Revision | License |
| --- | --- | --- |
| [Abseil](https://github.com/abseil/abseil-cpp) | `76bb24329e8bf5f39704eb10d21b9a80befa7c81` | Apache-2.0; [upstream license](https://github.com/abseil/abseil-cpp/blob/76bb24329e8bf5f39704eb10d21b9a80befa7c81/LICENSE) |
| [GoogleTest](https://github.com/google/googletest) | `52eb8108c5bdec04579160ae17225d66034bd723` | BSD-3-Clause; [upstream license](https://github.com/google/googletest/blob/52eb8108c5bdec04579160ae17225d66034bd723/LICENSE) |
| [Google Benchmark 1.9.5](https://github.com/google/benchmark) | `192ef10025eb2c4cdd392bc502f0c852196baa48` | Apache-2.0; [upstream license](https://github.com/google/benchmark/blob/192ef10025eb2c4cdd392bc502f0c852196baa48/LICENSE) |
| [utf8proc 2.12.0](https://github.com/JuliaStrings/utf8proc) | `79cdd5ab40c79afa559f689ffc13b76812dee1ac` | MIT and Unicode-3.0; [upstream licenses](https://github.com/JuliaStrings/utf8proc/blob/79cdd5ab40c79afa559f689ffc13b76812dee1ac/LICENSE.md) |
| [yyjson 0.13.0](https://github.com/ibireme/yyjson) | `6447536015f3d600f3d65323b10976103b337ca7` | MIT; [upstream license](https://github.com/ibireme/yyjson/blob/6447536015f3d600f3d65323b10976103b337ca7/LICENSE) |
| [xxHash 0.8.3](https://github.com/Cyan4973/xxHash) | `e626a72bc2321cd320e953a0ccf1584cad60f363` | BSD-2-Clause; [upstream license](https://github.com/Cyan4973/xxHash/blob/e626a72bc2321cd320e953a0ccf1584cad60f363/LICENSE) |

GoogleTest and Google Benchmark are `testonly` GN dependencies. Abseil is a
production dependency of the compiler and runtime, and Vela builds its own pinned copy.
utf8proc supplies Unicode grapheme segmentation; Vela compiles the pinned C source
directly through GN and does not use its upstream build system.
`abseil_sources.gni` lists the upstream library source
closure for `flat_hash_map` and `flat_hash_set`, including their supporting hash,
string, sampling, synchronization, and time libraries. It excludes unrelated
flags, logging, status, and test implementations. Review this list against the
upstream target declarations when updating the revision.

Third-party headers are system headers and their
source targets use the compiler's default warnings; Vela's additional warning
flags and clang-tidy checks continue to apply to project-owned code, including
unit tests and benchmarks. Formatting skips upstream source trees.
