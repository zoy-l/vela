# Vela Development Workflow

The root `README.md` and `tools/vl.py` are authoritative for project facts and commands. See
[Architecture and Contracts](architecture.md) for architecture and [C++ Code Rules](cpp_style.md)
for C++ conventions. See [Documentation Changes](documentation_changes.md) for documentation-editing
constraints.

## Before starting

1. Inspect the workspace state and applicable `AGENTS.md` files.
2. Inspect the target implementation, actual callers, tests, platforms, and build variants to
   establish the production contract chain.
3. Track completion criteria and remaining work for tasks with multiple stages. Update them
   according to actual dependencies when the scope changes.

## Change discipline

- Change underlying semantics at the production source, migrate consumers along the call chain,
  and remove obsolete paths.
- Use structured parsing and generation interfaces for structured data. Avoid fragile string
  concatenation.
- Do not edit `out/`, `.cache/`, or other generated directories. Do not modify third-party upstream
  sources to work around integration problems.
- When behavior changes, check that tests, documentation, build dependencies, and comments still
  match the final contract.

## Builds and dependencies

- Use the repository's `python -m tools.vl ...` commands for routine builds, tests, benchmarks,
  clangd compilation databases, and lint.
- Do not modify the global `PATH` or add system tools to the environment as project dependencies.
- Regenerate the corresponding output directories after GN changes. Debug and Release outputs
  must not share object files or generated resources.
- Add third-party dependencies through `DEPS`, GN configuration, and necessary license records.
  Do not introduce CMake, FetchContent, Conan, or vcpkg.

### Compiler cache

Developer builds use an installed `sccache` executable by default for C, C++, and assembly
compilation on Windows x64, Linux x64, and macOS arm64. Configuration resolves its absolute path;
a missing wrapper is an error. Linking and archiving invoke the toolchain directly.

`python -m tools.vl config debug --cc-wrapper /path/to/sccache` and
`python -m tools.vl build release tests --cc-wrapper /path/to/sccache` select a wrapper executable.
Quote paths containing spaces. An explicit `--cc-wrapper=""` disables the wrapper for that
invocation. A later command that configures a build without this option selects `sccache` again.
The wrapper is a developer build dependency; Vela distribution users do not need it.

Use `sccache --show-stats` to inspect cache hits. Cache storage and remote-cache settings use
sccache's own configuration. Changes to compiler flags, headers, or the compiler still participate
in its cache key; enabling a wrapper does not change Debug/Release separation or runtime ABI rules.

## Validation and completion

- During implementation, run targeted validation only when it can reveal a concrete current error.
  Once the implementation is complete, run the relevant builds, regression tests, and static checks.
- For tasks that require lint, run repository-wide `vl lint` and fix every reported diagnostic,
  including pre-existing findings outside the initial change scope. Existing lint failures are not
  an acceptable completion state.
- Validate behavior and performance separately. Performance conclusions require measurements from
  a Release build; compilation or static reasoning cannot replace benchmarks.
- Retain benchmark evidence according to [Benchmark Result Retention](benchmark_results.md).
- For changes limited to rules or documentation, check content, references, encoding, and diffs.
  Do not trigger unrelated full builds.
- Before completion, check `git diff --check`, UTF-8/LF formatting, workspace changes, and obsolete
  remnants. Report any unverified work accurately.
