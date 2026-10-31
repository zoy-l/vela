# Vela compiler

The compiler loads and checks Vela source, lowers it to Typed IR, and emits bytecode or native code.
Source tools, interactive sessions, and the command-line executable use the same frontend contracts.
See [Architecture and Contracts](../.agents/architecture.md) for dependency and ownership rules and
[the language architecture](../docs/en/architecture.md) for execution contracts.

## Directory responsibilities

| Directory | Responsibility |
| --- | --- |
| `frontend/syntax` | Tokens, lexing, parsing, AST, and AST cloning. |
| `frontend/modules` | Source loading, import binding, and declaration and symbol identities. |
| `frontend/builtins` | Builtin declarations, SDK modules, and the standard environment. |
| `frontend/generics` | Generic specialization and concrete declaration production. |
| `frontend/flow` | Capture analysis, initialization, and control-flow contracts. |
| `frontend/typing` | Type checking, structural inference, and checked semantic facts. |
| `ir` | Typed IR, lowering, analysis, optimization, and execution preparation. |
| `bytecode` | Bytecode compilation, register allocation, compilation cache, and native linking. |
| `native` | Shared native code generation and escape analysis. |
| `aot` | Ahead-of-time compilation, target support, and artifact publication. |
| `jit` | JIT compilation, executable code resources, and code caches. |
| `development` | Source sessions, development sessions, file tracking, and development REPL state. |
| `repl` | REPL source composition, identities, and bytecode execution sessions. |
| `project` | Project configuration loading and validation. |
| `cli` | Executable entry point, console input, standard streams, and interactive consoles. |
| `tooling/lsp` | Language server, JSON-RPC, and file URIs. |
| `tooling/semantic` | Shared semantic index, completion, hover, rename, and semantic tokens. |
| `tooling/format` | Source formatting. |
| `tooling/documentation` | Source and builtin documentation. |
| `tooling/mock` | Mock generation and its command. |
| `testing` | Source-test discovery, settings, execution, and reporting. |
| `sdk` | Embedded Vela SDK source. |
| `test_support` | Shared C++ test sources, compilation helpers, and checkpoint publication. |
| `language` | Language integration tests through compilation and execution. |

Shared frontend diagnostics remain in `frontend`; shared source text handling remains in `tooling`.
Module tests live beside the corresponding implementation. Build identity and its environment tests
remain at the compiler root.

## Build and validation

[`BUILD.gn`](BUILD.gn) defines the compiler targets centrally. Directory boundaries organize source
ownership; GN targets define dependencies and linkage. The executable remains `//compiler:vela`.
Frontend, IR, bytecode, native, AOT, JIT, development, and tooling consumers use the targets in this
file. Generated SDK sources and the cache build identity remain under the generated `compiler/`
directory.

Use the repository tools from the project root:

```powershell
python -m tools.vl build debug default
python -m tools.vl test debug all
python -m tools.vl lint
```

`vela check` loads imports, specializes declarations, and checks types. `vela run` lowers checked
source through Typed IR to validated bytecode and executes it in the VM. `vela build` emits a native
artifact through the shared native backend. See [Development Workflow](../.agents/development_workflow.md)
for build variants and validation and [the benchmark guide](../benchmarks/README.md) for measurements.
