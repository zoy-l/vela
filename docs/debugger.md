# Debugger

`vela debug FILE` compiles or loads the same validated bytecode used by `run`,
then pauses before the first instruction of zero-argument `main`. Script output
uses stdout; debugger prompts and inspection use stderr. A completed program
uses the ordinary script exit code. `quit` or EOF stops execution and exits `0`.

Commands:

| Command | Behavior |
| --- | --- |
| `continue`, `c` | Run to a breakpoint or completion |
| `step`, `s` | Stop at the next source line, entering Vela calls |
| `next`, `n` | Step over calls in the current frame |
| `finish`, `f` | Run until the current frame returns |
| `stepi`, `si` | Execute one bytecode instruction |
| `break [FILE:]LINE`, `b` | Set a source breakpoint; omitted FILE uses the selected frame |
| `break FUNCTION` | Break on entry to a named function or method |
| `breakpoints` | List breakpoint IDs and resolved locations |
| `delete ID`, `delete all` | Remove breakpoints |
| `stack`, `bt` | Show frames, PCs and call-site source positions |
| `frame DEPTH` | Select a frame for inspection; zero is the current frame |
| `registers`, `regs` | Show the selected frame's physical registers |
| `print rN` | Inspect one register without executing code |
| `field rN INDEX` | Read an object's field by its layout index |
| `element rN INDEX` | Read an array element |
| `help`, `h`; `quit`, `q` | Show commands; stop the program |

Full source paths and unique basenames are supported; ambiguous basenames require
a full path. Quote paths containing spaces. A requested line without an emitted
instruction moves to the next executable line in that file; the resolved location
is printed. Loop back edges can hit the same line again. Function breakpoints use
the names displayed in the stack. Stepping always controls the currently executing
frame, regardless of which caller is selected for inspection.

This debugger executes optimized bytecode. Eliminated statements have no stopping
location, and register inspection displays physical slots and temporaries rather
than source-local variable names. It does not evaluate arbitrary expressions,
modify variables, pause inside native C++ functions or interrupt a running continue
command asynchronously. Native calls are atomic from the instruction observer's
perspective. Console-input scripts share stdin with debugger commands.

## VM interface and lifetime

Embedding applications implement `vm::Debugger::BeforeInstruction` and call
`VirtualMachine::Debug(function, debugger)`. `DebugContext` and `DebugFrame` are
read-only borrowed views valid only during that callback. Depth zero is the current
frame; callers point at their call instruction, including while a nested call or
lazy global initializer is active. Missing symbols and out-of-range frame/register
requests are explicit optional results.

The VM's actual contiguous frames remain registered as precise GC roots while
paused. The observer may inspect values and allocate or collect through the same
isolate. A copied reference value retained beyond the callback needs the ordinary
host rooting API. Do not recursively execute the same VM from its observer.
The isolate, program and debugger must cover the call's lifetime.

`DebugAction::Stop` returns `DebuggerStopped` directly to the host. It is not a
language exception and does not run pending `finally` blocks. In-progress lazy
initializers return to `Uninitialized`; prior side effects are not rolled back.
Frames and temporary exception roots detach when execution ends. Continuing
through exceptions uses the normal typed catch/finally and unwinding machinery.

Ordinary execution instantiates an interpreter loop without the observer path;
it performs no per-instruction debugger check. Debug execution uses a separate
loop instantiation with the same instruction implementation.

## Verification

Compiler tests cover collection during every paused instruction, strings/arrays,
caller locations, exceptions/finally, cancellation of lazy initialization, loaded
images and invalid entries. Process tests exercise function and loop breakpoints,
step/next/finish, frame/register/field/element inspection and invalid commands.

The first Release sample after integration (three 0.1 s repetitions, wall-time
medians) measured ordinary VM execution at 17,365 ns for integer loops, 29,582 ns
for direct calls, 51,088 ns for closure calls, 24,928 ns for field access and
43,367 ns for allocation/GC. Raw data: `.cache/p2-debugger-hot-path.json`.
This sample does not establish a final performance guarantee; complete P2
regression and performance acceptance still run at the goal boundary.
