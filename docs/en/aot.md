# Native AOT compilation design

[**English**](aot.md) | [简体中文](../zh-CN/aot.md)

> Status: shared compilation boundaries are being implemented; the native backend is not implemented. `vela build`, artifact formats, and the native ABI below are proposed contracts.

Vela AOT generates native object files from the shared Typed IR and uses linking tools shipped with the toolchain to produce executables or static libraries. LLVM is an internal compiler dependency; users of an official distribution do not install Clang or LLVM separately. Generated applications depend on the Vela runtime and target platform libraries, not LLVM, the Vela compiler, or the bytecode interpreter.

This design covers compilation, native execution, GC, exceptions, async, embedding, linking, and distribution. Existing topic contracts remain authoritative for language semantics; AOT does not define a second language behavior. System SDK availability and licensing are separate from packaging LLVM. “No LLVM installation” does not mean every platform can build without a system SDK.

## User interface and scope

The proposed standalone build interface is:

```sh
vela build main.vela -o app
```

A library entry file can export functions for a host:

```vela
export func add(left: int, right: int): int {
    return left + right;
}
```

```sh
vela build math.vela --emit=obj --export=add -o math.o
vela build math.vela --emit=staticlib --export=add -o libmath.a
```

`--emit` defaults to `exe` and also accepts `obj` and `staticlib`. Repeatable `--export` selects public top-level functions from the entry module for native export. It does not change language-level export visibility, implicitly export imported modules, or permit exporting an unspecialized generic function. Checking still covers the loaded source; export selection cannot hide invalid source.

Executables use the existing zero-argument `main` contract, including synchronous or Future results, exit codes, unhandled errors, and `AsyncIdle`. Libraries do not require `main`, and instantiation does not execute a user entry function. An object file covers the entry file's complete import graph. A static library packages that compilation unit; it does not establish independent binary linking of Vela modules.

Library artifacts also generate `<output-path>.h` and `<output-path>.aot.json`, containing export wrapper declarations and a link manifest. Windows object files, static libraries, and executables use `.obj`, `.lib`, and `.exe`. Example paths do not define automatic extension conversion across platforms.

The first release supports native compilation for the existing bootstrap host matrix: Windows x64, Linux x64, and macOS arm64. Each platform must pass its own distribution acceptance before support is advertised. JIT, hot replacement, native dynamic-library loading, ABI compatibility across releases, independently distributed binary modules, and VM/AOT calls into each other are outside this release. REPL and the existing VM debugger continue to serve bytecode. Unsupported AOT operations must produce explicit diagnostics, never execute embedded bytecode as a fallback.

## Existing foundation and boundaries to separate

| Verified implementation | Required AOT change |
| --- | --- |
| [IR](../../compiler/ir/ir.h) uses 32-bit virtual register IDs, source locations, and exception handlers; [shared analysis](../../compiler/ir/analysis.h) supplies instruction effects and liveness across exception and resume edges | Supply type facts for every value definition to generate typed native values and root slots; keep bytecode field widths out of native capability limits |
| [Compilation preparation](../../compiler/ir/preparation.cc) runs the frontend, layout description, and common optimization offline, returning IR that borrows neither AST nor Isolate state; VM allocation and encoding belong to a separate backend | Branch from the same IR into LLVM lowering and code generation |
| [Layout instantiation](../../runtime/program_layouts.cc) consumes pointer-free descriptions; VM native-binding checks consume IR signatures | Reuse layout instantiation for native instances and connect native imports, constant roots, and global state |
| [NativeFunction](../../runtime/native_function.h) generates adapters from C++ signatures and invokes them with `RegisterView` | Reuse declarations and codecs to generate typed AOT entries; never cast the existing callback into a machine-code function pointer |
| The [Future scheduler](../../runtime/future.h) already belongs to runtime, while suspended frames and callback execution remain in `vm` | Share Future state and scheduling semantics, and supply native continuations and callback execution without a VM executor dependency |
| [GC](garbage_collection.md) already provides precise roots, a non-moving heap, and reference write barriers | Generate native root slots and safepoints, reusing the heap and barriers without conservative stack scanning |

These changes are prerequisites, not claims that current interfaces already emit machine code.

## Pipeline and module responsibilities

```text
source and import graph
  → parsing, module binding, generic specialization, static checking
  → Typed IR and common optimization
      ├─ VM register allocation → bytecode encoding → VM
      └─ AOT lowering → LLVM IR → LLVM optimization and target code generation
                               → object files → archiving or linking
```

