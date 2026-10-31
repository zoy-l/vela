# Native AOT compilation design

[**English**](aot.md) | [简体中文](../zh-CN/aot.md)

> Status: A2/A3 native execution and the A4 `exe`/`obj`/`staticlib` interfaces are implemented. Windows x64 Debug/Release regression, native-only host linking, explicit-SDK package tests, static checks, and Release measurements pass. Linux/macOS target packages and clean-host acceptance remain open, so A4/A5 are not fully accepted and no official three-platform release is certified.

Vela AOT generates native object files from the shared Typed IR and uses linking tools shipped with the toolchain to produce executables or static libraries. LLVM is an internal compiler dependency; users of an official distribution do not install Clang or LLVM separately. Generated applications depend on the Vela runtime and target platform libraries, not LLVM or the Vela compiler.

This design covers compilation, native execution, GC, exceptions, async, embedding, linking, and distribution. Existing topic contracts remain authoritative for language semantics; AOT does not define a second language behavior. System SDK availability and licensing are separate from packaging LLVM. “No LLVM installation” does not mean every platform can build without a system SDK.

## User interface and scope

The standalone build interface is:

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

`vela build --verify-manifest=math.o.aot.json` validates the target, runtime/toolchain identities, binary/header fingerprints, and publication generation before a build tool consumes a library. It rejects mixed or modified files. The manifest records generic CPU/PIC code generation, optimization, function frame/root-slot metrics, signatures, and the matching target-support identity when available.

Executable and archive builds load `share/vela/target.json` relative to the installed compiler. `--target-support=FILE` selects an explicit configuration. Object emission needs no final SDK or linker inputs. The configuration pins packaged tools and runtime by SHA-256 and records SDK versions, provenance, licenses, required inputs, and link arguments; tools are never selected from `PATH`.

Developers can create a Windows development package with `python -m tools.vl package release OUTPUT`. Its manifest supplies the exact MSVC and Windows SDK/UCRT versions and installation instructions. Set `VELA_MSVC_TOOLS` to the versioned MSVC tools directory and `VELA_WINDOWS_SDK` to the Windows Kits root specified by that manifest. The Visual Studio IDE is not required. SDK link inputs remain external to the package. Other host packages currently require an audited `--support-spec` describing their SDK/sysroot inputs and any bundled files; platform acceptance is still required.

The first release supports native compilation for the existing bootstrap host matrix: Windows x64, Linux x64, and macOS arm64. Each platform must pass its own distribution acceptance before support is advertised. Native dynamic-library loading, ABI compatibility across releases, independently distributed binary modules, and cross-version native calls are outside this AOT delivery scope. JIT and hot reload are provided by [development mode (Chinese)](../zh-CN/hot_reload.md). REPL and standalone debugging also use JIT/native execution. Unsupported AOT operations produce explicit diagnostics.

## Shared implementation and AOT responsibilities

| Shared implementation | AOT responsibility |
| --- | --- |
| [IR](../../compiler/ir/ir.h) uses 32-bit virtual register IDs, types, source locations, and exception handlers; [shared analysis](../../compiler/ir/analysis.h) supplies instruction effects and liveness across exception and resume edges | Generate typed native values and root slots from value-definition types, using native frame and metadata limits |
| [Compilation preparation](../../compiler/ir/preparation.cc) runs the frontend, layout description, and common optimization offline, returning IR that borrows neither AST nor Isolate state | Branch from the same IR into shared LLVM lowering and code generation |
| [Layout instantiation](../../runtime/isolate/program_layouts.cc) consumes pointer-free descriptions; binding checks consume concrete signatures | Reuse layout instantiation for native instances, which own native imports, constant roots, and global state |
| [NativeFunction](../../runtime/native/native_function.h) derives direct and checked native entries from C++ signatures | Reuse declarations and codecs, selecting direct or checked AOT entries by signature |
| The [Future scheduler](../../runtime/async/future.h) owns shared state and scheduling semantics; native activations own suspended execution | Use native continuations and callback entries through the shared native runtime |
| [GC](garbage_collection.md) provides precise roots, a non-moving heap, and reference write barriers | Generate native root slots and safepoints, reusing the heap and barriers without conservative stack scanning |

