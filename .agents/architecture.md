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
- Temporary contiguous native storage uses `NativeBuffer`; external blocks participate in heap
  accounting and may trigger GC during construction. Callers root borrowed managed inputs first
  and check allocation once before borrowing the buffer. The owner and its Heap cover that borrow.
- Consumers should rely on guarantees established by types, construction, and lifetimes instead of
  repeating defensive checks at every layer. When removing redundant checks, also remove their
  result enums, branches, and tests.

### Object fields

Object fields store eight-byte payloads in one contiguous region and `ValueKind` tags in a separate
region. Inline instances own this storage after their object header. Reloadable instances own an
`InstanceStorage` block whose initialization states follow the tags. Inline object headers and
reloadable field blocks record their physical field counts. Native code addresses payloads
directly and reads tags when the value representation requires them. Boxed `Value` and `NativeValue`
records retain their sixteen-byte ABI; array storage follows its element layout.

`Heap::GetField` probes for a field and returns a copied value, or no value for an object without a
class layout or an out-of-range field. `Heap::LoadField` reads a field whose existence is guaranteed
by the caller's layout contract. `Heap::SetField` reports `InvalidField` when that field is absent.
Managed references in a copied field borrow the heap and require a root before the
next allocation. `ObjectFieldStorage` borrows the owner's storage; callers keep the owner rooted and
do not retain the view across migration or a safepoint that can publish a migration. Prepared
migrations register their field blocks as roots until commit or discard. Commit replaces the layout
and complete field block before releasing the previous storage.

### Native host bindings

`NativeFunction::Bind<Function>` derives the host signature and AOT calling convention from the
C++ function. Native instantiation validates that signature against the external declaration before
publishing the instance. The binding is immutable for the instance's lifetime; generated functions
resolve each used import once per invocation, including resumed invocations.
Host functions may take `NativeContext&` as their first argument or omit it; scripts see the same
external signature. Direct entries use one context-first ABI, and the generated C++ adapter decides
at compile time whether the host function consumes that context.

Functions use a direct AOT entry with register results only when their argument codecs, return
conversion and call cannot fail. Direct host-class arguments borrow native pointers from rooted
wrappers; checked arguments retain the managed wrapper representation.
Direct booleans follow the C++ `bool` ABI, including LLVM zero-extension attributes; checked
entries continue to encode booleans as bytes.
Context-dependent managed decoding, `std::expected` results and managed return values use a checked
entry that propagates errors and validates results, including complete Future payload contracts.
Both entries preserve logical frames, call-depth limits and GC roots during host execution.
Dynamic callable boundaries validate their arguments before entering typed native code.

`include/vela/host.h` exposes logical types, `Local`, `Persistent`, `Type`, `Entry` and `Result`.
Canonical host descriptions use the shared layout graph, resolved against an explicit instance or
development session for script nominals. Type roots retain structural metadata; captured callables
retain code versions. Named session entries resolve the current publication. Local scopes and
persistent roots share the heap root mechanism, and both success values and `Error` payloads cross
the boundary while rooted. Their native storage participates in heap accounting and external
resources obey the existing shutdown protocol. Ordinary handles remain confined to the Isolate
thread; typed completion endpoints use the scheduler's explicit cross-thread delivery contract.

Closed native images share layout identities while their image owner remains alive. The cache
holds that owner weakly and traces its layout graph during metadata collection. After the last
image owner retires, only live objects and type roots retain those layouts; even an empty nominal
container must keep its element metadata valid. Host type resolution never promotes collectible
structural metadata into a permanent Isolate cache.

Generated `external class` bindings use this same signature and calling convention. Clang tooling
belongs to the independent binding generator; runtime code consumes immutable class descriptions,
logical host keys and typed entries. Registration publishes a whole module atomically. Instantiation
checks the complete public class contract, including unused members and parameter names, before
publishing any imports. JIT reuse keys include the class key, contract and hidden receiver.

Each Isolate owns its canonical host class layouts and a weak wrapper index. `ObjectKind::Host`
stores a stable native pointer outside the script field layout and remains a GC leaf. A wrapper
owns either a typed unique deleter or one shared owner; duplicate shared results reuse the wrapper
only when their control blocks match. GC removes the weak entry before releasing ownership. Host
storage, ownership records and reported external memory participate in heap accounting; construction
reserves its budget before entering the C++ constructor and retains RAII ownership until publication.
Typed method arguments borrow the pointer while the wrapper is rooted, without shared-owner copies.
After collection, the weak index shrinks to the capacity needed by its live entries. If allocating
the smaller table fails, the live index remains valid and a later collection can reclaim its excess
capacity. The next GC threshold uses the actual heap size after compaction.