`compiler/frontend` retains language semantics; `compiler/ir` owns shared lowering, control flow, and semantic optimization. A proposed `compiler/aot` owns LLVM conversion, root-slot planning, native ABI calls, and object emission. The build driver selects platforms, links, archives, and publishes artifacts. LLVM headers and types remain private to the AOT implementation.

`runtime` supplies objects, GC, Futures, errors, and common execution services. Native execution support owns program instances, native calls, and source traces. Neither may depend on frontend, IR, LLVM, or VM. The CLI may link both compilation backends and the VM; a pure AOT application links only the runtime components required for its execution. Existing `Compile*` interfaces returning VM programs continue to serve the VM. AOT must not create an unused temporary VM program through that path.

Current IR is a virtual-register instruction representation with reassignment, not LLVM IR already converted to SSA. AOT lowering must construct correct SSA values or promotable typed local slots for definitions, merges, and loops. Exception successors and await resume edges participate in dataflow analysis. Complete types, nullability shapes, callable signatures, and reference kinds must come explicitly from upstream, not guesses based on type-name strings or VM runtime values.

Shared operation facts must cover failure, allocation, GC, host calls, and suspension for optimization, liveness, and both backends. Extend the existing analysis responsibility rather than maintaining inconsistent semantic operation tables per backend. Calls not proven free of GC must be treated as capable of collection; only leaf operations with a proven contract may omit rooting.

Remove shared-path dependencies on 8-bit operands, 256 physical registers, and bytecode jump distances. The VM continues to diagnose those limits at its allocation and encoding boundaries. AOT diagnoses its own native-stack, object-format, and metadata limits instead of removing every limit and relying on LLVM to crash.

## Value representation and native ABI

Ordinary local `int`, enum ordinal, `double`, and `bool` values use 64-bit integers, 64-bit integers, binary64, and boolean representations respectively. Non-null references use non-moving heap pointers. Nullable primitives carry independent validity information; no valid numeric value is reserved as a sentinel. Heap fields, arrays, dynamic values, Expected, closure captures, and runtime type identity retain the existing object model. Unboxed locals do not imply a different heap layout.

Native calls have three boundaries:

| Boundary | Contract |
| --- | --- |
| Within a compilation unit | Typed arguments and results, with a hidden execution context for the instance, Isolate, and execution state; the compiler controls this private calling convention |
| Generated code to runtime | A versioned internal ABI with explicit fixed-width fields, pointers, layouts, alignment, results, and error channels; no reliance on C++ name mangling or standard-library object layouts |
| Host exports/imports | Generated C ABI wrappers and a type-safe C++ facade; raw machine-code entries are not public C++ functions |

Export wrappers take an explicit instance, return a status, and write a successful result to an output. Primitives use fixed-width representations. Managed results such as strings, objects, and Futures are delivered through rooted handles with Isolate ownership, explicitly released by the host. Structural signatures still participate in composite-value validation; object interiors are not exposed as host structs. Public wrappers must not expose the binary layouts of `std::string`, `std::expected`, or `std::shared_ptr`. Existing borrowed VM results retain their contract and must not be mistaken for newly rooted export results.

Native execution support maintains logical call frames and source positions independently of machine-register allocation. Preserve the current call-depth policy and `StackOverflow` status, checking before entering another script call. Inlining must not accidentally alter observable depth or traces. Native frame size must be bounded; large temporary storage uses runtime-managed allocation that can report failure, preventing user source from causing unchecked native-stack exhaustion. Normal, error, and suspension exits clean up frames and root records consistently.

## GC roots, safepoints, and write barriers

The first implementation uses explicit shadow root slots backed by existing tagged `Value` storage and `RootRange`. Only managed references live across safepoints need roots; pure numeric values remain in machine registers or ordinary local slots. Initialize root slots to null before registration and unregister before function exit. Slots can be reused over reference live intervals and are cleared when references die, avoiding retention merely because the frame remains active.

Safepoints include runtime calls that may allocate or advance GC, script calls that can reach those operations, and host calls. GC continues to advance cooperatively on the owning Isolate thread. Pure arithmetic loops do not introduce asynchronous collection or new task scheduling points. Any later explicit polling must participate in the same effect and root analysis.

Generated code must obey this ordering:

1. Before a safepoint, store every live reference, including arguments, receivers, closure environments, pending exceptions, and return values saved across finally.
2. No allocation omitted from the analysis may occur between root registration and the call. Caller roots cover a runtime helper's entire use of borrowed references.
3. After a reference return, the caller stores it in a registered root slot before another safepoint. GC cannot occur between callee root release and caller receipt. Host export wrappers complete the rooted-handle handoff before returning.
4. Suspension may unregister native frame roots only after the continuation state is traced through a reachable Future graph.