These production paths are implemented. Remaining target support packaging and release acceptance are recorded in this document's status and completion criteria.

## Pipeline and module responsibilities

```text
source and import graph
  → parsing, module binding, generic specialization, static checking
  → Typed IR and common optimization
      → shared native lowering → LLVM IR → LLVM optimization and target code generation
                               → object files → archiving or linking
```

`compiler/frontend` owns language semantics; `compiler/ir` owns shared lowering, control flow, and semantic optimization. `compiler/native` supplies shared LLVM conversion, root-slot planning, and native ABI calls for AOT and JIT. `compiler/aot` owns closed-program optimization, exports, and object emission. The build driver selects platforms, links, archives, and publishes artifacts. LLVM headers and types stay inside the compiler's native backends; runtime does not depend on LLVM.

`runtime` supplies objects, GC, Futures, errors and execution services. Native support
owns instances, calls and source traces. Runtime has no dependency on frontend,
IR or LLVM. The CLI links JIT and AOT compiler components; a standalone AOT
application links only the runtime components needed by its code. Both compilation
pipelines consume shared IR preparation directly.

Current IR is a virtual-register instruction representation with reassignment, not LLVM IR already converted to SSA. AOT lowering must construct correct SSA values or promotable typed local slots for definitions, merges, and loops. Exception successors and await resume edges participate in dataflow analysis. Complete types, nullability shapes, callable signatures, and reference kinds must come explicitly from upstream, not guesses based on type-name strings or runtime values.

Shared operation facts must cover failure, allocation, GC, host calls, and suspension for optimization, liveness, and both backends. Extend the existing analysis responsibility rather than maintaining inconsistent semantic operation tables per backend. Calls not proven free of GC must be treated as capable of collection; only leaf operations with a proven contract may omit rooting.

IR values and metadata use their full-width identities. Native generation checks
its own argument/frame, object-format and metadata limits explicitly, rather than
relying on LLVM failure. See [execution limits](native_execution.md).

## Value representation and native ABI

Ordinary local `int`, enum ordinal, `double`, and `bool` values use 64-bit integers, 64-bit integers, binary64, and boolean representations respectively. Non-null references use non-moving heap pointers. Nullable primitives carry independent validity information; no valid numeric value is reserved as a sentinel. Heap fields, arrays, dynamic values, closure captures, and runtime type identity retain the existing object model. Unboxed locals do not imply a different heap layout.

Function descriptors mark enum results whose native scalar payload needs boxing at the runtime boundary. Other results bypass boxing dispatch. Native value codecs copy only the shared payload/tag prefix, preserve payload bits, and zero the ABI reserved fields when encoding.

Heap `Value` slots use an explicit 16-byte payload/tag representation shared by C++, GC, and generated code. The AOT backend derives offsets from `NativeMemoryLayout`, `sizeof`, and `offsetof`; it does not inspect a standard-library variant representation. Primitive arrays retain their compact element storage; nullable primitives and dynamic elements use tagged slots, while references use pointer slots. Runtime ABI fingerprints cover these layout definitions and reject artifacts from a different build.

Array access instructions retain the array-layout index established during lowering. Erasing a temporary to `dynamic` changes its register representation without discarding the element storage facts needed to initialize or read the array. Loads and stores convert between that storage shape and the destination or source register representation.

Native calls have three boundaries:

| Boundary | Contract |
| --- | --- |
| Within a compilation unit | Typed arguments and results, with a hidden execution context for the instance, Isolate, and execution state; the compiler controls this private calling convention |
| Generated code to runtime | A versioned internal ABI with explicit fixed-width fields, pointers, layouts, alignment, results, and error channels; no reliance on C++ name mangling or standard-library object layouts |
| Host exports/imports | Generated C ABI wrappers and a type-safe C++ facade; raw machine-code entries are not public C++ functions |

Export wrappers take an explicit instance, return a status, and write a successful result to an output. Primitives use fixed-width representations. Managed results such as strings, objects, and Futures are delivered through rooted handles with Isolate ownership, explicitly released by the host. Structural signatures still participate in composite-value validation; object interiors are not exposed as host structs. Public wrappers must not expose the binary layouts of `std::string`, `std::expected`, or `std::shared_ptr`. Low-level `ExecuteNativeFunction` results remain borrowed and must not be mistaken for newly rooted export results.

