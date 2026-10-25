# Vela AI Development Rules

These rules apply throughout the repository. Explicit user instructions take precedence over
repository defaults. More specific `AGENTS.override.md` / `AGENTS.md` files in applicable directories
take precedence over the root rules.

## Starting a task

1. Inspect the workspace state. Preserve existing staged, unstaged, and untracked changes, and check
   for applicable rules in the target directories.
2. For implementation, review, performance investigation, build configuration, and related
   documentation work, read [`README.md`](README.md),
   [Development Workflow](.agents/development_workflow.md), and
   [Architecture and Contracts](.agents/architecture.md). Before changing C++, read
   [C++ Code Rules](.agents/cpp_style.md).
3. Derive completion criteria from the user's objective and roadmap. Complete the affected paths
   through producers, intermediate representations, consumers, tooling, and validation.
   Do not treat the first fix or a passing local test as completion of the entire task.

## Tooling and workspace constraints

- Prefer the `vl` tools for builds, tests, GN configuration, formatting, and static analysis. Commands
  are documented in [`README.md`](README.md) and [`tools/vl.py`](tools/vl.py). Do not modify the
  global `PATH`.
- Do not overwrite, revert, or format unrelated files or changes. Do not manually edit generated or
  cached directories such as `out/`, and do not use destructive Git commands.
- Use the patch tool for manual edits. Use UTF-8, LF line endings, and a final newline.
- Do not use `NOLINT` to hide diagnostics. Fix actual lint findings or document necessary exceptions
  in the project rules.
- Do not use screenshots to validate execution results. Use logs, status, tests, or reproducible
  programmatic assertions.

## Maintaining the rules

Keep project facts and commands in `README.md`, architecture and failure boundaries in
`.agents/architecture.md`, execution and validation in `.agents/development_workflow.md`, and C++
details in `.agents/cpp_style.md`. Define each rule only in its owning file and link to it elsewhere.
New rules must state their scope, required action, and necessary exceptions. Check relative links
and conflicting provisions whenever rules change.

Documentation editing restrictions for the project-root README files and temporary execution
documents are defined in [Documentation Changes](.agents/documentation_changes.md).

## Change discipline

- After every change, clean up obsolete code, stale comments, duplicate helpers, APIs without
  production callers, temporary output, and generated caches along the affected paths.
- Do not introduce compatibility layers, fallback paths, or preservation of old behavior unless
  explicitly requested by the user.
- Do not guess interfaces. Inspect existing implementations, actual callers, tests, and build
  relationships first.
- Do not invent domain semantics. Ask the user when the code and existing design cannot establish
  the intended meaning.
- Consolidate repeated access to the same expression within a scope into a local variable, following
  the [C++ Code Rules](.agents/cpp_style.md).
- Do not turn temporary workarounds or implementations intended only to make something run into the
  permanent architecture.

## Global code modification rules

- After every change, inspect related legacy code and remove implementations that no longer serve
  a purpose.
- Do not introduce compatibility layers, fallback paths, or preservation of old behavior unless
  explicitly requested by the user.
- Diff size is not an engineering objective. Bug fixes, refactoring, and architecture work must
  prioritize architectural correctness, clear responsibilities, complete contracts, and long-term
  maintainability.
- Prefer a local variable when accessing the same expression repeatedly within a scope.
- Fix CRLF/LF warnings and preserve the repository's established line-ending convention.
- Avoid excessive defensive design. Retain only real contracts, actual input boundaries, and
  necessary invariant checks.
- Do not guess interfaces; inspect existing implementations and call relationships first. Do not
  invent domain semantics; ask the user when the intended meaning cannot be established.
- Prefer existing interfaces. Do not add abstractions without a concrete justification.
- Validation must support implementation progress. Do not repeat a passing test or lint run when
  the relevant implementation has not changed. Run repository-wide checks at the task completion
  boundary.
- Follow the existing architecture and conventions when refactoring. Avoid unrelated extensions.
- Design documents, roadmap milestones, and explicit implementation steps specified by the user
  are the complete specification. Finish every production path and contract between stages; do not
  stop after fixing the first gap or passing a local test.