Root-slot addresses and registration records must remain visible to scanning. LLVM aliasing, memory effects, and call attributes must describe this accurately. Helpers that can observe roots cannot receive incorrect attributes such as `readnone`; optimization cannot delete root stores or move them across safepoints. GC stress validation must exercise optimized machine code.

Reference updates to fields, arrays, collections, globals, capture cells, and continuations obey existing barriers during both marking and sweeping. Initially reuse runtime access and mutation operations. Direct generated accesses require proof of type, bounds, layout, and barrier behavior. A non-moving object does not make a growable array's backing buffer immovable; element addresses cannot survive calls that may grow it.

This release does not add conservative scanning, LLVM stack maps, or another collector. A later rooting replacement requires Release benchmark evidence and complete migration of affected paths.

## Numeric behavior, failures, and exceptions

AOT preserves checked integer overflow, division by zero, indexing, conversion, string, and container failures. LLVM integer operations must not turn language errors into poison or undefined behavior. Check zero divisors and signed overflow before division, and lower shifts and conversions according to their language contracts. Floating-point operations preserve NaNs, infinities, subnormals, signed zero, and Vela's floating-point division-by-zero error. Fast-math and transformations changing these behaviors are disabled by default.

The first implementation uses explicit status propagation, not C++ exceptions or `longjmp`. Fallible calls return status and a success payload; GC-traced exception envelopes retain exception payloads. Callers follow IR exceptional successors to a handler or clean up their frame and propagate failure outward. Only proven non-failing internal calls may omit the error channel.

The [exception contract](exceptions.md) remains unchanged: catch matching order, finally replacing pending completion, preservation of original throw traces, explicit rethrow origins, and rollback of failed lazy initialization. Failure to allocate an exception envelope returns `OutOfMemory` directly rather than recursively trying to allocate another error. Successful execution allocates no exception or trace objects.

## Program images, instances, and module state

AOT artifacts contain machine code, immutable descriptors, and relocations. Descriptors record function and type identity, fields and dispatch definitions, UTF-8 literal bytes, floating-point bit patterns, native imports, global initializers, and source positions. They must not embed compilation-process heap pointers, Isolate addresses, C++ object snapshots, or host callback addresses.

The same read-only code can serve multiple Isolates. Each instantiation creates independent layout bindings, rooted strings, native import tables, and global state. Handles of one instance share that state; new instances do not. Code identity and program-local type IDs jointly determine nominal identity, preventing compilation-unit collisions. Inheritance and interface dispatch continue to use resolved slots.

Instantiation validates target and ABI, resolves all native imports, establishes layouts, and allocates constants after registering roots. Publish the instance handle only after complete success. Failure releases temporary roots, constants, and layout transactions instead of leaving a partially usable instance. User globals retain [module](modules.md) lazy initialization, `CyclicInitialization`, and failure retry behavior; they do not become process-start C++ static initializers.

External instance/function handles retain required code descriptors and roots. Internal async references trace globals and constants through Future graphs without permanently rooting unreachable suspended tasks. The Isolate outlives instances, managed handles, continuations, and native producers; destruction and root operations stay on its thread. Code is statically linked into the process in this release, with no runtime unloading.

## Host binding and dynamic calls

Source `external func` declarations suffice for offline checking and import generation. Building a library does not run the host or create an Isolate. Instantiation resolves logical symbols and exact signatures once against registered host implementations, rejecting missing or mismatched imports before publishing the instance. Hot calls use resolved entries without repeated string lookup.

Signature descriptors and codecs used by `NativeFunction::Bind` are the common source. The same template binding must generate the VM register adapter and a typed AOT ABI thunk. These serve two explicit execution backends, not fallback paths for each other. AOT does not construct a VM argument window or assume an arbitrary C++ function address is compatible. The registry supplies signature-specific thunks to generated code; native failures, UTF-8 checks, Future payload validation, and thread ownership remain shared.

Standalone startup registers only standard bindings supplied by the distribution. Initially, building an executable requiring custom host bindings fails at build time. Such applications emit libraries, and the host registers bindings before instantiation. Any later host-object input feature must define a registration entry and signature manifest; a system linker finding a same-named symbol is insufficient.

Closures, bound methods, interface calls, dynamic member access, and Future callbacks must invoke native targets. Shared metadata governs code identity and signature checks; the native backend supplies execution entries. Dead-code removal computes reachability from selected exports, main, initializers, dynamic members of reachable types, closures, and async resume entries. It cannot follow only static direct-call edges. This release rejects VM function values supplied as AOT callbacks at the host boundary.

## Async lowering