Native execution support maintains logical call frames and source positions independently of machine-register allocation. Preserve the current call-depth policy and `StackOverflow` status, checking before entering another script call. Inlining must not accidentally alter observable depth or traces. Native frame size must be bounded; large temporary storage uses runtime-managed allocation that can report failure, preventing user source from causing unchecked native-stack exhaustion. Normal, error, and suspension exits clean up frames and root records consistently.

Logical frame records live on the native stack and form a linked stack in the execution context. Generated code registers positions and depth directly, without allocating a resource frame for scalar calls. Small frames place roots on the native stack within the same size budget as locals and scratch buffers; larger storage retains explicit allocation failures. Typed locals remain separate from ABI scratch buffers whose addresses escape, allowing LLVM to promote locals to SSA. Calls, explicit throws, and async boundaries publish their position before runtime execution; pure checks record it only on failure.

Stack roots register through the existing `RootRange` without constructing an owning resource frame. Large frames use an owning resource frame for allocated roots and temporary storage, with explicit allocation failure. Generated code writes and clears root slots directly, including when restoring async state, without a runtime call per slot. These addresses remain stable across nested calls and expire at frame exit. Logical frames retain the same call-depth budget and exception traces independently of resource storage.

Functions with scalar results, bounded stack storage, no handlers, and no operations that can collect on success check the call-depth limit at entry and materialize their logical frame only on failure. They may read or write fields, arrays, and globals. On failure, generated code packs live references into stack slots; the failure helper registers that range before allocating the exception envelope and releases it with the temporary logical frame. Global reads qualify only when no managed locals require roots across possible lazy initialization; the initializer helper temporarily registers the caller's frame. Deferred positions use a 64-bit scalar with the function index in the high 32 bits and instruction index in the low 32 bits. Failure checks and lazy global reads create no position globals. Native ABI version 2 covers these scalar helper signatures; older objects require recompilation. Other functions register logical frames for their entire execution. Entry preparation clears pending failures only at the outermost call.

Plain function values and closures created in the current function can call their typed bodies directly when single definitions and copies establish an immutable target and argument/result types match. Closure calls retain their implicit capture object. Values obtained from parameters, globals, fields, or potentially reassigned locals retain runtime target and origin-instance resolution. Direct calls preserve logical depth checks, error positions, captures, and callback side effects. Array callback mutation checks compare the receiver's structural version directly after the callback; snapshot sorting and stable tie ordering retain their existing contracts.

## GC roots, safepoints, and write barriers

The first implementation uses explicit shadow root slots backed by existing tagged `Value` storage and `RootRange`. Only managed references live across safepoints need roots; pure numeric values remain in machine registers or ordinary local slots. Registered slots must contain valid values: ordinary frames initialize them to null, while deferred leaf failure ranges contain only the packed live references. Unregister before function exit. Slots can be reused over reference live intervals and are cleared when references die, avoiding retention merely because the frame remains active.

Safepoints include runtime calls that may allocate or advance GC, script calls that can reach those operations, and host calls. GC continues to advance cooperatively on the owning Isolate thread. Pure arithmetic loops do not introduce asynchronous collection or new task scheduling points. Any later explicit polling must participate in the same effect and root analysis.

Shared effects distinguish collection on success from allocation of an error envelope. An instruction that can collect only on failure stores live roots in its failure block before creating that envelope. Its successful path requires no root stores for that instruction.

Generated code must obey this ordering:

1. Before a safepoint, store every live reference, including arguments, receivers, closure environments, pending exceptions, and return values saved across finally.
2. No allocation omitted from the analysis may occur between root registration and the call. Caller roots cover a runtime helper's entire use of borrowed references.
3. After a reference return, the caller stores it in a registered root slot before another safepoint. GC cannot occur between callee root release and caller receipt. Host export wrappers complete the rooted-handle handoff before returning.
4. Suspension may unregister native frame roots only after the continuation state is traced through a reachable Future graph.

