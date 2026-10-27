# Third-party dependencies

The AOT backend uses the LLVM 21.1.8 development SDK. Host archive names and SHA-256
identities are pinned in `DEPS`; its `llvm_sdk` hook runs `python -m tools.llvm sync`.
For an existing checkout, `python -m tools.vl llvm sync` acquires the same package.
The acquisition keeps LLVM headers, static libraries, linking/archive tools, and
the checked Apache-2.0 WITH LLVM-exception license. These are compiler development
inputs, not prerequisites users must install alongside a Vela distribution.
`llvm-config` supplies the native backend library closure to GN. The SDK's absolute
zstd installation path is replaced by the repository-built zstd 1.5.7 dependency,
pinned at `f8745da6ff1ad1e7bab384bd1f9d742439278e99` and used under its
[BSD license](https://github.com/facebook/zstd/blob/f8745da6ff1ad1e7bab384bd1f9d742439278e99/LICENSE).
The LLVM license is retained at `third_party/build_tools/llvm/21.1.8/<host>/LICENSE.TXT`;
distribution packaging must include it. LLVM is a compiler dependency and must not
enter the generated application's runtime closure.

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
| [LuaJIT 2.1](https://github.com/LuaJIT/LuaJIT) | `a4c064539ea931274d407e1249b4bb575424a0f4` | MIT; [upstream license](https://github.com/LuaJIT/LuaJIT/blob/a4c064539ea931274d407e1249b4bb575424a0f4/COPYRIGHT) |
| [Abseil](https://github.com/abseil/abseil-cpp) | `76bb24329e8bf5f39704eb10d21b9a80befa7c81` | Apache-2.0; [upstream license](https://github.com/abseil/abseil-cpp/blob/76bb24329e8bf5f39704eb10d21b9a80befa7c81/LICENSE) |
| [GoogleTest](https://github.com/google/googletest) | `52eb8108c5bdec04579160ae17225d66034bd723` | BSD-3-Clause; [upstream license](https://github.com/google/googletest/blob/52eb8108c5bdec04579160ae17225d66034bd723/LICENSE) |
| [Google Benchmark 1.9.5](https://github.com/google/benchmark) | `192ef10025eb2c4cdd392bc502f0c852196baa48` | Apache-2.0; [upstream license](https://github.com/google/benchmark/blob/192ef10025eb2c4cdd392bc502f0c852196baa48/LICENSE) |
| [utf8proc 2.12.0](https://github.com/JuliaStrings/utf8proc) | `79cdd5ab40c79afa559f689ffc13b76812dee1ac` | MIT and Unicode-3.0; [upstream licenses](https://github.com/JuliaStrings/utf8proc/blob/79cdd5ab40c79afa559f689ffc13b76812dee1ac/LICENSE.md) |
| [PCRE2 10.49](https://github.com/PCRE2Project/pcre2) | `6f9d7c1373262c541324a16a358785b33ef116cf` | BSD-3-Clause WITH PCRE2-exception; [upstream license](https://github.com/PCRE2Project/pcre2/blob/6f9d7c1373262c541324a16a358785b33ef116cf/LICENCE.md). Bundled Unicode 17.0.0 data uses [Unicode-3.0](unicode_data/LICENSE) |
| Unicode UCD 18.0.0 | Versioned inputs and SHA-256 identities in [manifest.json](unicode_data/manifest.json) | Unicode-3.0; [license](unicode_data/LICENSE) |
| [yyjson 0.13.0](https://github.com/ibireme/yyjson) | `6447536015f3d600f3d65323b10976103b337ca7` | MIT; [upstream license](https://github.com/ibireme/yyjson/blob/6447536015f3d600f3d65323b10976103b337ca7/LICENSE) |
| [xxHash 0.8.3](https://github.com/Cyan4973/xxHash) | `e626a72bc2321cd320e953a0ccf1584cad60f363` | BSD-2-Clause; [upstream license](https://github.com/Cyan4973/xxHash/blob/e626a72bc2321cd320e953a0ccf1584cad60f363/LICENSE) |

GoogleTest, Google Benchmark and LuaJIT are `testonly` GN dependencies. Abseil is a
production dependency of the compiler and runtime, and Vela builds its own pinned copy.
PCRE2 builds only its 8-bit Unicode compiler/interpreter closure, listed in
`BUILD.gn`. It excludes DFA, conversion, substitution, serialization, command-line
tools and the sljit backend. `pcre2_jit_compile.c` supplies the no-JIT API stubs;
`SUPPORT_JIT` is unset. GN copies the upstream generic config/header and default
chartables into each output's generated directory without modifying upstream files.
The integration defines `PCRE2_STATIC`, `PCRE2_CODE_UNIT_WIDTH=8`, `SUPPORT_PCRE2_8`,
`SUPPORT_UNICODE`, `LINK_SIZE=2`, `MATCH_LIMIT=1000000`, `MATCH_LIMIT_DEPTH=1000`,
and `HEAP_LIMIT=8192` (KiB). Generic defaults retain parentheses nesting 250 and
maximum variable lookbehind 255. Runtime compilation fixes UTF/UCP, forbids `\\C`,
selects ANY newline and Unicode BSR, and uses default locale-independent tables.
Patterns can lower matching budgets with native directives. PCRE2's bundled
`pcre2_ucd.c` records Unicode 17.0.0, independent of string casing's Unicode 18.0.0.
The pinned revision and owning GN configuration participate in compiler cache identity.
utf8proc supplies Unicode grapheme segmentation and simple case mappings; Vela compiles the pinned C source
directly through GN and does not use its upstream build system.
The `unicode_case_data` DEPS hook downloads the pinned SpecialCasing and
DerivedCoreProperties inputs and verifies their SHA-256 identities. For an
existing checkout, `python tools/unicode_case.py --sync` runs that acquisition
step directly. GN then runs the same tool offline to generate
`gen/runtime/unicode_case_data.h` in each build output. It checks the pinned
utf8proc Unicode version, input schemas, and license identity. The full mappings,
Final_Sigma rule, and Cased/Case_Ignorable ranges supplement the public utf8proc
API. The generator and input identities participate in compiler cache identity.
`abseil_sources.gni` lists the upstream library source
closure for `flat_hash_map` and `flat_hash_set`, including their supporting hash,
string, sampling, synchronization, and time libraries. It excludes unrelated
flags, logging, status, and test implementations. Review this list against the
upstream target declarations when updating the revision.

Third-party headers are system headers and their
source targets use the compiler's default warnings; Vela's additional warning
flags and clang-tidy checks continue to apply to project-owned code, including
unit tests and benchmarks. Formatting skips upstream source trees.

LuaJIT is linked only by the benchmark executable. Its integration is in
[`BUILD.gn`](BUILD.gn). GN builds the upstream minilua and
buildvm host tools with the pinned compiler. [`luajit.py`](../build/scripts/luajit.py)
uses the target's preprocessed `lj_arch.h` for DynASM flags and invokes upstream
generators for version, VM assembly/object and definition headers. All generated
files live under each output's `gen/third_party/luajit`; upstream sources stay
unchanged. The runtime uses upstream `ljamalg.c`, the default allocator, GC64 and
the architecture's default number mode. Unix builds use external unwinding;
Windows builds consume buildvm's PE object. No system Lua installation or Lua
module installation is required. See [benchmark boundaries](../benchmarks/README.md)
for JIT mode selection and measurement.
