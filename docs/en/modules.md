# Modules

[**English**](modules.md) | [简体中文](../zh-CN/modules.md)


Every source file has a separate top-level scope. Functions, classes, enums,
interfaces, aliases, and module-level `let`/`const` bindings are private to that
file unless marked `export`. Class members are public by default, with `private`
restricting access to the declaring class, independently of whether the class is exported.

```typescript
// counters.vela
let current: int = 40;
export const step: int = 1;
export func next(): int {
  current = current + step;
  return current;
}
```

```typescript
// main.vela
import 'counters' as counter;
func main(): int {
  counter.next();
  return counter.next();
}
```

An import without `as` brings the dependency's exports into the importing
module's top-level scope. An import with `as` requires qualified access, such
as `counter.next()`. Classes, enum types, generic applications, and type aliases
also support qualification. Conflicting imports or top-level declarations
produce diagnostics; use prefixes to distinguish imports. Local variables and
parameters can shadow imported names or prefixes.

Imports are not implicit re-exports. A dependency cannot use the entry module's
private symbols, and importing a module does not expose its own dependencies.
Import prefixes are compile-time bindings, not runtime objects or values.
There is no `namespace` declaration block.

Files are loaded once by canonical path using a work queue. Cyclic imports can
link function declarations without recursively evaluating module bodies. Module
bindings and type names are resolved before generic specialization; calls and
field offsets use resolved native targets and layout metadata without module-name lookup at runtime.

Module globals reuse the program's rooted lazy initializer slots. They initialize
on demand and persist across executions and copies of the same program. A new
compilation gets separate state. A mutable exported `let` can be assigned through
an import, while assigning to a `const` is a compile error. Const prevents rebinding,
not mutation of the referenced object. Initializers use the defining module's
scope and cannot capture the local variables of a caller or another initializer's
lambda. Inference follows global dependencies; cyclic inference requires explicit
types, and a cycle during actual lazy initialization reports `CyclicInitialization`.

`CompileModules` and `CheckModules` accept named `SourceInput` entries and an
entry path. Relative imports resolve only among the supplied inputs. They do
not silently read the disk. `CheckFile` accepts content overlays for unsaved
editor buffers while retaining the file-backed dependency graph.

Use source files as compiler entry points. Import paths resolve relative to the
importing file. An extensionless specifier such as `import 'math';` resolves
directly to `math.vela`; an explicit extension remains valid. Resolution does not
probe an extensionless file or an `index.vela` directory entry. The loader,
compilation cache, editor navigation and import links share the same resolution.
Standard objects such as `Math` and `Error` are available without
imports; `std:` imports are no longer supported.

`import 'sdk:test';` loads the testing library from immutable source embedded in
the distribution. SDK modules follow ordinary export, privacy and prefix rules.
File and memory compilation, native object caches and language services share the
`sdk:test` identity; memory inputs cannot override it. Unknown `sdk:` modules
are errors and are never resolved as files. `builtin:` remains an internal
standard-environment identity. See the [testing contract](../zh-CN/testing.md).