Root-slot addresses and registration records must remain visible to scanning. LLVM aliasing, memory effects, and call attributes must describe this accurately. Helpers that can observe roots cannot receive incorrect attributes such as `readnone`; optimization cannot delete root stores or move them across safepoints. GC stress validation must exercise optimized machine code.

Reference updates to fields, arrays, collections, globals, capture cells, and continuations obey existing barriers during both marking and sweeping. Typed field, array, and global accesses emit direct loads and stores. Assigning a global marks its slot Ready without invoking its initializer. Array accesses retain an unsigned bounds check that rejects both negative and oversized indices; non-null reference writes call the shared GC barrier, while null and scalar writes require none. Array append writes directly when capacity is available and uses the existing heap growth operation when full, reloading the backing pointer afterward. A non-moving object does not make a growable array's backing buffer immovable; element addresses cannot survive calls that may grow it.

Single-element `push(value)` lowers to the shared scalar append operation, avoiding a temporary one-element array in both backends. Multiple-element append retains ordered argument evaluation and batch insertion.

These choices follow the explicit memory operations used by [Luau native code generation](https://github.com/luau-lang/luau/blob/master/CodeGen/src/IrLoweringA64.cpp). Further bounds and overflow-check elimination requires range and control-flow proofs, as in [Go SSA analysis](https://github.com/golang/go/blob/master/src/cmd/compile/internal/ssa/prove.go) and [Dart range analysis](https://github.com/dart-lang/sdk/blob/main/runtime/vm/compiler/backend/range_analysis.cc). Checked integer arithmetic, failure positions, observable writes, and logical call depth remain language contracts; enabling LLVM optimization alone does not establish those proofs.

This release does not add conservative scanning, LLVM stack maps, or another collector. A later rooting replacement requires Release benchmark evidence and complete migration of affected paths.

## Numeric behavior, failures, and exceptions

AOT preserves checked integer overflow, division by zero, indexing, conversion, string, and container failures. LLVM integer operations must not turn language errors into poison or undefined behavior. Check zero divisors and signed overflow before division, and lower shifts and conversions according to their language contracts. Floating-point operations preserve NaNs, infinities, subnormals, signed zero, and Vela's floating-point division-by-zero error. Fast-math and transformations changing these behaviors are disabled by default.

Release optimization analyzes integer loops in LLVM SSA. Unit increments and decrements guarded by strict signed bounds cannot cross an `int64` endpoint, so their overflow checks can be removed. For a loop with one accumulator and no side effects, when the terms retain one sign or remain invariant, accumulation uses exact `i128` arithmetic followed by an `int64` range check at the normal exit. LLVM loop optimization then computes exit values and removes the loop. The wide arithmetic is independently proven safe for up to `2^64-1` iterations. Zero iterations preserve the initial value, and failures retain the original accumulation statement's error and source position. Reductions whose terms cross zero without a proof of prefix safety, other potentially failing operations, and loops whose intermediate state is observable on exceptional paths retain per-iteration checks.

Unit field increments and decrements in a strict counted loop can be folded when the object is invariant and no other observable work occurs in the loop. The summary computes the final value in `i128`; on overflow it first writes the last successful `int64` boundary value, then enters the original failure block. Aliases and finally handlers therefore observe the same field prefix. Loops that expose the intermediate index or perform other side effects retain iteration semantics.

The first implementation uses explicit status propagation, not C++ exceptions or `longjmp`. Fallible calls return status and a success payload; GC-traced exception envelopes retain exception payloads. Callers follow IR exceptional successors to a handler or clean up their frame and propagate failure outward. Only proven non-failing internal calls may omit the error channel.

The [exception contract](exceptions.md) remains unchanged: catch matching order, finally replacing pending completion, preservation of original throw traces, explicit rethrow origins, and rollback of failed lazy initialization. Failure to allocate an exception envelope returns `OutOfMemory` directly rather than recursively trying to allocate another error. Successful execution allocates no exception or trace objects.

## Program images, instances, and module state

AOT artifacts contain machine code, immutable descriptors, and relocations. Descriptors record function and type identity, fields and dispatch definitions, UTF-8 literal bytes, floating-point bit patterns, native imports, global initializers, and source positions. They must not embed compilation-process heap pointers, Isolate addresses, C++ object snapshots, or host callback addresses.

The same read-only code can serve multiple Isolates. Each instantiation creates independent layout bindings, rooted strings, native import tables, and global state. Handles of one instance share that state; new instances do not. Code identity and program-local type IDs jointly determine nominal identity, preventing compilation-unit collisions. Inheritance and interface dispatch continue to use resolved slots.

Instantiation validates target and ABI, resolves all native imports, establishes layouts, and allocates constants after registering roots. Publish the instance handle only after complete success. Failure releases temporary roots, constants, and layout transactions instead of leaving a partially usable instance. User globals retain [module](modules.md) lazy initialization, `CyclicInitialization`, and failure retry behavior; they do not become process-start C++ static initializers.

Generated global reads inspect the instance's fixed initialization-state storage and read Ready values directly from their existing GC slots. Other states call the runtime initializer; failed initialization restores Uninitialized for retry. Global writes retain the shared write barrier and publish Ready afterward. These paths use offsets derived from C++ layout facts without exposing standard-library container layouts.

External instance/function handles retain required code descriptors and roots. Internal async references trace globals and constants through Future graphs without permanently rooting unreachable suspended tasks. The Isolate outlives instances, managed handles, continuations, and native producers; destruction and root operations stay on its thread. Code is statically linked into the process in this release, with no runtime unloading.

## Host binding and dynamic calls

Source `external func` declarations suffice for offline checking and import generation. Building a library does not run the host or create an Isolate. Instantiation resolves logical symbols and exact signatures once against registered host implementations, rejecting missing or mismatched imports before publishing the instance. Each native function invocation caches the immutable import targets and calling conventions it directly uses, including across loops. LLVM can move selection between direct and checked calls outside a loop.

Signature descriptors and codecs used by `NativeFunction::Bind` are the common source for typed JIT/AOT entries. Hosts may include or omit a leading `NativeContext&`. Plain scalar results use register returns; exact scalar signatures retain the original function pointer, and boolean or managed arguments use ABI codecs. Fallible or managed results use a checked entry with status propagation and a caller output slot. Both conventions preserve logical frames, depth limits and GC roots. native failures, UTF-8 checks, Future payload validation and thread ownership remain shared.

Standalone startup registers only standard bindings supplied by the distribution. Initially, building an executable requiring custom host bindings fails at build time. Such applications emit libraries, and the host registers bindings before instantiation. Any later host-object input feature must define a registration entry and signature manifest; a system linker finding a same-named symbol is insufficient.

Closures, bound methods, interface calls, dynamic member access, and Future callbacks must invoke native targets. Shared metadata governs code identity and signature checks; the native backend supplies execution entries. Dead-code removal computes reachability from selected exports, main, initializers, dynamic members of reachable types, closures, and async resume entries. It cannot follow only static direct-call edges. A callback must belong to the receiving Isolate and compatible native code identity.

Internal `dynamic` values and signatures do not by themselves retain every class
method. Dynamic constraints crossing selected exports or native imports propagate
through fields, arrays, collections, callbacks, and Future payloads. Unknown host
values and operations that resolve arbitrary dynamic calls conservatively retain
their member closure. Function/member pruning preserves type and layout indices.

## Async lowering

Reuse `FutureScheduler`, Future state, the host mailbox, and timers. AOT compiles an async function into an initial entry and native continuations dispatched by resume state. Continuations retain code/instance identity, a resume ID, live locals, exception state, and await origins. Resume IDs identify generated native blocks, within the compiled function.

Execute the body immediately up to the first await; completion without await creates no suspended frame. Every await is an asynchronous boundary, including ordinary values and completed Futures. Queuing, notification order, adoption, error handling, cancellation, and idle behavior follow the [async contract](async_tasks.md). Await failure edges participate in liveness, and catch/finally across await preserve pending exceptions or returns.

Suspension saves only state needed for resumption, with precise GC tracing and the existing scheduler counters for suspended frames and saved bytes. Reuse Future reference tracing where appropriate, but do not capture permanently rooted handles that make unreachable task cycles unreclaimable. Establish native roots before releasing or updating saved state on resume. Resuming one task does not scan the entire program.

The first suspension creates one result Future and one activation with fixed storage sized for the largest suspension point. Later awaits reuse both; completion or failure settles that result directly. Returning another Future retains the adoption boundary. Resume code reads saved values through a shared ABI layout and reconstructs constants instead of saving them. Proven async/import result types eliminate redundant bindings within a basic block. Activation ownership flows through reachable Future references and queued observers, so unreachable pending graphs remain collectible.

Future composition and scheduling live in shared runtime services. Native
activations resume generated entries while retaining their code, roots and error
state. JIT and AOT use the same completion, adoption and ordering semantics.

## Object files, linking, and version identity

Default code generation uses the native architecture's baseline ISA rather than build-machine-specific CPU instructions. Target descriptions specify object format, calling convention, data layout, relocation mode, minimum OS version, and runtime variant. Relocatable code requirements for deployment come from target configuration, not accidental host-compiler defaults.

Object files embed descriptors and an instantiation entry. The JSON manifest is for build tools; runtime execution does not require the sidecar. It records format version, compilation-unit identity, toolchain build identity, runtime ABI identity, target, export/import signatures, required libraries, and link order. Static libraries do not embed another Vela runtime. Hosts link one matching runtime and explicitly reference the instantiation entry so archive extraction and section removal retain required descriptors.

The first release requires an exact runtime ABI match and promises no compatibility across releases. ABI identity covers object layouts visible to generated code, rooting, helper signatures, calls, and exception protocols. LLVM version and optimization settings additionally contribute to toolchain build identity. Versioned link symbols and instantiation checks reject mismatches; calls do not repeat those validations. Multiple compilation units cannot combine incompatible runtimes or share unchecked layouts.

The linker driver invokes packaged tools by absolute path, constructing structured arguments or response files with correct UTF-8 path handling. Target configuration supplies startup objects, runtime, system libraries, and necessary C/C++ support libraries without using Clang as a driver. Emit and link in temporary locations before publishing outputs; compilation or linking failures do not overwrite valid old artifacts, and single-file publication uses atomic replacement. Publish a library's manifest last, with consistency identities covering its associated outputs. Consumers reject mixed build generations; the driver does not claim that several files can be atomically renamed together. Report publication I/O errors explicitly; any resulting mixed files cannot be consumed as a successful build.

AOT build does not use the [JIT object cache](compilation_cache.md).
An offline artifact cache would additionally need to cover complete source/import,
toolchain, target/CPU, optimization, ABI, SDK/link-input and export identities.
LLVM bitcode is not a public cross-version distribution format.

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

Package maintainers supply `--support-spec=FILE` for an audited SDK/sysroot. The JSON specifies `target`, `minimum_os`, `object_format`, `runtime_variant`, `application_dependencies`, `archive_arguments`, and `link_arguments`. Each `external_roots` entry identifies a named SDK root, its environment variable, exact version, source, license, and installation instructions. Each `files` entry records its expanded path, SHA-256, role, source, license, and redistribution terms. Optional `bundled_files` entries provide a `source_path` relative to the specification, a package-relative `destination`, source, and license; the packager copies and fingerprints those files. Arguments use `{package}`, named SDK roots, `{object}`, `{output}`, and `{runtime}`. SDK files with role `link` are required only for executables; object and archive emission remain independent of final SDK inputs.

Before publishing a package directory, the packager uses the staged compiler and declared SDK inputs to build and run an async native application with build-tool and library-search environment variables cleared. Missing link inputs or a runtime ABI mismatch reject the package without leaving a published directory. This check establishes a usable local package; each platform still requires the full distribution acceptance matrix.

On each configured host, `VELA_TEST_TARGET_SUPPORT_SPEC` selects that audited specification for `python -m tools.vl test release tooling`. The distribution tests clear build-tool environment variables, use Unicode/space paths, compare JIT/AOT programs, link two libraries against one runtime, reject inconsistent publication, and rerun applications after removing sources and the package. Windows can instead use the built-in development SDK probe. A skipped platform test is not release acceptance.

Object-only generation does not require final system link inputs. Hosts embedding a Vela library in C++ still need their own C++ development environment; that does not contradict compiling Vela without separately installing Clang/LLVM. Generated application dynamic dependencies must be listed in the distribution manifest. Statically linking the Vela runtime does not imply an application without system dynamic-library dependencies.

Cross-compilation is a later extension requiring target code generation, runtime, linker capability, SDK/sysroot, and a test environment together. Enabling an LLVM target alone is not complete support.

## Diagnostics and verifiability

Compiler errors retain the existing source diagnostic format. Runtime error codes, original throw sites, call positions, and await boundaries do not depend on enabling LLVM debug information; native logical-position mappings are stored independently. Optional DWARF/CodeView information serves platform debugging tools, without promising the interactive JIT console protocol for separately linked AOT applications. Optimization preserves enough origin information to explain inlined positions.

Native objects and executables are trusted code artifacts. ABI checks are neither a sandbox nor producer authentication. First execution does not read source, invoke the compiler, or compile code with a JIT.

## Implementation stages and completion criteria

These stages define dependencies. A working integer example is not complete AOT delivery.

Windows acceptance exercises C++ runtime contracts, source-tool commands,
JIT/AOT language comparisons and linked native hosts. Release execution,
compilation, linking, package sizes, RSS, roots and continuation costs have distinct
measurement boundaries; see [aggregate evidence](../../benchmarks/results/rolling_summary.json).
These checks do not substitute for Linux/macOS execution or SDK/signing acceptance.

| Stage | Scope | Completion criteria |
| --- | --- | --- |
| A1: shared compilation boundary | Separate common optimization, offline descriptions, native generation and instantiation; complete type/effect facts | Shared language regression passes; AOT preparation creates no Isolate and follows native limits |
| A2: synchronous native execution | LLVM objects, internal ABI, instantiation, roots, direct synchronous execution, exceptions, host binding | Host linking and execution work; GC stress and corresponding synchronous semantic comparisons pass through the standalone native runtime |
| A3: complete async and dynamic paths | Native continuations, all Future composition, closure/interface/dynamic calls, async main | All async contracts, host-thread completion, and unreachable-cycle collection pass, with native callback execution |
| A4: user builds and distribution | exe/obj/staticlib, export wrappers, manifests, version checks, three platform packages and diagnostics | Clean environments without separate Clang/LLVM installations can build, link, and run; SDK prerequisites and licenses are auditable |
| A5: release acceptance | Full regression, static checks, Release performance, package dependency checks | The matrix below passes, obsolete coupling and duplication are removed, and unfinished work is not hidden by release claims |

Acceptance matrix:

- Language comparisons: execute the same valid programs through JIT/AOT, comparing values, output, side-effect order, error codes, and source traces. Cover module initialization, inheritance, generics, dynamic values, and every implemented standard-library feature. Compare NaNs and nondeterministic APIs by contract rather than coincidental random numbers or wall-clock values.
- GC and ownership: stress every collection boundary, including result handoff, GC inside hosts, shared mutable captures, array growth, exceptions/finally, multiple Isolates, suspension/resumption, and result cycles. Verify root cleanup, layout transactions, and suspended-state counters after exit.
- Async: synchronous first-segment effects, awaiting ordinary values/completed Futures, FIFO, adoption, composition failures, cancellation, timers, background errors, external-thread completion, and idle, with no AOT-specific scheduling order.
- ABI and linking: mismatched targets/versions/signatures, missing imports, multiple instances, multiple static libraries sharing one runtime, archive removal, Unicode/space paths, preservation of previous outputs on compilation/linking failure, publication I/O failures, and rejection of mixed manifests.
- Distribution: remove separately installed Clang/LLVM and undeclared build tools in clean environments for each target, then build and run with the published SDK prerequisites. Remove source and the Vela toolchain and run the artifacts again. Inspect binary dependencies and link maps to establish that LLVM and compiler dependencies are absent.
- Performance: compare Release AOT, JIT, and semantically equivalent C++ arithmetic, calls, fields, arrays, host bindings, GC, and async workloads. Record compilation, linking, first/steady execution, toolchain/artifact size, RSS, roots, and suspended-state costs separately. Preserve evidence under the [benchmark retention rules](../../.agents/benchmark_results.md); do not promise speed multipliers in advance.

Required language paths use the shared native runtime. Remove duplicate services,
metadata and adapters when their callers migrate. An unsupported operation is an
explicit compilation or execution failure, not an alternate execution path.