Reuse `FutureScheduler`, Future state, the host mailbox, and timers. AOT compiles an async function into an initial entry and native continuations dispatched by resume state. Continuations retain code/instance identity, a resume ID, live locals, exception state, and await origins. Resume IDs identify generated native blocks, not bytecode PCs.

Execute the body immediately up to the first await; completion without await creates no suspended frame. Every await is an asynchronous boundary, including ordinary values and completed Futures. Queuing, notification order, adoption, error handling, cancellation, and idle behavior follow the [async contract](async_tasks.md). Await failure edges participate in liveness, and catch/finally across await preserve pending exceptions or returns.

Suspension saves only state needed for resumption, with precise GC tracing and the existing scheduler counters for suspended frames and saved bytes. Reuse Future reference tracing where appropriate, but do not capture permanently rooted handles that make unreachable task cycles unreclaimable. Establish native roots before releasing or updating saved state on resume. Resuming one task does not scan the entire program.

Future composition currently implemented in VM execution must be split by responsibility: shared composition and scheduling semantics become runtime services usable by both backends; bytecode frame resumption remains in VM; native frame resumption belongs to AOT. Neither duplicating Future semantics nor calling the VM executor from a native continuation is acceptable.

## Object files, linking, and version identity

Default code generation uses the native architecture's baseline ISA rather than build-machine-specific CPU instructions. Target descriptions specify object format, calling convention, data layout, relocation mode, minimum OS version, and runtime variant. Relocatable code requirements for deployment come from target configuration, not accidental host-compiler defaults.

Object files embed descriptors and an instantiation entry. The JSON manifest is for build tools; runtime execution does not require the sidecar. It records format version, compilation-unit identity, toolchain build identity, runtime ABI identity, target, export/import signatures, required libraries, and link order. Static libraries do not embed another Vela runtime. Hosts link one matching runtime and explicitly reference the instantiation entry so archive extraction and section removal retain required descriptors.

The first release requires an exact runtime ABI match and promises no compatibility across releases. ABI identity covers object layouts visible to generated code, rooting, helper signatures, calls, and exception protocols. LLVM version and optimization settings additionally contribute to toolchain build identity. Versioned link symbols and instantiation checks reject mismatches; calls do not repeat those validations. Multiple compilation units cannot combine incompatible runtimes or share unchecked layouts.

The linker driver invokes packaged tools by absolute path, constructing structured arguments or response files with correct UTF-8 path handling. Target configuration supplies startup objects, runtime, system libraries, and necessary C/C++ support libraries without using Clang as a driver. Emit and link in temporary locations before publishing outputs; compilation or linking failures do not overwrite valid old artifacts, and single-file publication uses atomic replacement. Publish a library's manifest last, with consistency identities covering its associated outputs. Consumers reject mixed build generations; the driver does not claim that several files can be atomically renamed together. Report publication I/O errors explicitly; any resulting mixed files cannot be consumed as a successful build.

There is no new persistent AOT compilation cache in this release. The [bytecode cache](compilation_cache.md) is not a native-code cache. Any future cache must cover source/import resolution, complete toolchain identity, target/CPU, optimization, runtime ABI, SDK/link inputs, and exports. LLVM bitcode is not a public cross-version distribution format.

## Toolchain distribution and system dependencies

```text
distribution (logical contents)
  bin/vela                    compiler with required LLVM backends statically linked
  libexec/                    pinned linker and archive tools
  lib/<target>/               Vela runtime, startup support, redistributable dependencies
  include/                    embedding interfaces and ABI wrapper declarations
  share/vela/                 target configuration, build identity, licenses
```

LLVM and LLD versions are pinned with the distribution, including only backends needed by its supported matrix. Compiling Vela source does not require Python, GN, Ninja, LLVM development packages, or Clang. Developing the Vela compiler still follows the existing [DEPS/GN/vl toolchain](toolchain.md); new dependencies and license records use that path without modifying the user's global PATH.

| Platform | Shipped components | External platform prerequisites |
| --- | --- | --- |
| Linux x64 | Integrated LLVM, ELF linking/archive tools, Vela runtime and startup support | A fixed supported libc/sysroot baseline; legally redistributable startup and link inputs ship with target support and are tested without development tools |
| Windows x64 | Integrated LLVM, COFF linking/archive tools, matching Vela runtime | Explicit sources and minimum versions for Windows SDK, UCRT, and the selected C++ runtime; no assumption that Visual Studio or Clang is installed |
| macOS arm64 | Integrated LLVM, validated Mach-O linking/archive tools, Vela runtime | Apple SDK and platform-required signing tools, usually obtained through Apple Command Line Tools; Vela does not invoke the Clang frontend even if that package includes it |

