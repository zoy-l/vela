# Vela language architecture

[**English**](architecture.md) | [简体中文](../zh-CN/architecture.md)


Vela is a statically typed scripting language for native applications. Its
native AOT and JIT share Typed IR and native code generation. Development
mode supports runtime script loading, stateful Hot Reload, and Reload through a
new script Isolate. See the [JIT and hot reload contract (Chinese)](../zh-CN/hot_reload.md)
for feature, performance, and platform acceptance scope. The shared runtime owns
values, GC, async operations, and host integration.

## Source model

The source language uses C-style blocks, semicolons, and operators, with postfix
type annotations. Top-level functions use `func name(parameter: Type): Return`;
class methods omit `func`, and fields use `name: Type`. Locals use `let` or
`const`, require an initializer, and may infer their exact type from that value.
Parameters, return types, and class fields require annotations. `const` prevents
rebinding; mutation of an object's fields or array elements remains allowed.
String contexts format primitive values and null automatically; other type
boundaries remain strict. There are no legacy prefix declarations. Function
types contain parameter types only, such as `(int, int) => int`; typed arrow
functions support lexical captures with shared mutation of captured `let`
bindings. [P1 contracts](core_language.md) specify closures, construction,
interfaces, generics, matching, and ordered collections.

Control flow includes switches with shared empty labels and explicit non-empty branch exits. Explicit nullable types
use `T?` and flow-sensitive non-null checks. Scope, declaration order, and
source diagnostics are specified in [Basic language contracts](language_basics.md).

The initial class model is deliberately smaller than C++:

- `class` declares a nominal object layout with statically known fields and
  methods;
- classes support single inheritance, explicit `super` constructor chaining,
  `override` method replacement, and runtime dispatch through multiple interfaces;
- a class construction expression has no `new` keyword;
- named construction arguments are resolved to public instance fields or explicit constructor parameter
  IDs at compile time;
- enum members always name their enum explicitly (`Brightness.light`), including
  constructor arguments, declarations, returns, assignments, and array elements;
- fields and methods are statically declared, so runtime member lookup is not a
  string-table operation on the hot path.

For example:

```text
enum Brightness {
    light,
    dark,
}

class CupertinoThemeData {
    brightness: Brightness;
}

class ActionSheetExample {
}

class CupertinoApp {
    theme: CupertinoThemeData;
    home: ActionSheetExample;
}

func buildApp(): CupertinoApp {
    return CupertinoApp(
        theme: CupertinoThemeData(brightness: Brightness.light),
        home: ActionSheetExample(),
    );
}
```

The surface syntax resembles a data constructor, but construction still has a
precise runtime contract. The first runtime representation uses an opaque
isolate-owned heap reference and a class layout descriptor. A later `struct` feature can add
inline value semantics without changing the constructor grammar.

## Compilation pipeline

```text
source -> lexer/parser -> name resolution and type checking
       -> Typed IR -> shared IR optimization -> native LLVM lowering
       -> JIT linking and instantiation, or AOT artifact publication
```

The frontend solves generic arguments through shared constraints, then
materializes and checks concrete declarations. Explicit and inferred calls share
canonical specialization identities. Resolved types retain structural
element/signature/argument relationships; lowering does not recover them by
parsing type-name suffixes.

The frontend records control-flow facts for fallthrough, break and continue.
Non-void functions cannot reach their closing brace; infinite-loop paths need no
return. Unreachable statements are errors. Boolean literals and logical
operators refine reachability; lowering preserves condition side effects while
omitting unreachable jumps and returns.

Typed IR retains source locations, resolved calls, field identities, exception
edges and runtime type descriptions. Shared optimization performs basic-block
copy and constant propagation, jump threading, and dead-value/unreachable-block
elimination. Observable failures and side effects remain when removal cannot be
proven valid. LLVM optimization and [escape analysis](../zh-CN/escape_analysis.md)
operate after this semantic boundary.

## Native execution

`run`, source tests and standalone debugging use closed JIT snapshots.
`dev` and the REPL use reloadable native sessions. AOT emits objects, static
libraries or executables through the same native generator.
See [execution contracts](native_execution.md), [AOT](aot.md) and
[hot reload](../zh-CN/hot_reload.md) for their boundaries.

Baseline and optimized JIT preserve the same language behavior. The optimized
reloadable tier retains recovery information and baseline entries for active
frames. LLVM optimization is configured separately from the JIT tier.

Host functions have immutable signatures established at registration and checked
at instantiation. Generated native calls use the typed host ABI. Dynamic calls
perform their required argument/type checks before invoking a typed target.
Native declarations remain visible as logical frames even when C++ code triggers
GC. The Isolate owns runtime state; a JIT image's shared owner owns linked code.

## Memory and object boundary

One Isolate owns its heap, metadata, scheduler and host bindings. Closed native
instances use fixed class layouts. Reloadable objects preserve their addresses
while field storage can evolve through validated publication. Neither mode
exposes C++ object layout as a script object.

GC is precise, non-moving and incremental. Managed values live in explicitly
registered shadow roots, globals, retained activations and host handles.
Reference fields and array layouts identify what to trace. Stores preserve the
write barrier, and roots are revisited before sweep because active code can
change them between incremental steps. Small objects use size-class free lists.

The public `Value` is a 16-byte tagged value. Typed native locals can use unboxed
machine representations; arrays store compact native primitives or references.
Growing arrays replace backing buffers while retaining their GC object's identity.
Strings use valid UTF-8 with code-point indexing and a separate byte length;
immutable contents and mutable builder buffers follow the same heap accounting.

A `NativeInstance` roots its constants and program state. Closures, async
activations and development handles retain the versions they need; unreachable
state and code can retire together. Returned values are borrowed and must be
rooted before later allocation. See [embedding](native_embedding.md) and
[garbage collection](garbage_collection.md).

Async calls execute until return, error or await. Suspension retains live state
in a heap activation and resumes through the scheduler. Future state and GC are
owned by the Isolate thread. Host producers publish immutable completion messages
through an MPSC queue; endpoints can outlive shutdown and return `Closed`
without accessing retired runtime storage. Ordinary instance, Future and root
handles must be released before their Isolate is destroyed.

## Performance evidence

Measure compilation, first execution and steady execution separately in Release.
Use equivalent workloads and check every result. Report code and native frame
plans separately from GC live/committed bytes and process resident memory;
compiler frame plans are not measured physical stack sizes.

The benchmark suites compare JIT, both reloadable tiers, AOT and appropriate C++
or LuaJIT workloads. Historical measurements are comparable only when backend,
machine, toolchain, repetitions and boundaries match. Exact retention rules and
commands are in the [benchmark guide](../../benchmarks/README.md).
