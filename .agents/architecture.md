# Vela Architecture and Contracts

This document defines Vela's design, failure semantics, and module boundaries. See
[C++ Code Rules](cpp_style.md) for C++ conventions and [Development Workflow](development_workflow.md)
for execution and validation.

## Evidence and completion scope

- Inspect existing interfaces, implementations, production callers, tests, and build relationships
  first. Do not guess interfaces or domain semantics.
- Designs, roadmaps, and explicit user steps define the complete specification. Identify all
  remaining work across producers, intermediate representations, consumers, runtime, and validation.
- Prioritize architectural correctness, clear responsibilities, and long-term maintainability.
  Do not stop at a passing local test while production paths remain incomplete.
- Do not preserve compatibility layers, competing sources of truth, fallback paths, or incomplete
  migrations just to reduce the diff.

## Module boundaries

- `compiler/frontend` owns lexing, the syntax tree, and static type semantics. Its `syntax`,
  `modules`, `builtins`, `generics`, `flow`, and `typing` directories group those responsibilities.
  `compiler/ir` owns lowering from AST to Typed IR and IR optimization. `compiler/native` provides
  shared native code generation for `compiler/aot` and `compiler/jit`. JIT owns source compilation,
  native object caching and in-process linking; AOT owns offline artifacts. `runtime` owns values,
  objects, the heap, Isolate and native execution.
- `compiler/development` owns source-session and development-session lifetimes; `compiler/repl`
  owns REPL source composition; development sessions own its JIT execution. `compiler/cli` owns
  executable entry points and consoles.
  `compiler/tooling` owns editor and source tools, grouped into `lsp`, `semantic`, `format`,
  `documentation`, and `mock`. `compiler/project` owns project configuration; `compiler/testing`
  owns source-test discovery and execution. C++ test support lives in `compiler/test_support`,
  language integration tests in `compiler/language`, and module tests beside their implementations.
- `runtime/value` owns tagged values, object layout definitions, and callable contracts.
  `runtime/heap` owns allocation, storage, GC, and host roots; `runtime/isolate` owns runtime metadata
  and external-resource lifetimes. `runtime/async` owns futures, scheduling, and execution contexts.
  `runtime/native` owns native ABI, host bindings, instances, and execution. Collection, dynamic,
  string, JSON, regular-expression, library, exception, process, and testing responsibilities live
  in their corresponding runtime directories. Runtime GN targets remain centralized in
  `runtime/BUILD.gn`; distribution headers preserve these paths beneath `include/runtime`.
- Keep dependencies unidirectional: frontend does not depend on IR or runtime; IR depends on
  frontend; native compilation connects IR and runtime contracts; runtime does not depend on compiler.
- Pass facts from the same upstream producer directly or bind them during construction.
  Intermediate layers must not create metadata mirrors solely for downstream checks.
- Type inference belongs to the frontend's shared structural constraint solver and contextual
  expression checker. Intrinsic APIs project their declared payload/callback contracts into those
  mechanisms; they must not introduce independent inference engines. Specialization owns canonical
  concrete declarations, while the semantic model supplies final signatures and call targets to
  backends and editor tooling. Inference variables never reach IR or runtime.
- Prefer O(1), O(log n), or O(n) algorithms and data structures. Use hashing, sorting, indexing, or
  bucketing for growing datasets; avoid unindexed nested scans.
- New abstractions must serve a concrete responsibility: semantic conversion, ownership transfer,
  constraint consolidation, failure handling, caching, synchronization, or a stable module boundary.

## Failure semantics and lifetimes

- Return explicit diagnostics for untrusted source files and external input at actual boundaries
  such as the lexer, parser, checker, and file loader. Do not silently degrade or substitute defaults.
- Return recoverable failures to the owner that can handle them. Log once at the originating
  boundary or final owner; do not repeatedly rename and propagate the same failure across layers.
- Define ownership, borrowing duration, and destruction order for `Value`, heap objects, `Isolate`,
  native images, instances, and compilation results.
- Consumers should rely on guarantees established by types, construction, and lifetimes instead of
  repeating defensive checks at every layer. When removing redundant checks, also remove their
  result enums, branches, and tests.

### Native host bindings

`NativeFunction::Bind<Function>` derives the host signature and AOT calling convention from the
C++ function. Native instantiation validates that signature against the external declaration before
publishing the instance. The binding is immutable for the instance's lifetime; generated functions
resolve each used import once per invocation, including resumed invocations.
Host functions may take `NativeContext&` as their first argument or omit it. A context is supplied
only when the function or its codecs require it; scripts see the same external signature.

Functions returning plain `void`, `bool`, `int64_t` or `double` use a direct AOT entry with register
results. Exact scalar signatures retain the original C++ function pointer. Boolean and managed
arguments use their ABI codecs. Functions returning `std::expected` or managed values use a checked
entry that propagates errors and validates managed results, including Future completion types.
Both entries preserve logical frames, call-depth limits and GC roots during host execution.
Dynamic callable boundaries validate their arguments before entering typed native code.

Each `NativeExecution` owns its `NativeContext`; it borrows the same Isolate for the execution's
lifetime. Host calls borrow that context and their managed arguments for the duration of the call.

### Source-test mocks

`sdk:mock` owns interaction history, matching, responses, verification and call observations in
ordinary script objects. Generated adapters retain the original interface signatures and provide
separate call-pattern methods. The generator reads the production bound module graph, checks its
output before atomic publication, and never evaluates source initializers or test registration.

The native runtime exposes signature inspection, resolved source traces and test-context bridges.
Matchers and responses execute in normal script frames. Each mock session belongs to one real test
scope; the suite invokes its internal finalizers after all user cleanup and teardown callbacks,
before `testDone`. Call observations reuse tracked asynchronous expectations and retain their
registration context when errors are forwarded. Reporters receive copied strings, numbers and
resolved frames; interaction records do not retain machine-code owners. See
[the testing contract](../docs/zh-CN/testing.md#12-mock-扩展) for the public API and failure
categories.

## Assertion boundaries and cleanup

- Syntax, type, file, and runtime data errors that users can trigger must not cause fatal assertions.
  Return diagnostics or an explicit execution status.
- Assertions are only for development contracts established by code structure and internal
  lifetimes. They do not replace input validation or runtime error handling.
- After every change, clean up obsolete fields, APIs without production callers, temporary output,
  stale comments, and caches along the compiler -> IR -> native code -> runtime call chain.