Each target support manifest records whether every system input comes from the distribution or SDK, how it is located, supported versions, and redistribution rights. SDKs may be configured explicitly or discovered through platform-defined locations. Do not silently switch to other compilers/linkers on PATH. Missing or unsupported inputs produce a pre-compilation diagnostic with installation/configuration instructions. A package is not officially supported until its SDK, linking, signing, and clean-environment tests pass.

Object-only generation does not require final system link inputs. Hosts embedding a Vela library in C++ still need their own C++ development environment; that does not contradict compiling Vela without separately installing Clang/LLVM. Generated application dynamic dependencies must be listed in the distribution manifest. Statically linking the Vela runtime does not imply an application without system dynamic-library dependencies.

Cross-compilation is a later extension requiring target code generation, runtime, linker capability, SDK/sysroot, and a test environment together. Enabling an LLVM target alone is not complete support.

## Diagnostics and verifiability

Compiler errors retain the existing source diagnostic format. Runtime error codes, original throw sites, call positions, and await boundaries do not depend on enabling LLVM debug information; native logical-position mappings are stored independently. Optional DWARF/CodeView information serves platform debugging tools, without claiming that bytecode breakpoints or stepping protocols already support AOT. Optimization preserves enough origin information to explain inlined positions.

Native objects and executables are trusted code artifacts. ABI checks are neither a sandbox nor producer authentication. First execution does not read source, invoke the compiler, or compile code with a JIT.

## Implementation stages and completion criteria

These stages define dependencies. A working integer example is not complete AOT delivery.

| Stage | Scope | Completion criteria |
| --- | --- | --- |
| A1: shared compilation boundary | Separate common optimization, VM allocation, offline descriptions, and instantiation; complete type/effect facts | Full VM regression passes; AOT preparation creates no Isolate and is independent of bytecode encoding limits |
| A2: synchronous native execution | LLVM objects, internal ABI, instantiation, roots, direct synchronous execution, exceptions, host binding | Host linking and execution work; GC stress and corresponding synchronous semantic comparisons pass without linking VM |
| A3: complete async and dynamic paths | Native continuations, all Future composition, closure/interface/dynamic calls, async main | All async contracts, host-thread completion, and unreachable-cycle collection pass, with no VM callback execution |
| A4: user builds and distribution | exe/obj/staticlib, export wrappers, manifests, version checks, three platform packages and diagnostics | Clean environments without separate Clang/LLVM installations can build, link, and run; SDK prerequisites and licenses are auditable |
| A5: release acceptance | Full regression, static checks, Release performance, package dependency checks | The matrix below passes, obsolete coupling and duplication are removed, and unfinished work is not hidden by release claims |

Acceptance matrix:

- Language comparisons: execute the same valid programs through VM/AOT, comparing values, output, side-effect order, error codes, and source traces. Cover module initialization, inheritance, generics, Expected, dynamic values, and every implemented standard-library feature. Compare NaNs and nondeterministic APIs by contract rather than coincidental random numbers or wall-clock values.
- GC and ownership: stress every collection boundary, including result handoff, GC inside hosts, shared mutable captures, array growth, exceptions/finally, multiple Isolates, suspension/resumption, and result cycles. Verify root cleanup, layout transactions, and suspended-state counters after exit.
- Async: synchronous first-segment effects, awaiting ordinary values/completed Futures, FIFO, adoption, composition failures, cancellation, timers, background errors, external-thread completion, and idle, with no AOT-specific scheduling order.
- ABI and linking: mismatched targets/versions/signatures, missing imports, multiple instances, multiple static libraries sharing one runtime, archive removal, Unicode/space paths, preservation of previous outputs on compilation/linking failure, publication I/O failures, and rejection of mixed manifests.
- Distribution: remove separately installed Clang/LLVM and undeclared build tools in clean environments for each target, then build and run with the published SDK prerequisites. Remove source and the Vela toolchain and run the artifacts again. Inspect binary dependencies and link maps to establish that LLVM, compiler, and VM executor dependencies are absent.
- Performance: compare Release AOT, VM, and semantically equivalent C++ arithmetic, calls, fields, arrays, host bindings, GC, and async workloads. Record compilation, linking, first/steady execution, toolchain/artifact size, RSS, roots, and suspended-state costs separately. Preserve evidence under the [benchmark retention rules](../../.agents/benchmark_results.md); do not promise speed multipliers in advance.

Required language paths must not retain “run this in VM when unimplemented” branches. When moving shared services, remove VM-private copies with no remaining callers, duplicate metadata, and adapters needed only by the old binding arrangement. Execution implementations still required by VM remain owned by VM.
