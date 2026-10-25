# Vela language architecture

[**English**](architecture.md) | [简体中文](../zh-CN/architecture.md)


Vela is a statically typed scripting language for native applications. The
first execution path is a compiled register bytecode VM. Native AOT is a later
backend of the same typed intermediate representation. JIT is deliberately
outside the first implementation boundary and must not shape the runtime API
before a benchmark demonstrates that it is needed.

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
interfaces, generics, Expected, matching, and ordered collections.

Control flow includes C/C++-style fallthrough switches. Explicit nullable types
use `T?` and flow-sensitive non-null checks. Scope, declaration order, and
source diagnostics are specified in [Basic language contracts](language_basics.md).

The initial class model is deliberately smaller than C++:

- `class` declares a nominal object layout with statically known fields and
  methods;
- classes are final; inheritance and class method overriding are later language
  work, while explicit interfaces already support runtime dispatch;
- a class construction expression has no `new` keyword;
- named construction arguments are resolved to `param` fields or explicit constructor parameter
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
    param brightness: Brightness;
}

class ActionSheetExample {
}

class CupertinoApp {
    param theme: CupertinoThemeData;
    param home: ActionSheetExample;
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
Strings and typed arrays use the same IR and bytecode path. Array element types
and rank remain exact across literals, fields, parameters, returns, indexing,
and assignment. Context supplies the type of empty array literals;
there are no implicit element conversions. Arrays have reference semantics and compact
contiguous native storage. Their length can grow or shrink through typed mutation
APIs while aliases retain the same object identity; see [growable arrays](arrays.md).
See [compiler details](../../compiler/README.md) for
current bytecode limits.

Generic expansion produces concrete declarations before static checking.
Resolved types retain structural element/signature/argument relationships;
lowering does not parse type-name suffixes to recover array elements. Interface
selectors, field offsets, and argument bindings are resolved before execution.
Validated layout tables are immutable and shared by all functions in a program.

The compiler must retain source locations through parsing, type checking, IR
instructions, and bytecode ranges. This keeps diagnostics and future debugger
mapping independent from the VM instruction encoding.

Type checking also records whether each statement can fall through, break, or
continue. Non-void functions cannot reach their closing brace; paths that loop
forever need no return. Unreachable statements are compile errors. Boolean
literals, `!`, `&&`, and `||` refine branch reachability; other conditions are
conservative, without variable-value propagation. Lowering consumes these facts
to place void returns and omit unreachable branch jumps, loop increments, and
back edges. Condition side effects are preserved even when the result is known.

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
- no heap allocation for ordinary integer, double, boolean, or handle moves;
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

Small object allocations use size-class free lists. VM registers and instance
fields use tagged `Value` slots; arrays already store native primitives or
pointers in type-specific contiguous buffers. Small arrays use inline storage;
larger arrays and growth use a separately accounted backing buffer while the GC
object stays at its original address. Array layouts identify reference elements;
marking scans only the live prefix, and reference writes apply the write barrier.
Immutable strings use one allocation
for the object header and bytes, participate in the same collector, and are
marked as leaves. Concatenation allocates the result once and copies both
operands directly; it remains linear in the total byte length. Class string
fields and native string-pointer arrays are traced precisely.

Compiled functions share a fixed-size, rooted native string constant pool.
Its owner registers the pool before allocating literals and releases its root
when its last owner is destroyed. `vm::Program` owns immutable, validated
function storage; copies share that storage. `vm::FunctionHandle` retains the
program, including its code and rooted constants. Raw `vm::Function` views are
borrowed loading inputs: `Program::Load` copies them, then validates the owned
snapshot. Execution does not repeat program validation. Layouts remain owned
by the Isolate, which must outlive all programs, handles, and root ranges.
Program destruction and root operations remain on the Isolate's thread.
Execution results are borrowed values: hosts must root returned objects or
strings before another allocation or collection. See
[VM execution contracts](vm_execution.md) for register allocation and limits.

Roots are revisited before the collector enters sweep because VM registers
can change between incremental steps. Root registration and reference stores
also preserve references during sweep. The collector is single-threaded per
isolate; a host thread must cross an isolate-owned root boundary before it can
retain a script object. Strings use valid UTF-8, with code point lengths,
indices, and slice boundaries, plus a separate byte length. Interpolation,
explicit conversions, join, and GC-owned builders use the same checked compiler
and VM path. See [string contracts](strings.md) for APIs, costs, and rooting.

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