Binding snapshots share descriptions, never another Isolate's wrappers or nominal pointers. A
development session freezes those bindings; script reload may retain wrappers but cannot change a
host key or public contract, replace native implementations, or migrate native storage. Bound
methods, asynchronous frames and typed AOT export handles retain the wrapper through the existing
root contracts. Host destructors must be `noexcept` and must not allocate Vela objects or reenter
script. See [native class bindings](../docs/zh-CN/native_class_bindings.md) for ownership and generator
contracts.

Each `NativeExecution` owns its `NativeContext`; it borrows the same Isolate for the execution's
lifetime. Host calls borrow that context and their managed arguments for the duration of the call.
The context constructs its handle scope on first use and releases it after each host call. Linked
AOT direct calls perform that cleanup in generated code; dynamic entries retain scoped adapters.
On Windows x64, bindings whose C++ signature accepts borrowed string views and unchanged scalar
arguments can expose the native view ABI. C++ constructs each opaque view, and generated calls copy
it for each value parameter. The compiler never assumes the standard library's member layout.
Closed synchronous AOT functions whose only successful observable operations are host calls may
borrow `host_execution` for the whole function. Reentrant executions retain their own contexts,
restore the outer execution, and preserve the caller's logical frames.

The execution registers its exception root when it publishes an exception envelope, and releases
that root when ownership transfers to a catch handler, execution aborts, or the execution ends.

Synchronous native stack roots form a LIFO chain owned by the Heap, including reloadable frames
and their synchronous recovery calls. Async frames retain independent root registration. Frames
containing loops initialize their root slots and link them on entry so loop optimizations see a fixed
root chain. Bounded, acyclic optimized synchronous frames may initialize and link their roots with
the logical trace at the first observable boundary. Linked frames restore the previous head on every
exit; a frame that never links neither clears root slots nor restores a root-chain head. Closed synchronous
frames may defer roots when their complete successful path only performs inline scalar or field
operations, initialized global access, and direct self-recursion using stack storage. These frames
retain logical trace and depth bookkeeping. The first failure temporarily roots only its initialized
live prefix while constructing the exception. Ancestors propagating an existing exception or an
exception-allocation failure must not trace their abandoned locals; an outer handler retains its
own observable values through the ordinary root plan. Instance string
constants remain rooted by program state and need no duplicate synchronous frame roots. Native
string views and collection lookup locations participate in the existing frame-size budget;
large frames use the same recoverable native storage, with sixteen-byte alignment.

AOT may execute small pure recursive calculations through a bounded leaf entry after reserving
logical depth and physical stack space. Exhausting the bound or encountering an arithmetic failure
returns to the full entry to construct the original error and trace. Eligibility excludes state
access, side effects and loops, so that this execution cannot repeat observable work. Managed
recursive bodies use complete inlined frames and retain their root and error contracts.

Closed linked AOT may specialize direct-call bodies using integer ranges, array aliases and all
element writes. Control-flow constraints and source-constant widening establish a fixed point across
loops and recursive calls before code generation removes proven redundant bounds or overflow checks.
Exported, callable, interface, constructor and property entries contribute unrestricted parameters;
arrays published through globals or unknown uses do not acquire stronger element assumptions.
Specialized bodies remain outside the NativeImage entry table. Trusted host calls to internal
functions retain the generic body and its full type-domain checks. Range proofs preserve access and
failure order, logical frames, roots and physical stack budgets; they do not enlarge parameter-array
caches or apply to dynamic JIT code.

Reloadable optimized JIT may inline small pure scalar functions and readonly array kernels called
inside loops. Scalar leaves are statically bounded; readonly kernels use the shared dynamic work
guard and retain ordinary safepoints when that guard does not hold. Empty generated constructors
also retain a direct leaf edge with their full stack and observation contracts.
These direct edges retain body dependencies for code lifetime and invalidate their callers when a
callee changes; other cross-component calls retain typed entry slots. Slot selection participates in
the caller's code-reuse fingerprint. Leaf execution checks pending control requests and preserves
stack budgets; requested observation enters the complete function, while arithmetic failure
materializes its original source frame. Active callers retain baseline recovery and resume after
completed calls without replaying their effects.

