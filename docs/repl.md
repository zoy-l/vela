# Persistent REPL

Run `vela repl` from the directory used to resolve relative imports.
The shell uses normal `func`, strict static types, globals, classes and imports.
It uses the same string formatting and strict numeric rules as compiled scripts.

```text
> let value = 40;
> func add(value: int): int {
...   return value + 2;
... }
> add(value)
42
> value = value + 1;
> value
41
```

Declarations and values persist; only the current submission executes. New globals
initialize once when submitted. Compile errors commit nothing. Runtime errors keep
admitted declarations and side effects already performed; failed lazy initialization
can be retried. Duplicate declarations remain errors, as in ordinary source files.

Expressions without a trailing semicolon echo their non-void result once. A semicolon
suppresses echo. Unfinished blocks, parentheses and brackets prompt for continuation.
EOF with unfinished input reports an error. Prompts and diagnostics use stderr;
script output uses stdout. `readLine` consumes the same input stream as the shell.

`.help` lists commands, `.source` prints accepted input, `.reset` discards the entire
isolate and history, and `.quit` exits. Source diagnostics and runtime frames identify
the original submission as `<repl:N>` with its original line number.

Accepted module sources and canonical resolutions are pinned until
reset. Editing imported files affects a fresh session or a reset session. Failed
compilation does not pin new modules. Module prefixes, function/global/interface IDs,
selectors and runtime layout pointers remain stable as the session grows. Old closures,
mutable capture cells, interface method values and nested collections remain valid.

The current implementation reparses and checks accepted source before each submission;
it is not an incremental semantic compiler. Retired synthetic entries are checked but
their parent bodies are not lowered or executed. They share one reserved entry slot;
nested lambdas keep separate stable targets. Bytecode encoding limits still apply to
actual declarations and closures. A session does not retain prior program handles.
Successful replacement roots new globals before releasing old storage. Metadata
allocation rolls back on compile/load failure before any instance can escape.

`repl/submission/{0,32,128,512}` measures one submission after growing history, with
setup and collection outside the timed interval. Its heap counters cover GC objects
and allocator blocks, not AST/compiler metadata or process working set. Final P2
performance results are recorded in [P2 acceptance](p2_acceptance.md).

The persistence and echo choices follow [Node REPL](https://nodejs.org/api/repl.html)
and [Julia REPL](https://docs.julialang.org/en/v1/stdlib/REPL/); stable symbol identity
follows the separation used by [LLVM ORC sessions](https://llvm.org/docs/ORCv2.html).
