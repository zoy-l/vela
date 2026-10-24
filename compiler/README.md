# Vela compiler workspace

This directory contains the executable, typed IR lowering, and build identity
for the bootstrap compiler. The language and runtime contracts are recorded in
[`docs/language_architecture.md`](../docs/language_architecture.md); the
bootstrap executable remains intentionally smaller than that target.

The frontend, compiler, and VM stay under the same GN/Ninja target boundary so
later optimizer and native backend work does not change the developer entry
points:

- `//compiler:vela` is the user-facing executable.
- `//compiler:build_info` is the small shared build identity target.
- `//compiler:frontend` contains the lexer, parser, and static checker.
- `//runtime:runtime` contains the isolate-owned object heap and collector
  contract used by the VM.
- `//vm:vm` contains the fixed-width register VM bootstrap and frame root
  integration.
- `//compiler:vela_environment_tests` validates that the target links and its
  build identity is available to consumers.
- `//compiler:vela_frontend_tests` covers the first source-level contracts.
- `//runtime:runtime_tests` covers object reachability, cycles, write barriers,
  and size-class reuse.
- `//vm:vm_tests` covers typed arithmetic, bytecode validation, and register
  roots surviving collection.

The first frontend slice is now available through:

```powershell
.\out\debug\vela.exe check path\to\source.vela
```

The compiler executes integer, double, boolean, string, typed-array, and fixed-layout
class programs:

```powershell
.\out\debug\vela.exe run path\to\source.vela
```

`run` selects a zero-argument `main` function, lowers the checked AST through
Typed IR, emits register bytecode, and executes it in the VM. Function calls,
control-flow branches, loops, strings, and typed arrays are supported.

Array type names retain their element identity and rank (`int[]`, `Item[][]`).
The checker interns these names and records each array literal's checked type
against the AST. Lowering consumes those facts, propagates contextual element
types (including enum shorthand), and assigns compact array-layout IDs. Nested
element layouts precede their parent layouts. Bytecode `NewArray` encodes the
destination register, layout ID, and length in three 8-bit operands; lengths or
layout IDs above 255 produce a diagnostic. The existing per-function register
limit can constrain large literals earlier.

Arrays are invariant references: assignments alias their storage, and element
types must match exactly. An empty literal requires an expected array type;
nonempty literals can infer a homogeneous element type. Indices must be `int`.
There are no implicit conversions or array equality operators. The VM reports
`IndexOutOfBounds` for negative and out-of-range accesses.

`double` uses IEEE 754 binary64. Decimal and exponent literals (`1.5`, `1e2`,
`1.25e-1`) are parsed once, checked for representable range, and stored in a
per-function constant pool deduplicated by bit pattern. `LoadDouble` selects
the pool entry by an 8-bit index; a function can contain at most 256 distinct
double constants within its existing register limits. Dedicated double
instructions implement arithmetic, negation, and comparisons. Integer
instructions retain their existing overflow checks and truncating division.

Both operands of arithmetic and comparisons must have exactly the same type.
`let x: double = 1`, `1 + 1.0`, and `1 == 1.0` are compilation errors. Write `1.0`
or `1e0` for double literals; `num` and `float` are not built-in types. Double
division by positive or negative zero returns `DivisionByZero`; other operations
preserve IEEE infinity, NaN comparisons, subnormals, and signed zero. `print`
and CLI results share the shortest round-trip double formatting. `double[]`
elements occupy eight bytes and require no GC element scan.

It covers the initial C-style block syntax, enum declarations, class fields,
aggregate construction with named arguments, functions, and static type
checking. The checker feeds the typed IR and bytecode VM; it does not interpret
source directly.

Benchmarks use a separate Google Benchmark executable:

```powershell
python -m tools.vl bench -- --benchmark_filter=integer_loop
```

For the complete set, use `python -m tools.vl bench`. Compilation, first execution,
and steady-state VM execution have separate benchmark cases. See
[`benchmarks/README.md`](../benchmarks/README.md) for measurement boundaries.