Script reflection caches borrow code from one publication and validate its epoch, execution program
and unique receiver layout id before calling a signature-specific entry. Caches retain neither the
receiver nor old layouts or code owners. Positional argument values are fully decoded before entering
the callee, preserving snapshots when the callee changes the argument objects or collects garbage.
Named, default and rest arguments retain the complete binding contract.

Closed linked AOT binds reflected member identity only from single-definition descriptors, immutable
global initializers and consistent no-argument return summaries. Public instance fields with primitive
or dynamic values and a statically matching non-null receiver use ordinary field storage directly;
writes retain value checks and GC barriers, including derived receivers with larger layouts. Small
scalar methods may use the existing leaf entry after full positional argument validation. Mutable
descriptors, unresolved inheritance queries and other value contracts retain general reflection.

Counted loops may share request checks and stable reflection argument guards across bounded chunks.
Both the iteration limit and successful-path instruction budget bound the observation delay. Guard
loads must remain valid under every successful-path write; repeated writes of an already-checked
kind tag preserve that fact. Guard checks retain their short-circuit order, and a failed guard enters
the ordinary current iteration before any chunk effects. Each chunk reloads mutable arguments;
user calls, allocation and other unproven mutations retain per-iteration observation and validation.

Default type and member lookups reuse a single descriptor-cache slot keyed by operation, name,
qualifier, result contract, execution program and publication epoch. Only lookups within the same
program populate the slot, so generic descriptor contracts never cross program-local type indices.
The cache traces its names and result through its owning program or handle, including null results;
publication invalidates both positive and negative entries. Mutable query and type-argument objects
do not participate in this cache.

An immediately consumed standard `CallArguments` construction containing an array literal of
standard `CallArgument` constructions lowers to an ordered value/name register window. These
records have no escaping identity or user constructor effects. Expression evaluation order,
conversions, GC roots, suspension and canonical recovery values remain in the ordinary IR pipeline.
Existing argument objects are snapshotted at invocation. Both representations finish decoding before
entering user code and share the same named, default, rest and constructor binding. Deleted or
withdrawn targets are resolved before argument-binding errors.

Reloadable optimized reflection may use a bounded entry for small synchronous scalar methods that
access only their receiver's scalar fields and contain no loops, allocation or user calls. The caller
checks requests and saves roots and observation state before probing the cache. The cache reuses the
caller's no-request result; a boundary that processes requests enters the complete execution path,
including persistent debugger observation. Cache construction requires the function version's
canonical receiver layout, and the entry checks that all fields are ready before performing any
effects. This bounded fragment reuses the caller's safety boundary;
ordinary entries keep their full safepoints. Arithmetic failures materialize the original frame at the
failing instruction without replaying completed writes. Unready fields and noncanonical layouts enter
the ordinary binding and execution path before any bounded-body effects occur.

String constants are borrowed from the current execution instance's immutable constant pool.
Publication retains old pools with their instances, and host reentry restores the caller's instance.
Synchronous frames need no duplicate root for values derived solely from that pool and null through
copies. Closed linked AOT with safepoint-only observation also applies this ownership proof to global
slots proven readonly by the shared global analysis. Every derivation must reach a known instance
root or null; an unknown definition invalidates its entire copy chain. Parameters, mutable-global
loads, field reads and recovery materialization do not acquire this proof, and async frames retain
their ordinary roots.
Initialization depth is owned by `NativeControlState`; generated construction boundaries and runtime
scopes update that same counter, including failure cleanup, so publication cannot expose partial
construction.

Repeated field traversals may be folded only when every object receives identical independent
updates, the increment fields remain immutable, and no failure can observe an enlarged update
prefix. Initialization is preserved as a separate first iteration. Floating additions retain their
original order. An AOT Map get/set pair may reuse its lookup only when the get dominates the set,
the map and key have unchanged single-definition origins, and every intervening path avoids state
access, allocation and user calls. Existing untagged integer entries can then be written directly;
insertions retain the normal allocation, GC, iteration-order and failure behavior.

Development-session entry lookup caches a borrowed target from the current publication and validates
its revision before reuse. Publication and stopping clear that cache before old state can retire;
the cache owns no GC root or code version. Public exported calls and trusted internal entry calls
retain distinct access checks. An omitted internal-entry module selects the session's entry module.

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
