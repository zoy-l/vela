# Vela language architecture

Vela is a statically typed scripting language for native applications. The
first execution path is a compiled register bytecode VM. Native AOT is a later
backend of the same typed intermediate representation. JIT is deliberately
outside the first implementation boundary and must not shape the runtime API
before a benchmark demonstrates that it is needed.

## Source model

The source language uses C/C++-style blocks, semicolons, declarations, and
operators. The initial class model is deliberately smaller than C++:

- `class` declares a nominal object layout with statically known fields and
  methods;
- classes are final in the first version; inheritance and virtual dispatch are
  later language work;
- a class construction expression has no `new` keyword;
- named construction arguments are resolved to field or constructor parameter
  IDs at compile time;
- `.member` enum shorthand is accepted only when the expected type uniquely
  identifies the enum or static member;
- fields and methods are statically declared, so runtime member lookup is not a
  string-table operation on the hot path.

For example:

```text
enum Brightness {
    light,
    dark,
}

class CupertinoThemeData {
    Brightness brightness;
}

class ActionSheetExample {
}

class CupertinoApp {
    CupertinoThemeData theme;
    ActionSheetExample home;
}

CupertinoApp buildApp() {
    return CupertinoApp(
        theme: CupertinoThemeData(brightness: .light),
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
source
  -> lexer and parser
  -> name resolution and static type checking
  -> typed control-flow IR
  -> IR optimization
  -> register bytecode
  -> Vela VM
```

Typed IR is the semantic boundary shared by bytecode and future native
backends. Bytecode is a separate execution representation optimized for dense
decoding and predictable register access; it is not used as the compiler's
general optimization graph.

The VM bootstrap now consumes the runtime root contract directly: each frame
register span is registered as a root range for the duration of execution.
Register slots stay contiguous, and allocation instructions can trigger GC
without copying or relocating frame values.

The current compiler path lowers checked integer expressions, local variables,
function calls, field access, assignments, enum ordinals, fixed-layout class
construction, and structured branches/loops through Typed IR into the bootstrap
bytecode. `vela run` exercises this path for a zero-argument `main` function.
Arrays and strings will use the same IR and bytecode contracts after their
typed storage representation is defined.

The compiler must retain source locations through parsing, type checking, IR
instructions, and bytecode ranges. This keeps diagnostics and future debugger
mapping independent from the VM instruction encoding.

## VM execution contract

The first bytecode format follows the properties that make Luau's interpreter
fast without requiring assembly:

- one fixed-width 32-bit instruction word for the common case;
- an 8-bit opcode and compact register operands, with explicit auxiliary words
  for wide constants and metadata;
- contiguous register slots in each call frame;
- type-specific arithmetic and comparison operations after static checking;
- direct function IDs for statically resolved calls;
- field IDs or fixed layout offsets for class access;
- no heap allocation for ordinary integer, float, boolean, or handle moves;
- constants and debug metadata stored outside the hot instruction stream.

The interpreter keeps the program counter, frame base, constant table, and
current instruction in local variables. Clang/GCC builds may use computed-goto
dispatch; a switch dispatcher remains the portable implementation and a
benchmark must justify any platform-specific path. Assembly is outside the
initial scope.

Calls into the host use an explicit registered function table. Each entry
owns a stable function ID, a static signature, and a typed adapter. The VM
call instruction selects the adapter by ID; it does not construct a generic
`Value*` array or perform string lookup on every call. A native backend can
lower the same typed call directly to the host function ABI.

## Initial optimization boundary

The first optimizer should implement only transformations with clear typed IR
contracts:

1. constant folding;
2. constant propagation and sparse conditional propagation;
3. dead instruction and unreachable block elimination;
4. jump threading and redundant move elimination;
5. direct call and field resolution;
6. basic-block local common-subexpression elimination when operands are pure.

The optimizer must not turn user-visible failure checks, object lifetime rules,
or host API ownership into speculative behavior. Every optimization is checked
against the typed IR and has a deterministic fallback in the compiler when it
cannot prove its precondition.

## Memory and object boundary

One VM isolate owns its heap and registered host references. Class instances
have stable handles and a fixed layout; field offsets are assigned once when a
class is compiled. The runtime does not expose C++ object layout directly to
scripts.

The runtime foundation uses a precise, non-moving, incremental mark-and-sweep
collector with an isolate-local allocation path. Each class layout records its
reference fields, so object marking can scan those fields directly without a
dynamic map lookup. VM frame slots, globals, and host handles must be registered
as explicit root ranges. Field stores go through a write barrier while marking,
and the gray work list is intrusive so collection does not allocate temporary
work buffers.

Small object allocations use size-class free lists. The bootstrap `Value`
representation is deliberately general; Typed IR and the VM will keep
primitive registers and typed arrays in narrower contiguous storage so ordinary
numeric execution does not enter the heap. The collector is single-threaded per
isolate; a host thread must cross an isolate-owned root boundary before it can
retain a script object.

Generational collection remains an optimization decision for the Release
benchmarks. It can be added behind this object, root, and barrier contract
without changing source-level lifetime semantics.
Generational collection and arena specialization are later implementation
optimizations, not alternate ownership models.

## Performance evidence

Every VM optimization is evaluated in Release builds with repeatable workloads:

- integer and floating-point loops;
- function calls and returns;
- contiguous array traversal;
- class field reads and writes;
- statically bound host calls;
- batched entity-style updates.

Each result records compile time, first execution, steady-state time, peak
resident memory, bytecode size, and instruction count where available. A native
C++ implementation provides the baseline; a Luau comparison is useful only
when the workload and observable semantics are comparable.
